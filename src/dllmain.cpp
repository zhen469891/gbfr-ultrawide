// GBFRUltrawide - ultrawide fix for Granblue Fantasy Relink v2.0.2
// Derived from Lyall's GBFRelinkFix (https://github.com/Lyall/GBFRelinkFix), MIT License.
// Changes vs upstream: spdlog removed (minimal built-in logger), renamed to GBFRUltrawide,
// explicit HIT/MISS logging for every pattern scan, nullptr-safe scan handling.

// === PATTERN STATUS (game v2.0.2) ===
// Patterns are verbatim from Lyall's GBFRelinkFix v1.1.1 (written for game v1.x).
// Status below reflects Zydis instruction-level disassembly verification against
// game v2.0.2. Dead patterns are kept as-is on purpose and will be updated one by
// one; a MISS only disables that feature.
//
// IMPORTANT - v2.0.2 UI object struct shifted -0x38 vs v1:
//   width 0x1F4 -> 0x1BC | height 0x1F8 -> 0x1C0 | object ID 0x1FC -> 0x1C4
//   offsX 0x1CC -> 0x194 | offsY  0x1D0 -> 0x198 | our marker 0x200 -> 0x1C8
//
// Alive on v2.0.2:
//   [OK]    Resolution      (ApplyResolution)  - verified; 2 hits (two inlined copies), BOTH hooked at +0x25
//                                                (diagnostic: runtime showed hooks install but 21:9 not applied,
//                                                so we no longer assume the first copy is the live path)
//   [OK]    ScreenEffects   (GraphicalFixes)   - verified; hook at +0xB
//   [FIXED] UIBackgrounds   (HUDFix)           - base at +0x2F; >16:9 hook (base+0x0) now writes xmm1 (v2 loads width
//                                                into xmm1; xmm0 is overwritten right after), <16:9 hook moved
//                                                base+0x28 -> base+0x29 (v1 offset landed inside an 8-byte vmovss);
//                                                lambda struct offsets shifted -0x38
//   [REWORKED 2026-07-10] HUDConstraints (HUDFix) - hook at +0x1C unchanged (rcx = child element, rax = parent
//                                                canvas, xmm2/xmm0 = parent w/h), but the v1 ID-gated body was
//                                                inert on v2 (v1 object IDs never appear; +0x194/+0x198 are
//                                                normalized anchors, not pixel offsets). Body replaced with the
//                                                three-layer menu/story filter + full-canvas register widen +
//                                                Combat Prompts recenter, derived from independent analysis of
//                                                the v2.0.2 exe (build 0x6A3E573A) and cross-validated against
//                                                community ultrawide research. See HUDFix() for details.
//   [FIXED] ShadowQuality   (GraphicalTweaks)  - hook moved scan+0x0 -> scan-0x1 (v2 adds REX.X prefix 0x42, pattern
//                                                hits mid-instruction); addressing now rax+r8 (was rcx+rdx)
//   [OK]    TemporalAA      (GraphicalTweaks)  - verified; byte patch, not a hook
//   [FIXED] CutsceneFOV     (AspectFOVFix)     - hook moved +0xC -> +0x1C (v2 loads xmm2 at +0x14; patching earlier
//                                                gets overwritten); 4 identical inline hits, first one hooked
//   [REFOUND v2.0.2] AspectRatio (AspectFOVFix) - new pattern, only byte[2] 49->48 vs v1 (reg alloc change);
//                                                hook at +0x11 unchanged; verified unique hit RVA 0x00751089
//   [REFOUND v2.0.2] UIAspect    (HUDFix)       - v1 single pattern replaced by two patterns/sites: patch site
//                                                RVA 0x00231BDD (P1+0xC: 0F 85 -> 90 E9) + hook site RVA
//                                                0x00231F10 (P2+0xA); both must hit or the fix is skipped
//   [REWORKED v2.0.2] UIMarkers  (HUDFix)       - v1 site (RVA 0x026812CD) is a DEAD corner-widget path in v2:
//                                                HIT but never FIRED in combat (diagnosed 2026-07-10). The real
//                                                world->screen widget math is inlined 51x and reads the GLOBAL
//                                                canvas manager [0x07C02358]; its positioning assumes
//                                                canvasW*scale == windowW, an invariant the CanvasFitHeight
//                                                patch broke (=> uniform +440px right shift of health bars /
//                                                damage numbers / lock-on at 3440x1440). Fix "UIMarkersCanvas":
//                                                hook the CanvasFitHeight scale-store site at +0x40 (rax =
//                                                canvas manager) and write BOTH source W/H (+0x1B4/+0x1B8)
//                                                and current W/H (+0x1BC/+0x1C0) = 2160*aspect (>16:9) or
//                                                3840/aspect (<16:9). +0x1BC alone gets washed back to 3840
//                                                by the dirty-layout recalc (RVA 0x0261C5D0), which copies
//                                                +0x1B4 over it every dirty frame; writing the source field
//                                                makes the recalc propagate our value instead. (ADR-0006)
//   [REFOUND v2.0.2] GfxCorruption1 (GraphicalFixes) - new pattern, 4 hits (branch arms of one function) and ALL
//                                                are hooked at +0x8 (was: first hit @ +0x0). Hooked value is
//                                                width/64, not width/32 as v1 assumed; Cygames' v2.0.2 code
//                                                already ceils the w/32 & h/30 lanes itself (new vroundss),
//                                                only lane0 (w/64) is still stored raw - our hook fixes it
//   [REFOUND v2.0.2] GfxCorruption2 (GraphicalFixes) - new pattern, 2 hits (alloc success/fallback arms), both
//                                                hooked at +0x8; same lane0 = width/64 fix as GfxCorruption1
//   [REMOVED 2026-07-10] GameplayCamera (AspectFOVFix) - the v2-relocated message-block site (unique hit RVA
//                                                0x009D8C70) HIT but its distance hook never FIRED in any
//                                                session: v2.0.2 gameplay no longer drives the camera through
//                                                that message path. Hook deleted (it would double-multiply the
//                                                distance vs the CamDist* families below if the path ever
//                                                revives). Historical notes - incl. the "v1 FOV slot is now
//                                                pitch, hooking it tilts the camera" trap - kept in
//                                                docs/PATTERNS.md 3.6.
//   [NEW 2026-07-10] ProjMatrixFOV (AspectFOVFix) - THE rendered gameplay-FOV multiplier (ADR-0011): mid-hook
//                                                inside the projection-matrix builder (expected RVA 0x00750970,
//                                                hook at pattern+0x1A = the tanf call; xmm0 = FOV/2 at entry):
//                                                xmm0 *= fFOVMulti before tan(). Serves EVERY camera incl.
//                                                cutscenes - the multiplier is global by design. Register-only;
//                                                [obj+0x9D4] is never written (14+ writers, compounding hazard).
//   [ROLE CHANGE 2026-07-10] ViewParamsFOV (AspectFOVFix) - does NOT affect the rendered projection: its xmm3
//                                                only feeds the culling/shared-view-constants call (0x0216ABE0);
//                                                the builder above re-reads FOV from memory. KEPT (same site,
//                                                RVA 0x0075109A, hook +0x20, xmm3 *= fFOVMulti) so the culling
//                                                path sees the same multiplied FOV as ProjMatrixFOV - without
//                                                it, multipliers > 1 pop objects at the screen edges. ADR-0011.
//   [NEW 2026-07-10] CamDistPreset / FollowCamDist / RoamCamDist (AspectFOVFix) - camera distance multiplier,
//                                                three live hook families replacing the dead GameplayCamera
//                                                site: preset publish to the global camera-params block
//                                                (4 hits, ALL hooked at +0x5, xmm0, meters), follow-cam zoom
//                                                track (unique hit, +0x23, xmm8, normalized 0..1), free-roam
//                                                config copy (unique hit, +0x5, xmm0). Derived from community
//                                                ultrawide research for this exe build, re-verified offline.
//   [NEW 2026-07-10] Nameplate    (NameplateFix) - world-anchored nameplate horizontal scale (expected RVA
//                                                0x00847F6B, hook at pattern+0x3C right after "vdivss
//                                                xmm1,xmm7,[rax+0x9D0]"): xmm1 = xmm7 / live fAspectRatio.
//                                                Derived from independent analysis of the v2.0.2 exe.
//   [REFOUND v2.0.2] LODDistance (GraphicalTweaks) - v1 site compiled away; semantically relocated to the
//                                                per-object LOD threshold loop (unique hit RVA 0x020E56C0),
//                                                hook +0x0 -> +0x8 (after the 8-byte vdivss), xmm1 -> xmm2.
//                                                Confidence: MEDIUM - needs in-game verification; fallback
//                                                candidate RVA 0x0322ADDE (see hunt_lod_fpscap.txt)
//   [REFOUND v2.0.2] FPSCap      (FPSCap)          - v2 limiter loads frame time (double) from 3-entry table
//                                                (1/30, 1/60, 1/120 @ RVA 0x054D6BF0) into xmm10 then spins on
//                                                vucomisd+pause. New pattern @ RVA 0x001B6E63; hook +0x5 -> +0xC
//                                                (must be after the vmovsd), xmm6 -> xmm10
//
// MISS on v2.0.2: none - all v1 patterns relocated or replaced (GameplayCamera deleted as a dead
// site), plus the v2-native patterns ViewParamsFOV, ProjMatrixFOV, Nameplate, CamDistPreset,
// FollowCamDist and RoamCamDist. The gameplay FOV multiplier is served by ProjMatrixFOV (with
// ViewParamsFOV keeping culling consistent); the camera distance multiplier by the three CamDist*
// families; only the <16:9 vert- FOV compensation remains unported.
// =====================================

#include "stdafx.h"
#include "helper.hpp"
#include <inipp/inipp.h>
#include <safetyhook.hpp>

HMODULE baseModule = GetModuleHandle(NULL);

// Logger and config setup
inipp::Ini<char> ini;
std::string sFixName = "GBFRUltrawide";
// Stamped at build time: CI passes the release tag via CMake (GBFR_VERSION);
// local builds fall back to 0.0.0-dev, marking unofficial binaries in user logs.
#ifndef GBFR_VERSION_STRING
#define GBFR_VERSION_STRING "0.0.0-dev"
#endif
std::string sFixVer = GBFR_VERSION_STRING;
std::string sLogFile = "GBFRUltrawide.log";
std::string sConfigFile = "GBFRUltrawide.ini";
std::string sExeName;
filesystem::path sExePath;
std::pair DesktopDimensions = { 0,0 };

// Ini Variables
int iInjectionDelay;
bool bCustomResolution;
int iCustomResX;
int iCustomResY;
float fFOVMulti;
float fCamDistMulti;
bool bHUDFix;
bool bSpanHUD;
float fHUDAspectRatio;
bool bSpanAllHUD;
bool bSpanAllBackgrounds;
#ifdef GBFR_DEVBUILD
// [Debug - Span HUD] per-element position overrides + probe diagnostic, and the
// [Debug - Backgrounds] probe (dev builds only - release builds compile all of this
// out and ignore both ini sections).
// Fixed-capacity storage, filled once in ReadConfig before any hook is installed -
// the hooks read them lock-free.
bool bHUDProbe;
bool bBackgroundProbe;
// [Debug - Scene] CropFactorOverride (dev builds only): diagnostic lever left over
// from the flash-bars hunt (ADR-0012). The experiment it was built for is CONCLUDED:
// the view CB's +0x59C factor is consumed by both the scene pass and full-screen
// combat-VFX quads, and the shipping value at any non-16:9 aspect is now
// fAspectMultiplier (the scene renders identically under 1.0 and 1.34375 - the old
// "it zooms the scene" reading was confounded). When >= 0 and the screen is wider
// than 16:9, the ScreenEffects hook writes THIS value instead of the shipping
// fAspectMultiplier. Negative = disabled.
float fCropFactorOverride = -1.0f;
// [Debug - Scene] WindowRefOverride (dev builds only): second experiment instrument
// built for the flash-bars hunt. The hunt CONCLUDED via the crop factor (ADR-0012)
// before this lever was ever needed in-game; kept as a diagnostic. Target: the view CB
// PRODUCER (RVA 0x020D0E20; docs/PATTERNS.md 2.7/3.23) fills the window-reference pair
//   CB +0x594 = (float)windowRefW   (int global 0x06B84098)
//   CB +0x598 = (float)windowRefH   (int global 0x06B8409C)
// Theory (MEDIUM): a shader sizes the full-screen combat-flash quad as
// renderH * (CB+0x594 / CB+0x598); if the pair holds 1920/1080 the quad is 16:9 ->
// vertical bars exactly at the 16:9 boundaries at 3440x1440.
// OPEN CONTRADICTION the instrument settles: the runtime DIAG dump shows the globals
// already patched to 3440/1440 (ResPublish) - the ratio would already be 2.389, killing
// the theory, UNLESS the producer runs before the patch or reads another source. The
// hook's one-shot FIRED line therefore prints the INCOMING width before overriding.
// When true and aspect > 16:9, a mid-hook forces CB +0x594 = windowRefH * fAspectRatio
// (register-only). Diagnostic, NOT a shipping fix.
bool bWindowRefOverride = false;
// Resolved at install time from the WindowRefOverride site's second vcvtsi2ss disp32:
// the int global the game stores to CB +0x598 right after our hooked width store. The
// hook re-reads it every fire so the override stays consistent with whatever +0x598
// actually receives that frame (1080 pre-ResPublish vs 1440 after).
volatile int* g_pWindowRefHGlobal = nullptr;
constexpr int kMaxMoveEntries = 32;
uint32_t uEdgeSnapIds[kMaxMoveEntries];
int iEdgeSnapCount = 0;
uint32_t uMoveIds[kMaxMoveEntries];
float fMoveDeltas[kMaxMoveEntries];
int iMoveIdCount = 0;
// DIAG-CAMDIST instrumentation (dev builds only): most recent camera/view-params
// object pointer seen by the AspectRatio / ProjMatrixAspect hooks (the struct with
// +0x9D0 aspect / +0x9D4 FOV). Sampled every ~2s by DiagCamDistWatchdog (bottom of
// this file); relaxed atomic - the watchdog only wants "a recent camera", not a
// synchronized snapshot.
std::atomic<uintptr_t> g_DiagCamObj{ 0 };
#endif // GBFR_DEVBUILD
bool bAspectFix;
bool bFOVFix;
bool bShadowQuality;
int iShadowQuality;
float fLODMulti;
bool bDisableTAA;
bool bFPSCap;
// Default true on purpose: existing user configs predate the [Fix Nameplates] section,
// and inipp::get_value leaves the variable untouched when the key is missing.
bool bFixNameplates = true;

// Aspect ratio + HUD stuff
float fNativeAspect = (float)16 / 9;
float fAspectRatio;
float fAspectMultiplier;
float fHUDWidth;
float fHUDHeight;
float fDefaultHUDWidth = (float)1920;
float fDefaultHUDHeight = (float)1080;
float fHUDWidthOffset;
float fHUDHeightOffset;

// Nameplate Fix: the game's own HUD-projection scale global (store target of the
// Nameplate pattern's "vmovss [rip+disp32],xmm1" at +0x3C; game global RVA ~0x07194FCC).
// Resolved at scan time in NameplateFix() from the instruction's rip-relative disp32 -
// NEVER hardcoded. Refreshed to 1/fAspectRatio each HUDConstraints pass so the value
// stays corrected on frames where the Nameplate site itself does not run.
float* g_pNameplateScalar = nullptr;

// Minimal thread-safe logger (spdlog replacement)
namespace Log
{
    static std::mutex mtx;
    static std::ofstream file;
    static bool bConsoleFallback = false;

    static void Open(const std::filesystem::path& path)
    {
        std::lock_guard<std::mutex> lock(mtx);
        file.open(path, std::ios::out | std::ios::trunc);
        if (!file.is_open())
        {
            AllocConsole();
            FILE* dummy;
            freopen_s(&dummy, "CONOUT$", "w", stdout);
            bConsoleFallback = true;
            std::cout << "Log initialisation failed: could not open " << path.string() << std::endl;
        }
    }

    static void WriteV(const char* level, const char* fmt, va_list args)
    {
        char msg[1024];
        vsnprintf(msg, sizeof(msg), fmt, args);

        SYSTEMTIME st;
        GetLocalTime(&st);
        char line[1200];
        snprintf(line, sizeof(line), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%s] %s\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, level, msg);

        std::lock_guard<std::mutex> lock(mtx);
        if (file.is_open())
        {
            file << line;
            file.flush();
        }
        else if (bConsoleFallback)
        {
            std::cout << line;
        }
    }
}

static void LogInfo(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    Log::WriteV("info", fmt, args);
    va_end(args);
}

static void LogWarn(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    Log::WriteV("warning", fmt, args);
    va_end(args);
}

static void LogError(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    Log::WriteV("error", fmt, args);
    va_end(args);
}

// Module-relative offset helper for HIT logging
static unsigned long long ModOffset(const uint8_t* address)
{
    return (unsigned long long)((uintptr_t)address - (uintptr_t)baseModule);
}

void Logging()
{
    // Get game name and exe path
    WCHAR exePath[_MAX_PATH] = { 0 };
    GetModuleFileNameW(baseModule, exePath, MAX_PATH);
    sExePath = exePath;
    sExeName = sExePath.filename().string();
    sExePath = sExePath.remove_filename();

    // Logger initialisation
    {
        std::error_code ec;
        std::filesystem::create_directories(sExePath / "scripts", ec); // best effort
        Log::Open(sExePath / "scripts" / sLogFile);

        LogInfo("----------");
#ifdef GBFR_DEVBUILD
        LogInfo("%s v%s loaded. (dev)", sFixName.c_str(), sFixVer.c_str());
        LogInfo("Development build: [Debug - Span HUD] (Probe / EdgeSnapIds / MoveIds), [Debug - Backgrounds] (Probe) and [Debug - Scene] (CropFactorOverride / WindowRefOverride) sections are active, read from scripts\\GBFRUltrawide.dev.ini.");
#else
        LogInfo("%s v%s loaded.", sFixName.c_str(), sFixVer.c_str());
#endif
        LogInfo("----------");
        LogInfo("Path to logfile: %s", (sExePath.string() + "scripts\\" + sLogFile).c_str());
        LogInfo("----------");

        // Log module details
        LogInfo("Module Name: %s", sExeName.c_str());
        LogInfo("Module Path: %s", sExePath.string().c_str());
        LogInfo("Module Address: 0x%llx", (unsigned long long)(uintptr_t)baseModule);
        LogInfo("Module Timestamp: %u", Memory::ModuleTimestamp(baseModule));
        LogInfo("----------");
    }
}

void ReadConfig()
{
    // Initialise config
    std::ifstream iniFile("scripts\\" + sConfigFile);
    if (!iniFile)
    {
        LogError("Failed to load config file.");
        LogInfo("Trying alternate path.");

        std::ifstream iniFile(sConfigFile);
        if (!iniFile)
        {
            LogError("Make sure %s is present in the game folder.", sConfigFile.c_str());
        }
        else
        {
            LogInfo("Path to config file: %s", (sExePath.string() + sConfigFile).c_str());
            ini.parse(iniFile);
        }
    }
    else
    {
        LogInfo("Path to config file: %s", (sExePath.string() + "scripts\\" + sConfigFile).c_str());
        ini.parse(iniFile);
    }

    // Read ini file
    inipp::get_value(ini.sections["GBFRelinkFix Parameters"], "InjectionDelay", iInjectionDelay);
    inipp::get_value(ini.sections["Custom Resolution"], "Enabled", bCustomResolution);
    inipp::get_value(ini.sections["Custom Resolution"], "Width", iCustomResX);
    inipp::get_value(ini.sections["Custom Resolution"], "Height", iCustomResY);
    inipp::get_value(ini.sections["Gameplay FOV"], "Multiplier", fFOVMulti);
    inipp::get_value(ini.sections["Gameplay Camera Distance"], "Multiplier", fCamDistMulti);
    inipp::get_value(ini.sections["Fix HUD"], "Enabled", bHUDFix);
    inipp::get_value(ini.sections["Span HUD"], "Enabled", bSpanHUD);
    inipp::get_value(ini.sections["Span HUD"], "AspectRatio", fHUDAspectRatio);
    inipp::get_value(ini.sections["Span HUD"], "SpanAllHUD", bSpanAllHUD);
    inipp::get_value(ini.sections["Span HUD"], "SpanAllBackgrounds", bSpanAllBackgrounds);
#ifdef GBFR_DEVBUILD
    // Dev-only configuration lives in a separate file so the shipped GBFRUltrawide.ini
    // stays clean: the [Debug - *] sections are read from GBFRUltrawide.dev.ini next to
    // the main ini (release builds compile this out and never reference the file).
    // Missing/unreadable file = all debug keys keep their off/empty defaults.
    inipp::Ini<char> devIni;
    std::string sDevConfigFile = "GBFRUltrawide.dev.ini";
    {
        std::ifstream devIniFile("scripts\\" + sDevConfigFile);
        if (devIniFile)
        {
            LogInfo("Path to dev config file: %s", (sExePath.string() + "scripts\\" + sDevConfigFile).c_str());
            devIni.parse(devIniFile);
        }
        else
        {
            // Same alternate-path fallback as the main ini (game root).
            std::ifstream devIniFileAlt(sDevConfigFile);
            if (devIniFileAlt)
            {
                LogInfo("Path to dev config file: %s", (sExePath.string() + sDevConfigFile).c_str());
                devIni.parse(devIniFileAlt);
            }
            else
            {
                LogInfo("Dev config: GBFRUltrawide.dev.ini not found - debug features off");
            }
        }
    }
    inipp::get_value(devIni.sections["Debug - Span HUD"], "Probe", bHUDProbe);
    inipp::get_value(devIni.sections["Debug - Backgrounds"], "Probe", bBackgroundProbe);
    inipp::get_value(devIni.sections["Debug - Scene"], "CropFactorOverride", fCropFactorOverride);
    inipp::get_value(devIni.sections["Debug - Scene"], "WindowRefOverride", bWindowRefOverride);
    std::string sEdgeSnapIds;
    std::string sMoveIds;
    inipp::get_value(devIni.sections["Debug - Span HUD"], "EdgeSnapIds", sEdgeSnapIds);
    inipp::get_value(devIni.sections["Debug - Span HUD"], "MoveIds", sMoveIds);
#endif // GBFR_DEVBUILD
    inipp::get_value(ini.sections["Fix Aspect Ratio"], "Enabled", bAspectFix);
    inipp::get_value(ini.sections["Fix FOV"], "Enabled", bFOVFix);
    inipp::get_value(ini.sections["Shadow Quality"], "Enabled", bShadowQuality);
    inipp::get_value(ini.sections["Shadow Quality"], "Value", iShadowQuality);
    inipp::get_value(ini.sections["Level of Detail"], "Multiplier", fLODMulti);
    inipp::get_value(ini.sections["Disable TAA"], "Enabled", bDisableTAA);
    inipp::get_value(ini.sections["Raise Framerate Cap"], "Enabled", bFPSCap);
    inipp::get_value(ini.sections["Fix Nameplates"], "Enabled", bFixNameplates);

    // Log config parse
    LogInfo("Config Parse: iInjectionDelay: %dms", iInjectionDelay);
    LogInfo("Config Parse: bCustomResolution: %s", bCustomResolution ? "true" : "false");
    LogInfo("Config Parse: iCustomResX: %d", iCustomResX);
    LogInfo("Config Parse: iCustomResY: %d", iCustomResY);
    LogInfo("Config Parse: fFOVMulti: %g", fFOVMulti);
    if (fFOVMulti < (float)0.1 || fFOVMulti > (float)2.5)
    {
        fFOVMulti = std::clamp(fFOVMulti, (float)0.1, (float)2.5);
        LogInfo("Config Parse: fFOVMulti value invalid, clamped to %g", fFOVMulti);
    }
    LogInfo("Config Parse: fCamDistMulti: %g", fCamDistMulti);
    if (fCamDistMulti < (float)0.1 || fCamDistMulti >(float)2.5)
    {
        fCamDistMulti = std::clamp(fCamDistMulti, (float)0.1, (float)2.5);
        LogInfo("Config Parse: fCamDistMulti value invalid, clamped to %g", fCamDistMulti);
    }
    LogInfo("Config Parse: bHUDFix: %s", bHUDFix ? "true" : "false");
    LogInfo("Config Parse: bSpanHUD: %s", bSpanHUD ? "true" : "false");
    LogInfo("Config Parse: fHUDAspectRatio: %g", fHUDAspectRatio);
    LogInfo("Config Parse: bSpanAllHUD: %s", bSpanAllHUD ? "true" : "false");
    LogInfo("Config Parse: bSpanAllBackgrounds: %s", bSpanAllBackgrounds ? "true" : "false");
#ifdef GBFR_DEVBUILD
    LogInfo("Config Parse: bHUDProbe: %s", bHUDProbe ? "true" : "false");
    LogInfo("Config Parse: bBackgroundProbe: %s", bBackgroundProbe ? "true" : "false");
    LogInfo("Config Parse: fCropFactorOverride: %g", fCropFactorOverride);
    if (fCropFactorOverride >= 0.0f)
        LogInfo("Config Parse: [Debug - Scene] CropFactorOverride ACTIVE: ScreenEffects will write %g to the view CB +0x59C factor at >16:9 instead of the shipping fAspectMultiplier - diagnostic only (ADR-0012)", fCropFactorOverride);
    LogInfo("Config Parse: bWindowRefOverride: %s", bWindowRefOverride ? "true" : "false");
    if (bWindowRefOverride)
        LogInfo("Config Parse: [Debug - Scene] WindowRefOverride ACTIVE: view CB +0x594 (windowRef W) will be forced to windowRefH * aspect at >16:9 - flash-bars experiment #2; the first FIRED line prints the incoming value");

    // EdgeSnapIds = 123,456 - comma-separated element ids. Any non-digit run acts as
    // a separator; entries beyond the fixed capacity are dropped with a warning.
    {
        const char* p = sEdgeSnapIds.c_str();
        while (*p)
        {
            if (iEdgeSnapCount >= kMaxMoveEntries)
            {
                LogWarn("Config Parse: EdgeSnapIds: more than %d ids, extra entries ignored", kMaxMoveEntries);
                break;
            }
            char* end = nullptr;
            unsigned long v = strtoul(p, &end, 10);
            if (end == p) { ++p; continue; }
            uEdgeSnapIds[iEdgeSnapCount++] = (uint32_t)v;
            p = end;
        }
        LogInfo("Config Parse: EdgeSnapIds: %d id(s)", iEdgeSnapCount);
        for (int i = 0; i < iEdgeSnapCount; ++i)
            LogInfo("Config Parse: EdgeSnapIds[%d] = %u", i, uEdgeSnapIds[i]);
    }

    // MoveIds = 123:660,456:-660 - comma-separated id:deltaX pairs (delta in canvas
    // units, 3840x2160 space). Malformed pairs are skipped with a warning.
    {
        const char* p = sMoveIds.c_str();
        while (*p)
        {
            if (iMoveIdCount >= kMaxMoveEntries)
            {
                LogWarn("Config Parse: MoveIds: more than %d pairs, extra entries ignored", kMaxMoveEntries);
                break;
            }
            char* end = nullptr;
            unsigned long id = strtoul(p, &end, 10);
            if (end == p) { ++p; continue; }
            p = end;
            while (*p == ' ' || *p == '\t') ++p;
            if (*p != ':')
            {
                LogWarn("Config Parse: MoveIds: id %lu has no ':deltaX' - entry skipped", id);
                while (*p && *p != ',') ++p;
                continue;
            }
            ++p;
            float delta = strtof(p, &end);
            if (end == p)
            {
                LogWarn("Config Parse: MoveIds: id %lu has an unreadable deltaX - entry skipped", id);
                while (*p && *p != ',') ++p;
                continue;
            }
            p = end;
            uMoveIds[iMoveIdCount] = (uint32_t)id;
            fMoveDeltas[iMoveIdCount] = delta;
            ++iMoveIdCount;
        }
        LogInfo("Config Parse: MoveIds: %d pair(s)", iMoveIdCount);
        for (int i = 0; i < iMoveIdCount; ++i)
            LogInfo("Config Parse: MoveIds[%d] = %u -> %+g", i, uMoveIds[i], fMoveDeltas[i]);
    }
#endif // GBFR_DEVBUILD
    LogInfo("Config Parse: bAspectFix: %s", bAspectFix ? "true" : "false");
    LogInfo("Config Parse: bFOVFix: %s", bFOVFix ? "true" : "false");
    LogInfo("Config Parse: bShadowQuality: %s", bShadowQuality ? "true" : "false");
    LogInfo("Config Parse: iShadowQuality: %d", iShadowQuality);
    if (iShadowQuality < 256 || iShadowQuality > 8192)
    {
        iShadowQuality = std::clamp(iShadowQuality, 128, 16384);
        LogInfo("Config Parse: iShadowQuality value invalid, clamped to %d", iShadowQuality);
    }
    LogInfo("Config Parse: fLODMulti: %g", fLODMulti);
    if (fLODMulti < (float)0.1 || fLODMulti >(float)10)
    {
        fLODMulti = std::clamp(fLODMulti, (float)0.1, (float)10);
        LogInfo("Config Parse: fLODMulti value invalid, clamped to %g", fLODMulti);
    }
    LogInfo("Config Parse: bDisableTAA: %s", bDisableTAA ? "true" : "false");
    LogInfo("Config Parse: bFPSCap: %s", bFPSCap ? "true" : "false");
    LogInfo("Config Parse: bFixNameplates: %s", bFixNameplates ? "true" : "false");

    // Calculate aspect ratio / use desktop res instead
    DesktopDimensions = Util::GetPhysicalDesktopDimensions();

    if (iCustomResX > 0 && iCustomResY > 0)
    {
        fAspectRatio = (float)iCustomResX / (float)iCustomResY;
    }
    else
    {
        iCustomResX = (int)DesktopDimensions.first;
        iCustomResY = (int)DesktopDimensions.second;
        fAspectRatio = (float)DesktopDimensions.first / (float)DesktopDimensions.second;
        LogInfo("Custom Resolution: iCustomResX: Desktop Width: %d", iCustomResX);
        LogInfo("Custom Resolution: iCustomResY: Desktop Height: %d", iCustomResY);
    }
    fAspectMultiplier = fAspectRatio / fNativeAspect;

    // HUD variables
    fHUDWidth = iCustomResY * fNativeAspect;
    fHUDHeight = (float)iCustomResY;
    fHUDWidthOffset = (float)(iCustomResX - fHUDWidth) / 2;
    fHUDHeightOffset = 0;
    if (fAspectRatio < fNativeAspect)
    {
        fHUDWidth = (float)iCustomResX;
        fHUDHeight = (float)iCustomResX / fNativeAspect;
        fHUDWidthOffset = 0;
        fHUDHeightOffset = (float)(iCustomResY - fHUDHeight) / 2;
    }

    if (fHUDAspectRatio == (float)0)
    {
        fHUDAspectRatio = fAspectRatio;
        LogInfo("Config Parse: fHUDAspectRatio = 0, set to %g", fHUDAspectRatio);
    }
    LogInfo("----------");

    // Log aspect ratio stuff
    LogInfo("Custom Resolution: fAspectRatio: %g", fAspectRatio);
    LogInfo("Custom Resolution: fAspectMultiplier: %g", fAspectMultiplier);
    LogInfo("Custom Resolution: fHUDWidth: %g", fHUDWidth);
    LogInfo("Custom Resolution: fHUDHeight: %g", fHUDHeight);
    LogInfo("Custom Resolution: fHUDWidthOffset: %g", fHUDWidthOffset);
    LogInfo("Custom Resolution: fHUDHeightOffset: %g", fHUDHeightOffset);
    LogInfo("----------");
}

void ApplyResolution()
{
    if (bCustomResolution)
    {
        // [OK v2.0.2] Resolution: 2 hits (exe+0x215BFE and exe+0x21B277), two inlined copies
        // of the same apply logic. Previously only the first hit was hooked; runtime testing
        // showed the hooks install but 21:9 does not take effect, so to rule out the game
        // running the OTHER copy, ALL hits are now hooked at +0x25 with the same override.
        // safetyhook mid-hooks only accept capture-less lambdas, so the two known hits get
        // distinct lambdas to tell their FIRED diagnostics apart.
        std::vector<uint8_t*> ResolutionScanResults = Memory::PatternScanAll(baseModule, "41 ?? ?? ?? 3C 04 B9 04 00 00 00 0F ?? ?? 0F ?? ??");
        if (!ResolutionScanResults.empty())
        {
            LogInfo("HIT: Resolution: %zu hit(s)", ResolutionScanResults.size());
            for (size_t i = 0; i < ResolutionScanResults.size(); ++i)
            {
                LogInfo("HIT: Resolution hit #%zu: %s+0x%llx", i + 1, sExeName.c_str(), ModOffset(ResolutionScanResults[i]));
            }

            static std::vector<SafetyHookMid> ResolutionMidHooks;
            ResolutionMidHooks.push_back(safetyhook::create_mid(ResolutionScanResults[0] + 0x25,
                [](SafetyHookContext& ctx)
                {
                    static std::atomic<bool> logged{ false };
                    if (!logged.exchange(true)) LogInfo("FIRED: Resolution (hit #1), game wanted %ux%u", (uint32_t)ctx.rcx, (uint32_t)ctx.rax);
                    ctx.rcx = iCustomResX;
                    ctx.rax = iCustomResY;
                }));
            if (ResolutionScanResults.size() >= 2)
            {
                ResolutionMidHooks.push_back(safetyhook::create_mid(ResolutionScanResults[1] + 0x25,
                    [](SafetyHookContext& ctx)
                    {
                        static std::atomic<bool> logged{ false };
                        if (!logged.exchange(true)) LogInfo("FIRED: Resolution (hit #2), game wanted %ux%u", (uint32_t)ctx.rcx, (uint32_t)ctx.rax);
                        ctx.rcx = iCustomResX;
                        ctx.rax = iCustomResY;
                    }));
            }
            for (size_t i = 2; i < ResolutionScanResults.size(); ++i)
            {
                // Unexpected extra hits (pattern currently has exactly 2 on v2.0.2) - still
                // hook them identically; they share one FIRED marker.
                ResolutionMidHooks.push_back(safetyhook::create_mid(ResolutionScanResults[i] + 0x25,
                    [](SafetyHookContext& ctx)
                    {
                        static std::atomic<bool> logged{ false };
                        if (!logged.exchange(true)) LogInfo("FIRED: Resolution (hit #3+)");
                        ctx.rcx = iCustomResX;
                        ctx.rax = iCustomResY;
                    }));
            }
            // v2.0.2: the two hooked sites are NOT the only inlined copies of the
            // preset->dimensions conversion - runtime testing showed the swapchain is sized
            // through a copy our pattern cannot catch (different register allocation), so the
            // screen stayed 16:9 even though both hooks fired ("game wanted 2560x1440").
            // All copies, however, read the same two 5-entry preset tables in .rdata:
            //   width  {3840, 2560, 1920, 1600, 1280}
            //   height {2160, 1440, 1080,  900,  720}
            // Patching the tables themselves covers every consumer at once. Table addresses
            // are derived at runtime from hit #1's two rip-relative leas (lea r64,[width]
            // at +0x11, lea r64,[height] at +0x1B - disp32 at +0x14/+0x1E, next insn at
            // +0x18/+0x22), then the contents are sanity-checked before writing.
            uint8_t* site = ResolutionScanResults[0];
            if (site[0x11] == 0x48 && site[0x12] == 0x8D && site[0x1B] == 0x48 && site[0x1C] == 0x8D)
            {
                uint32_t* widthTable = reinterpret_cast<uint32_t*>(site + 0x18 + *reinterpret_cast<int32_t*>(site + 0x14));
                uint32_t* heightTable = reinterpret_cast<uint32_t*>(site + 0x22 + *reinterpret_cast<int32_t*>(site + 0x1E));
                if (widthTable[0] == 3840 && widthTable[2] == 1920 && heightTable[0] == 2160 && heightTable[2] == 1080)
                {
                    LogInfo("Custom Resolution: preset tables found at %s+0x%llx / %s+0x%llx - patching all 5 presets to %dx%d",
                        sExeName.c_str(), ModOffset(reinterpret_cast<uint8_t*>(widthTable)),
                        sExeName.c_str(), ModOffset(reinterpret_cast<uint8_t*>(heightTable)),
                        iCustomResX, iCustomResY);
                    for (int i = 0; i < 5; ++i)
                    {
                        Memory::Write((uintptr_t)&widthTable[i], (uint32_t)iCustomResX);
                        Memory::Write((uintptr_t)&heightTable[i], (uint32_t)iCustomResY);
                    }
                }
                else
                {
                    LogError("Custom Resolution: preset table contents unexpected (%u/%u/%u/%u) - table patch skipped",
                        widthTable[0], widthTable[2], heightTable[0], heightTable[2]);
                }
            }
            else
            {
                LogError("Custom Resolution: lea opcodes not at hit #1 +0x11/+0x1B - table patch skipped");
            }

            LogInfo("Custom Resolution: Applied custom resolution of %dx%d (%zu hook(s))", iCustomResX, iCustomResY, ResolutionScanResults.size());
        }
        else
        {
            LogError("MISS: Resolution pattern not found - feature disabled");
        }

        // ---- v2.0.2 late-apply fixes (root cause analysis in hunt_present_path.txt) ----
        // The boot-time "apply saved settings" path does NOT go through the 5-entry preset
        // table above. It switches the active row of the QUALITY TABLE (6 rows, stride 0x4C,
        // static defaults all 1920x1080: +0x24/+0x28 render W/H, +0x2C/+0x30 window W/H),
        // then two xchg-quad sites publish the row values to the resolution globals that
        // drive render targets, the window request and the swapchain resize. That is what
        // reverted the screen to 16:9 on the second boot-time flicker.

        // F2: patch all 6 quality-table rows. Table base is derived from the unique
        // "imul rax,rax,0x4C; lea r8,[table]; mov eax,[rax+r8+0x04]" indexing site.
        uint8_t* QualityTableSite = Memory::PatternScan(baseModule, "48 6B C0 4C 4C 8D 05 ?? ?? ?? ?? 42 8B 44 00 04");
        if (QualityTableSite)
        {
            uint8_t* qualityTable = QualityTableSite + 0xB + *reinterpret_cast<int32_t*>(QualityTableSite + 0x7);
            uint32_t* row0dims = reinterpret_cast<uint32_t*>(qualityTable + 0x24);
            if (row0dims[0] == 1920 && row0dims[1] == 1080)
            {
                LogInfo("Custom Resolution: quality table found at %s+0x%llx - patching 6 rows to %dx%d",
                    sExeName.c_str(), ModOffset(qualityTable), iCustomResX, iCustomResY);
                for (int r = 0; r < 6; ++r)
                {
                    uint8_t* row = qualityTable + r * 0x4C;
                    Memory::Write((uintptr_t)(row + 0x24), (uint32_t)iCustomResX);  // render W
                    Memory::Write((uintptr_t)(row + 0x28), (uint32_t)iCustomResY);  // render H
                    Memory::Write((uintptr_t)(row + 0x2C), (uint32_t)iCustomResX);  // window W
                    Memory::Write((uintptr_t)(row + 0x30), (uint32_t)iCustomResY);  // window H
                }
            }
            else
            {
                LogError("Custom Resolution: quality table row0 unexpected (%ux%u) - row patch skipped",
                    row0dims[0], row0dims[1]);
            }
        }
        else
        {
            LogError("MISS: QualityTable pattern not found - late settings apply may revert resolution");
        }

        // F3: pin the two xchg-quad publish sites. Whatever gets written into the quality
        // table row later (saved-settings apply, staging swap), the values published to the
        // resolution globals are forced to ours. Register roles at both sites:
        // ecx = render W, edx = render H, r8d = window/UI W, eax = window/UI H.
        struct XchgQuadSite { const char* name; const char* pattern; size_t hookOffset; };
        const XchgQuadSite XchgQuadSites[] = {
            // "mov ecx,[rax+r13+0x24]; mov edx,[+0x28]; mov r8d,[+0x2C]; mov eax,[+0x30]" @ RVA 0x006C126B
            { "ResPublish1", "42 8B 4C 28 24 42 8B 54 28 28 46 8B 44 28 2C 42 8B 44 28 30", 0x14 },
            // same quad, rbx-indexed encoding @ RVA 0x001C8F33
            { "ResPublish2", "8B 4C 18 24 8B 54 18 28 44 8B 44 18 2C 8B 44 18 30", 0x11 },
        };
        static std::vector<SafetyHookMid> ResPublishMidHooks;
        for (const auto& s : XchgQuadSites)
        {
            uint8_t* hit = Memory::PatternScan(baseModule, s.pattern);
            if (!hit)
            {
                LogError("MISS: %s pattern not found - late settings apply may revert resolution", s.name);
                continue;
            }
            LogInfo("HIT: %s: %s+0x%llx (hook at +0x%zx)", s.name, sExeName.c_str(), ModOffset(hit), s.hookOffset);
            ResPublishMidHooks.push_back(safetyhook::create_mid(hit + s.hookOffset,
                [](SafetyHookContext& ctx)
                {
                    static std::atomic<bool> logged{ false };
                    if (!logged.exchange(true)) LogInfo("FIRED: ResPublish (was %ux%u / %ux%u)",
                        (uint32_t)ctx.rcx, (uint32_t)ctx.rdx, (uint32_t)ctx.r8, (uint32_t)ctx.rax);

                    ctx.rcx = iCustomResX;
                    ctx.rdx = iCustomResY;
                    ctx.r8  = iCustomResX;
                    ctx.rax = iCustomResY;
                }));
        }

        // F4: two more inlined copies of the preset->dimensions conversion (S3/S4) whose
        // prologue is "8B 41 3C" instead of "41 8B ?? 3C", so the main Resolution pattern
        // cannot see them. The patched preset table already covers them data-wise; hooking
        // is belt and braces (and keeps working if the table patch ever gets skipped).
        std::vector<uint8_t*> ResolutionAltResults = Memory::PatternScanAll(baseModule, "8B 41 3C 3C 04 B9 04 00 00 00 0F 42 C8 0F B6 C1");
        if (!ResolutionAltResults.empty())
        {
            LogInfo("HIT: ResolutionAlt: %zu hit(s)", ResolutionAltResults.size());
            static std::vector<SafetyHookMid> ResolutionAltMidHooks;
            for (uint8_t* hit : ResolutionAltResults)
            {
                LogInfo("HIT: ResolutionAlt: %s+0x%llx (hook at +0x24)", sExeName.c_str(), ModOffset(hit));
                ResolutionAltMidHooks.push_back(safetyhook::create_mid(hit + 0x24,
                    [](SafetyHookContext& ctx)
                    {
                        static std::atomic<bool> logged{ false };
                        if (!logged.exchange(true)) LogInfo("FIRED: ResolutionAlt");

                        ctx.rcx = iCustomResX;
                        ctx.rax = iCustomResY;
                    }));
            }
        }
        else
        {
            LogError("MISS: ResolutionAlt pattern not found (S3/S4) - covered by table patch if that succeeded");
        }
    }
}

void GraphicalFixes()
{
    if (bCustomResolution)
    {
        // Fix graphical corruption at odd resolution widths.
        // v2.0.2 finding: the value the hook fixes is width/64, NOT width/32 as v1 assumed
        // (v1's gate was w/32 only). Gate on either w/32 or w/64 being fractional: keeps v1
        // parity and also catches widths divisible by 32 but not by 64 (e.g. 2592), which
        // corrupt yet would slip past the old gate. 3440 triggers both.
        float graphicsWidth32 = (float)iCustomResX / 32;
        float graphicsWidth64 = (float)iCustomResX / 64;
        if (floor(graphicsWidth32) != graphicsWidth32 || floor(graphicsWidth64) != graphicsWidth64)
        {
            LogInfo("Graphics Corruption: Detected non-whole number, injecting hooks to round up values.");

            // [REFOUND v2.0.2] GfxCorruption1/2: both patterns start at the 8-byte
            // "vmulss xmm3, xmm3, [rip+disp32]" (xmm3 = renderWidth * 1/64), followed by the
            // vinsertps triple that packs the float4 {w/64, h/60, ceil-ish w/32, ceil-ish h/30}
            // and the vmovaps store into a cbuffer block. Each pattern has MULTIPLE hits
            // (branch arms of the same computation: scaled/unscaled x alloc success/fallback);
            // which arm runs depends on runtime state, so EVERY hit must be hooked.
            // Hook at +0x8 = the first vinsertps: proven instruction boundary, 6 bytes,
            // no rip-relative operand - safe for safetyhook mid-hook relocation.
            // ceilf is a no-op for already-integral values, so over-hooking arms is harmless.
            static std::vector<SafetyHookMid> GraphicsCorruptionMidHooks;

            struct GfxCorruptionPattern { const char* name; const char* pattern; size_t expectedHits; };
            const GfxCorruptionPattern GfxCorruptionPatterns[] = {
                // Verified on v2.0.2: exactly 4 hits @ RVA 0x02487219 / 0x024872D0 / 0x02487557 / 0x02487672
                { "GfxCorruption1", "C5 E2 59 1D ?? ?? ?? ?? C4 E3 61 21 C0 10 C4 E3 79 21 C2 20 C4 E3 79 21 C1 30 C5 F8 29 00", 4 },
                // Verified on v2.0.2: exactly 2 hits @ RVA 0x021A8D86 / 0x021A8E4E
                { "GfxCorruption2", "C5 E2 59 1D ?? ?? ?? ?? C4 E3 61 21 D2 10 C4 E3 69 21 C9 20 C4 E3 71 21 C0 30 C5 F8 29 00 C5 F8 28 05 ?? ?? ?? ?? C5 F8 29 40 10", 2 },
            };

            for (const auto& p : GfxCorruptionPatterns)
            {
                std::vector<uint8_t*> hits = Memory::PatternScanAll(baseModule, p.pattern);
                if (hits.empty())
                {
                    LogError("MISS: %s pattern not found - feature disabled", p.name);
                    continue;
                }

                LogInfo("HIT: %s: %zu match(es)", p.name, hits.size());
                if (hits.size() != p.expectedHits)
                    LogError("WARN: %s: expected %zu matches, found %zu - hooking all of them anyway", p.name, p.expectedHits, hits.size());

                for (uint8_t* hit : hits)
                {
                    LogInfo("HIT: %s: %s+0x%llx (hook at +0x8)", p.name, sExeName.c_str(), ModOffset(hit));
                    GraphicsCorruptionMidHooks.push_back(safetyhook::create_mid(hit + 0x8,
                        [](SafetyHookContext& ctx)
                        {
                            // xmm3 = renderWidth / 64 - round up (lane0 of the packed float4)
                            ctx.xmm3.f32[0] = ceilf(ctx.xmm3.f32[0]);
                        }));
                }
            }
        }

        // [OK v2.0.2] Screen Effects
        uint8_t* ScreenEffectsScanResult = Memory::PatternScan(baseModule, "C5 ?? ?? ?? 48 ?? ?? ?? ?? ?? 00 C5 ?? ?? ?? ?? ?? ?? 00 C3");
        if (ScreenEffectsScanResult)
        {
            ScreenEffectsScanResult += 0xB; // hook offset (Lyall: pattern + 0xB)
            LogInfo("HIT: ScreenEffects: %s+0x%llx", sExeName.c_str(), ModOffset(ScreenEffectsScanResult));

            static SafetyHookMid ScreenEffects1MidHook{};
            ScreenEffects1MidHook = safetyhook::create_mid(ScreenEffectsScanResult,
                [](SafetyHookContext& ctx)
                {
                    static std::atomic<bool> logged{ false };
                    if (!logged.exchange(true)) LogInfo("FIRED: ScreenEffects");

                    // v2.0.2: this store is the view constant block's +0x59C, a
                    // shader-consumed factor read by BOTH the scene pass and full-screen
                    // combat-VFX quads (charge flashes / finisher overlays). The correct
                    // value at any non-16:9 aspect is fAspectMultiplier (aspect / (16/9)):
                    // field A/B at 3440x1440 with the FOV confound removed shows the scene
                    // renders identically under 1.0 and 1.34375, while the VFX quads are
                    // sized by this factor - shipping 1.0 left them at 16:9 width with
                    // vertical bars on the 16:9 boundaries (GitHub issue #1). ADR-0004's
                    // "fAspectMultiplier zooms the scene" diagnosis was confounded; see
                    // ADR-0012.
                    if (fAspectRatio != fNativeAspect)
                    {
                        ctx.xmm0.f32[0] = fAspectMultiplier;
#ifdef GBFR_DEVBUILD
                        // [Debug - Scene] CropFactorOverride (dev builds only): diagnostic
                        // lever kept from the flash-bars hunt (ADR-0012); >= 0 replaces the
                        // shipping value at >16:9.
                        if (fAspectRatio > fNativeAspect && fCropFactorOverride >= 0.0f)
                            ctx.xmm0.f32[0] = fCropFactorOverride;
#endif // GBFR_DEVBUILD
                    }
                });
        }
        else
        {
            LogError("MISS: ScreenEffects pattern not found - feature disabled");
        }
    }

#ifdef GBFR_DEVBUILD
    // [Debug - Scene] WindowRefOverride experiment site (dev builds only; see the
    // variable's comment block near the top of this file and docs/PATTERNS.md 3.23).
    // Site = inside the view-CB producer (RVA 0x020D0E20), at the windowRef pair fill:
    //   +0x00  vcvtsi2ss xmm1, xmm6, [rip->windowRefW]   (8 bytes, disp32 at +0x4)
    //   +0x08  mov rdx, [rcx+0x2A0]                      (7 bytes)  <- HOOK (xmm1 = W)
    //   +0x0F  vmovss [rdx+0x594], xmm1                  (8 bytes)
    //   +0x17  vcvtsi2ss xmm1, xmm6, [rip->windowRefH]   (8 bytes, disp32 at +0x1B)
    //   +0x1F  mov rdx, [rcx+0x2A0]                      (7 bytes)
    //   +0x26  vmovss [rdx+0x598], xmm1                  (8 bytes)
    // Verified unique on v2.0.2 @ RVA 0x020D10C5 (hook +0x8 = RVA 0x020D10CD; the
    // stolen 7-byte mov has no rip-relative operand). The scan runs in EVERY dev build
    // regardless of the ini key, so the HIT line doubles as a pattern-survival canary;
    // the hook installs only when WindowRefOverride = true and the screen is >16:9.
    {
        uint8_t* WindowRefScanResult = Memory::PatternScan(baseModule,
            "C5 CA 2A 0D ?? ?? ?? ?? 48 8B 91 A0 02 00 00 C5 FA 11 8A 94 05 00 00 "
            "C5 CA 2A 0D ?? ?? ?? ?? 48 8B 91 A0 02 00 00 C5 FA 11 8A 98 05 00 00");
        if (WindowRefScanResult)
        {
            LogInfo("HIT: WindowRefOverride site: %s+0x%llx (hook at +0x8)", sExeName.c_str(), ModOffset(WindowRefScanResult));

            // Resolve both rip-relative int globals from the two vcvtsi2ss disp32
            // fields (GetAbsolute64 takes the disp32 address; the instructions end at
            // +0x8 / +0x1F). Expected on v2.0.2: W = 0x06B84098, H = 0x06B8409C.
            // Logging their CURRENT values is itself evidence: at install time (after
            // the injection delay) they show whether ResPublish already patched them.
            uintptr_t windowRefWGlobal = Memory::GetAbsolute64((uintptr_t)WindowRefScanResult + 0x4);
            uintptr_t windowRefHGlobal = Memory::GetAbsolute64((uintptr_t)WindowRefScanResult + 0x1B);
            LogInfo("WindowRefOverride: windowRef globals at install: W %s+0x%llx = %d, H %s+0x%llx = %d",
                sExeName.c_str(), ModOffset((const uint8_t*)windowRefWGlobal), *(volatile int*)windowRefWGlobal,
                sExeName.c_str(), ModOffset((const uint8_t*)windowRefHGlobal), *(volatile int*)windowRefHGlobal);

            if (!bWindowRefOverride)
            {
                LogInfo("WindowRefOverride: disabled (WindowRefOverride = false) - site verified, no hook installed");
            }
            else if (fAspectRatio <= fNativeAspect)
            {
                LogInfo("WindowRefOverride: aspect %g is not wider than 16:9 - hook not installed", fAspectRatio);
            }
            else
            {
                g_pWindowRefHGlobal = (volatile int*)windowRefHGlobal;
                static SafetyHookMid WindowRefOverrideMidHook{};
                WindowRefOverrideMidHook = safetyhook::create_mid(WindowRefScanResult + 0x8,
                    [](SafetyHookContext& ctx)
                    {
                        // xmm1 = (float)windowRefW the game just converted, about to
                        // be stored to CB +0x594. The +0x598 store a few instructions
                        // later receives (float)windowRefH from its global
                        // UNCONDITIONALLY (no [0x07032DE0]+0x65 branch, unlike the
                        // render W/H pair at +0x58C/+0x590) - so the consistent
                        // override that makes +0x594/+0x598 == the real screen aspect
                        // is windowRefH * fAspectRatio, re-reading the SAME global the
                        // +0x598 store is about to read. A hardcoded 1080*aspect would
                        // give a wrong ratio whenever the global holds 1440.
                        float incoming = ctx.xmm1.f32[0];
                        int refH = *g_pWindowRefHGlobal;
                        float overridden = (float)refH * fAspectRatio;
                        ctx.xmm1.f32[0] = overridden;
                        static std::atomic<bool> logged{ false };
                        if (!logged.exchange(true))
                            LogInfo("FIRED: WindowRefOverride (CB windowRef W in: %g -> %g; windowRefH global: %d)", incoming, overridden, refH);
                    });
                LogInfo("WindowRefOverride: hook installed - CB +0x594 will be forced to windowRefH * %g", fAspectRatio);
            }
        }
        else
        {
            LogError("MISS: WindowRefOverride site pattern not found - experiment unavailable");
        }
    }
#endif // GBFR_DEVBUILD
}

void AspectFOVFix()
{
    // ============================== SCAN PHASE ==============================
    // ORDERING IS LOAD-BEARING (ADR-0008/ADR-0011, docs/PATTERNS.md 3.5/3.17/3.19):
    // the four aspect/FOV patterns below overlap each other's bytes at two sites, and
    // installing a mid-hook rewrites bytes at its site (trampoline jmp), which would
    // turn a later scan into a MISS:
    //   - view-params site: the AspectRatio pattern (RVA 0x00751089, 0x37 bytes) spans
    //     the ViewParamsFOV hook site (+0x20), and the ViewParamsFOV pattern starts at
    //     the exact address the AspectRatio mid-hook patches (RVA 0x0075109A).
    //   - projection-builder site: the ProjMatrixFOV pattern's first 13 bytes ARE the
    //     ProjMatrixAspect pattern (both hit RVA 0x00750970), and the ProjMatrixAspect
    //     mid-hook at +0x9 rewrites bytes +0x9..+0x10 inside the ProjMatrixFOV pattern.
    // ALL FOUR scans therefore complete here before ANY of the four hooks installs.
    // The installed hooks themselves coexist: at the builder the stolen ranges are
    // +0x9..+0x10 vs +0x1A..+0x1E (the 9-byte vmulss between them stays intact); at
    // the view-params site +0x11.. vs +0x20...

    // [ROLE CHANGE 2026-07-10] ViewParamsFOV - culling/shared-view-constants FOV
    // consistency. This hook does NOT change the rendered FOV (ADR-0011): the xmm3 it
    // multiplies is stored only to [rsp+0x20] and passed to 0x0216ABE0, the culling /
    // shared-view-constants builder; the projection matrix is built at RVA 0x00750970
    // and re-reads the FOV FROM MEMORY [r14+0x9D4], out of this register's reach.
    // (Diagnosed 2026-07-10: HIT and FIRED were both genuine - the hook ran, on the
    // right value, at a site that cannot alter the image.) KEPT deliberately: with
    // ProjMatrixFOV below widening the rendered frustum, this hook makes the culling
    // path see the same multiplied FOV - without it, fFOVMulti > 1 culls objects
    // against the narrower unmultiplied frustum and pops them at the screen edges.
    //
    // Pattern = the four adjacent view-params loads (viewport W/H, aspect, FOV):
    //   +0x00 vmovss xmm0,[rax+0x9A0]   ; viewport width
    //   +0x08 vmovss xmm1,[rax+0x9A4]   ; viewport height
    //   +0x10 vmovss xmm2,[rax+0x9D0]   ; aspect
    //   +0x18 vmovss xmm3,[rax+0x9D4]   ; FOV -> xmm3
    //   +0x20 vmovss [rsp+0x40],xmm11   ; <- hook here (after the FOV load)
    // Expected unique hit RVA 0x0075109A. Hook at +0x20: xmm3 *= fFOVMulti - register
    // only, same install gate as ProjMatrixFOV so the two stay in lockstep.
    //
    // NO hook is added at pattern+0x10 (the alternative aspect-force spot): our
    // AspectRatio hook below already force-writes [viewParams+0x9D0] = fAspectRatio at
    // this very site BEFORE the +0x10 load executes - duplicating the write here would
    // be redundant.
    uint8_t* ViewParamsFOVScanResult = nullptr;
    if (fFOVMulti != (float)1)
    {
        std::vector<uint8_t*> ViewParamsFOVHits = Memory::PatternScanAll(baseModule, "C5 ?? ?? ?? A0 09 00 00 C5 ?? ?? ?? A4 09 00 00 C5 ?? ?? ?? D0 09 00 00 C5 ?? ?? ?? D4 09 00 00");
        if (!ViewParamsFOVHits.empty())
        {
            if (ViewParamsFOVHits.size() != 1)
                LogError("WARN: ViewParamsFOV: expected unique hit, found %zu - hooking the first only", ViewParamsFOVHits.size());
            ViewParamsFOVScanResult = ViewParamsFOVHits[0];
            LogInfo("HIT: ViewParamsFOV: %s+0x%llx (hook at +0x20, multiplier %g)", sExeName.c_str(), ModOffset(ViewParamsFOVScanResult), fFOVMulti);
        }
        else
        {
            LogError("MISS: ViewParamsFOV pattern not found - culling will not match the multiplied FOV (edge pop-in possible at multipliers > 1)");
        }
    }

    // [NEW 2026-07-10] ProjMatrixFOV - THE rendered gameplay-FOV multiplier (ADR-0011).
    // Site = the projection-matrix builder @ RVA 0x00750970 (the same site
    // ProjMatrixAspect hooks): dirty(+0x9DE)-triggered, and it serves EVERY camera
    // whose dirty flag is set - gameplay, cutscenes, menu 3D scenes. There is no clean
    // "is gameplay" discriminator at this site, so the multiplier is GLOBAL BY DESIGN
    // (cutscenes are also affected; default 1.0 = off).
    //   +0x00 vmovss xmm8,[r14+0x9D0]      ; aspect (ProjMatrixAspect overrides at +0x9)
    //   +0x09 vmovss xmm7,[rip+...]        ; = 0.5
    //   +0x11 vmulss xmm0,xmm7,[r14+0x9D4] ; xmm0 = FOV/2 - FOV read FROM MEMORY
    //   +0x1A call tanf                    ; <- hook here: xmm0 *= fFOVMulti before tan()
    //   +0x1F ...                          ; yscale = 1/tan(FOV/2); xscale = yscale/aspect
    // Pattern = the ProjMatrixAspect pattern extended through the vmulss and the call
    // opcode; expected unique hit RVA 0x00750970. Hook +0x1A steals exactly the 5-byte
    // E8 call (safetyhook relocates the rel32; the hook body runs BEFORE the relocated
    // call, which is what we need). tan((m*FOV)/2) is an exact angular multiply; yscale
    // AND xscale both follow, so the frustum widens consistently on both axes.
    //
    // NEVER write [obj+0x9D4] memory instead: 14+ rip-visible writers rewrite that
    // field (several per-frame) - a memory multiply is either overwritten (no effect)
    // or, placed after a per-frame writer, compounds every frame and explodes within a
    // second. A register multiply at the final consumer is immune to both.
    uint8_t* ProjMatrixFOVScanResult = nullptr;
    if (fFOVMulti != (float)1)
    {
        std::vector<uint8_t*> ProjMatrixFOVHits = Memory::PatternScanAll(baseModule, "C4 41 7A 10 86 D0 09 00 00 C5 FA 10 3D ?? ?? ?? ?? C4 C1 42 59 86 D4 09 00 00 E8");
        if (!ProjMatrixFOVHits.empty())
        {
            if (ProjMatrixFOVHits.size() != 1)
                LogError("WARN: ProjMatrixFOV: expected unique hit, found %zu - hooking the first only", ProjMatrixFOVHits.size());
            ProjMatrixFOVScanResult = ProjMatrixFOVHits[0];
            LogInfo("HIT: ProjMatrixFOV: %s+0x%llx (hook at +0x1A, multiplier %g)", sExeName.c_str(), ModOffset(ProjMatrixFOVScanResult), fFOVMulti);
        }
        else
        {
            LogError("MISS: ProjMatrixFOV pattern not found - gameplay FOV multiplier disabled");
        }
    }

    uint8_t* AspectRatioScanResult = nullptr;
    uint8_t* ProjMatrixScanResult = nullptr;
    if (bAspectFix)
    {
        // [REFOUND v2.0.2] Aspect Ratio: only change vs v1 is byte[2] 49 -> 48 - v1's
        // "mov rax,[r14]" is compiled as "mov rax,[rbx]" in v2.0.2 (register allocation
        // change only). Verified unique hit at RVA 0x00751089; hook at +0x11 (first vmovss)
        // unchanged. Camera struct unchanged: +0x9D0 = aspect, +0x9D4 = FOV (writer site
        // verified at RVA 0x00691D3A: vdivss w/h then vmovss [rdx+0x9D0]).
        AspectRatioScanResult = Memory::PatternScan(baseModule, "74 ?? 48 ?? ?? 48 ?? ?? ?? ?? ?? 00 48 ?? ?? 74 ?? C5 ?? ?? ?? ?? ?? ?? 00 C5 ?? ?? ?? ?? ?? ?? 00 C5 ?? ?? ?? ?? ?? ?? 00 C5 ?? ?? ?? ?? ?? ?? 00 C5 ?? ?? ?? ?? ??");
        if (AspectRatioScanResult)
        {
            LogInfo("HIT: AspectRatio: %s+0x%llx (hook at +0x11)", sExeName.c_str(), ModOffset(AspectRatioScanResult));
        }
        else
        {
            LogError("MISS: AspectRatio pattern not found - feature disabled");
        }

        // [NEW v2.0.2] Projection-matrix builder (hunt_present_path.txt): the AspectRatio
        // hook feeds only a secondary consumer (culling/shared constants at 0x0216ABE0).
        // The REAL projection matrix is built earlier in the same function, at RVA
        // 0x00750970: dirty(+0x9DE)-triggered, xscale = 1/tan(fov/2)/aspect, writes camera
        // +0x40/+0x100. Data-driven 16:9 aspect writers (e.g. cutscene camera data via
        // 0x00ECFA78) set the dirty flag themselves, so their value always reaches the
        // matrix first - overriding xmm8 right after "vmovss xmm8,[r14+0x9D0]" (9 bytes,
        // hence +0x9) covers every camera and every aspect source. ProjMatrixFOV (above)
        // extends this same pattern - see the scan-phase ordering note.
        ProjMatrixScanResult = Memory::PatternScan(baseModule, "C4 41 7A 10 86 D0 09 00 00 C5 FA 10 3D");
        if (ProjMatrixScanResult)
        {
            LogInfo("HIT: ProjMatrixAspect: %s+0x%llx (hook at +0x9)", sExeName.c_str(), ModOffset(ProjMatrixScanResult));
        }
        else
        {
            LogError("MISS: ProjMatrixAspect pattern not found - 3D camera may stay 16:9");
        }
    }

    // ============================= INSTALL PHASE =============================
    // All four aspect/FOV scans above are complete - installing may now rewrite site
    // bytes freely. Do NOT scan any of the four patterns after this point.
    if (AspectRatioScanResult)
    {
        static SafetyHookMid AspectRatioMidHook{};
        AspectRatioMidHook = safetyhook::create_mid(AspectRatioScanResult + 0x11,
            [](SafetyHookContext& ctx)
            {
                static std::atomic<bool> logged{ false };
                if (!logged.exchange(true)) LogInfo("FIRED: AspectRatio");

#ifdef GBFR_DEVBUILD
                // rax = camera/view-params object - cache it for DiagCamDistWatchdog.
                g_DiagCamObj.store(ctx.rax, std::memory_order_relaxed);
#endif // GBFR_DEVBUILD
                *reinterpret_cast<float*>(ctx.rax + 0x9D0) = fAspectRatio;
            });
    }

    if (ProjMatrixScanResult)
    {
        static SafetyHookMid ProjMatrixAspectMidHook{};
        ProjMatrixAspectMidHook = safetyhook::create_mid(ProjMatrixScanResult + 0x9,
            [](SafetyHookContext& ctx)
            {
                static std::atomic<bool> logged{ false };
                if (!logged.exchange(true)) LogInfo("FIRED: ProjMatrixAspect");

#ifdef GBFR_DEVBUILD
                // r14 = camera object in the projection-matrix builder - cache it
                // for DiagCamDistWatchdog.
                g_DiagCamObj.store(ctx.r14, std::memory_order_relaxed);
#endif // GBFR_DEVBUILD
                ctx.xmm8.f32[0] = fAspectRatio;
            });
    }

    // ProjMatrixFOV install. Only when fFOVMulti != 1.0 - at 1.0 the multiply is a no-op.
    if (ProjMatrixFOVScanResult)
    {
        static SafetyHookMid ProjMatrixFOVMidHook{};
        ProjMatrixFOVMidHook = safetyhook::create_mid(ProjMatrixFOVScanResult + 0x1A,
            [](SafetyHookContext& ctx)
            {
                static std::atomic<bool> logged{ false };
                if (!logged.exchange(true)) LogInfo("FIRED: ProjMatrixFOV (base FOV %g x%g)", ctx.xmm0.f32[0] * 2.0f, fFOVMulti);

                // xmm0 = FOV/2 (radians), on its way into tanf. Register-only multiply -
                // [obj+0x9D4] game memory stays untouched (see the scan comment).
                ctx.xmm0.f32[0] *= fFOVMulti;
            });
    }

    // ViewParamsFOV install - the culling-consistency companion of ProjMatrixFOV.
    if (ViewParamsFOVScanResult)
    {
        static SafetyHookMid ViewParamsFOVMidHook{};
        ViewParamsFOVMidHook = safetyhook::create_mid(ViewParamsFOVScanResult + 0x20,
            [](SafetyHookContext& ctx)
            {
                static std::atomic<bool> logged{ false };
                if (!logged.exchange(true)) LogInfo("FIRED: ViewParamsFOV (culling-path base FOV %g x%g)", ctx.xmm3.f32[0], fFOVMulti);

                // FOV was just loaded from [viewParams+0x9D4] into xmm3; this register
                // feeds ONLY the culling/shared-view-constants call (no visual effect) -
                // kept in lockstep with ProjMatrixFOV so edge objects are not culled
                // against the unmultiplied frustum. Game memory stays unmodified.
                ctx.xmm3.f32[0] *= fFOVMulti;
            });
    }

    // [REMOVED 2026-07-10] GameplayCamera distance hook (v1-relocated message-block
    // site, unique hit RVA 0x009D8C70, hook +0x8, xmm9 *= fCamDistMulti): the pattern
    // HIT but the hook never FIRED in any session - v2.0.2 gameplay no longer drives
    // the camera through that message path. Deleted so the distance cannot be
    // multiplied TWICE through the live families below if the message path ever
    // revives. Historical notes - incl. the "v1 FOV slot is now pitch, hooking it
    // tilts the camera" trap - are kept in docs/PATTERNS.md 3.6.
    //
    // [NEW 2026-07-10] Camera distance multiplier - three live hook families, derived
    // from community ultrawide research for this exe build and re-verified offline
    // instruction-by-instruction (docs/PATTERNS.md 3.20-3.22). The three sites do not
    // overlap each other (or anything else we hook), so plain scan+install per family
    // is safe here - no ordering constraint like the aspect/FOV block above.
    if (fCamDistMulti != (float)1)
    {
        // F1 - CamDistPreset (primary effect): the four inlined "apply active camera
        // preset" sites that publish the preset's distance [rcx+0x14] (meters, default
        // 4.8) to the global camera-params block [0x07C25720]:
        //   +0x00 vmovss xmm0,[rcx+0x14]   ; rcx = active preset, +0x14 = distance
        //   +0x05 vmovss [rip->global],xmm0 ; publish  <- hook lands ON this boundary
        //   +0x0D vmovaps xmm0,[rcx+0x30]  ; rest of the block copy (FOV +0x18, ...)
        // Expected 4 hits (RVA 0x0095A91F / 0x01F9245F / 0x0268DA8F / 0x02DB617F) -
        // branch/caller copies; which one runs depends on game mode, so hook ALL FOUR
        // (like GfxCorruption; over-hooking is harmless, each publish is scaled exactly
        // once). Hook +0x5: after the 5-byte load, before the 8-byte rip-relative
        // publish store (safetyhook relocates the rip operand - same class as our
        // FPSCap/Nameplate sites). Multiplying the PUBLISHED value never compounds:
        // the preset source field is never written, so re-publishing starts from the
        // unscaled 4.8 every time.
        std::vector<uint8_t*> CamDistPresetHits = Memory::PatternScanAll(baseModule, "C5 FA 10 41 14 C5 FA 11 05 ?? ?? ?? ?? C5 F8 28 41 30 C5 F8");
        if (!CamDistPresetHits.empty())
        {
            LogInfo("HIT: CamDistPreset: %zu hit(s)", CamDistPresetHits.size());
            if (CamDistPresetHits.size() != 4)
                LogError("WARN: CamDistPreset: expected 4 hits, found %zu - hooking all of them anyway", CamDistPresetHits.size());

            static std::vector<SafetyHookMid> CamDistPresetMidHooks;
            for (uint8_t* hit : CamDistPresetHits)
            {
                LogInfo("HIT: CamDistPreset: %s+0x%llx (hook at +0x5)", sExeName.c_str(), ModOffset(hit));
                CamDistPresetMidHooks.push_back(safetyhook::create_mid(hit + 0x5,
                    [](SafetyHookContext& ctx)
                    {
                        static std::atomic<bool> logged{ false };
                        if (!logged.exchange(true)) LogInfo("FIRED: CamDistPreset (dist %g -> %g)", ctx.xmm0.f32[0], ctx.xmm0.f32[0] * fCamDistMulti);

                        ctx.xmm0.f32[0] *= fCamDistMulti;
                    }));
            }
        }
        else
        {
            LogError("MISS: CamDistPreset pattern not found - camera distance multiplier (preset path) disabled");
        }

        // F2 - FollowCamDist: the follow-camera's 1/2/3-channel zoom-track selector
        // (popcnt on a channel mask) loads the zoom value into xmm8 from
        // [rdi+0x17C/+0x180/+0x184] (or vxorps xmm8 when no channel); all arms converge
        // on "vxorps xmm9,xmm9,xmm9" at +0x23 - hook there (5-byte insn, boundary
        // verified; next insn "mov rax,[rsi]" confirms a clean split). The value is a
        // NORMALIZED [0..1] pull-back fraction, NOT meters - downstream the game
        // computes dist = min(1, max(0, xmm8+xmm7)) when flag [obj+0x5B5C]&0x80 is set,
        // so a multiplier > 1 pulls back and SATURATES at the far end of the zoom
        // range via that clamp (expected behavior).
        uint8_t* FollowCamDistScanResult = Memory::PatternScan(baseModule, "C5 7A 10 ?? 7C 01 00 00 EB ?? C4 41 38 57 C0 EB ?? C5 7A 10 ?? 80 01 00 00 EB ?? C5 7A 10 ?? 84 01 00 00 C4 41 30 57 C9");
        if (FollowCamDistScanResult)
        {
            LogInfo("HIT: FollowCamDist: %s+0x%llx (hook at +0x23)", sExeName.c_str(), ModOffset(FollowCamDistScanResult));

            static SafetyHookMid FollowCamDistMidHook{};
            FollowCamDistMidHook = safetyhook::create_mid(FollowCamDistScanResult + 0x23,
                [](SafetyHookContext& ctx)
                {
                    static std::atomic<bool> logged{ false };
                    if (!logged.exchange(true)) LogInfo("FIRED: FollowCamDist (zoom %g -> %g)", ctx.xmm8.f32[0], ctx.xmm8.f32[0] * fCamDistMulti);

                    // xmm8 may be 0 on the vxorps arm - multiplying is harmless there.
                    ctx.xmm8.f32[0] *= fCamDistMulti;
                });
        }
        else
        {
            LogError("MISS: FollowCamDist pattern not found - camera distance multiplier (follow-cam path) disabled");
        }

        // F3 - RoamCamDist: free-roam camera config copy (same camera-apply function
        // family as CamDistPreset hit #1):
        //   +0x00 vmovss xmm0,[rsi+0x38]   ; free-roam distance
        //   +0x05 vmovss [rcx+0x54],xmm0   ; <- hook here
        //   +0x0A vmovss xmm0,[rsi+0x3C]   ; second field - intentionally NOT scaled
        //   +0x0F vmovss [rcx+0x58],xmm0
        // Expected unique hit RVA 0x0095A625; hook +0x5 (after the load, before the
        // 5-byte store). Only the copied value is scaled; the source [rsi+0x38] is
        // untouched, so re-copies never compound.
        uint8_t* RoamCamDistScanResult = Memory::PatternScan(baseModule, "C5 FA 10 ?? 38 C5 FA 11 ?? 54 C5 FA 10 ?? 3C C5 FA 11 ?? 58");
        if (RoamCamDistScanResult)
        {
            LogInfo("HIT: RoamCamDist: %s+0x%llx (hook at +0x5)", sExeName.c_str(), ModOffset(RoamCamDistScanResult));

            static SafetyHookMid RoamCamDistMidHook{};
            RoamCamDistMidHook = safetyhook::create_mid(RoamCamDistScanResult + 0x5,
                [](SafetyHookContext& ctx)
                {
                    static std::atomic<bool> logged{ false };
                    if (!logged.exchange(true)) LogInfo("FIRED: RoamCamDist (dist %g -> %g)", ctx.xmm0.f32[0], ctx.xmm0.f32[0] * fCamDistMulti);

                    ctx.xmm0.f32[0] *= fCamDistMulti;
                });
        }
        else
        {
            LogError("MISS: RoamCamDist pattern not found - camera distance multiplier (free-roam path) disabled");
        }
    }

    if (bFOVFix && (fAspectRatio < fNativeAspect))
    {
        // What remains unported from v1 is the <16:9 vert- gameplay-FOV compensation
        // (v1 divided the camera-message FOV by fAspectMultiplier; v2.0.2's camera
        // message no longer carries FOV, and ProjMatrixFOV is a plain multiplier with
        // no aspect term).
        LogWarn("Gameplay FOV vert- compensation is NOT SUPPORTED on game v2.0.2 (the camera message no longer carries FOV) - gameplay FOV stays uncorrected below 16:9");
        // [FIXED v2.0.2] Cutscene FOV: 4 hits, PatternScan returns the first.
        // All 4 hits are identical inlined copies of the cutscene path; only the first is hooked for now.
        // This hook is only installed when fAspectRatio < 16:9, so 21:9 users are unaffected either way.
        uint8_t* CutsceneFOVScanResult = Memory::PatternScan(baseModule, "48 ?? ?? ?? C5 ?? ?? ?? ?? ?? ?? 00 C5 ?? ?? ?? ?? ?? ?? 00 C5 ?? ?? ?? ?? ?? ?? ?? C5 ?? ?? ?? ?? ?? ?? ?? 00 48 ?? ??");
        if (CutsceneFOVScanResult)
        {
            LogInfo("HIT: CutsceneFOV: %s+0x%llx", sExeName.c_str(), ModOffset(CutsceneFOVScanResult));

            static SafetyHookMid CutsceneFOVMidHook{};
            // v2.0.2: hook at +0x1C, after "vmovaps xmm2,[rbp+0x110]" at +0x14 - Lyall's +0xC ran
            // before that load, so any xmm2 change there was immediately overwritten.
            CutsceneFOVMidHook = safetyhook::create_mid(CutsceneFOVScanResult + 0x1C,
                [](SafetyHookContext& ctx)
                {
                    ctx.xmm2.f32[0] /= fAspectMultiplier;
                });
        }
        else
        {
            LogError("MISS: CutsceneFOV pattern not found - feature disabled");
        }
    }
}

void HUDFix()
{
    if (bHUDFix)
    {
        // [REWORKED v2.0.2] v1's "UIAspect" trick (force the UI ortho width to 16:9) is
        // the wrong tool for v2 and has been REPLACED (hunt_scalecrop.txt). In v2 every
        // named canvas (41 of them, all 3840x2160 units, center pivot) is mapped to the
        // screen through a single scale chosen at RVA 0x0015FAB8:
        //     scaleX = windowRefW / 3840, scaleY = windowRefH / 2160
        //     scale  = scaleX + (scaleY - scaleX) * t     with t HARDCODED to 0
        // i.e. always fill-width. On 21:9 the 16:9 canvas then overflows vertically by
        // (W/3840)/(H/2160) = 1.34375 and the whole frame looks zoomed/cropped.
        // NOPing the lerp's multiply ("vmulss xmm1,xmm1,xmm2" at pattern+0x18) turns the
        // expression into scaleX + (scaleY - scaleX) = scaleY -> fit-height: the UI sits
        // in the centered 16:9 area (the v1 look), and the scene-crop-factor fix in the
        // ScreenEffects hook lets the 3D span the full window width.
        //
        // [NEW 2026-07-10] UIMarkersCanvas - the REAL markers fix. The lerp result is stored
        // to the global canvas manager right after the pattern:
        //     +0x28  vmovss [rax+0x17C], xmm0   ; scaleX  (rax = canvas manager [0x07C02358])
        //     +0x30  vmovss [rax+0x180], xmm1   ; -scaleY (negated for y-flip)
        //     +0x38  vmovss [rax+0x184], xmm0   ; scale
        //     +0x40  mov rsi, [rip+...]         ; <- hook here, rax still = canvas manager
        // World-anchored widgets (enemy HP bars, damage numbers, lock-on) compute
        //     canvasX = screenX/scale - canvasW/2
        // and rendering maps that back through windowW/2 + canvasX*scale, so positions are
        // only correct while canvasW*scale == windowW. Vanilla fill-width satisfies this;
        // our fit-height NOP broke it (uniform +440px right shift at 3440x1440). Writing
        // canvasW = 2160*aspect (>16:9) / canvasH = 3840/aspect (<16:9) at the same site
        // that stores the scale restores the invariant for all ~51 inlined projection
        // copies at once. (Offline analysis: hunt_uimarkers.txt + 2026-07-10 session.)
        uint8_t* CanvasScaleScanResult = Memory::PatternScan(baseModule, "C5 FA 59 05 ?? ?? ?? ?? C5 F2 59 0D ?? ?? ?? ?? C5 F2 5C C8");
        if (CanvasScaleScanResult)
        {
            if (fAspectRatio > fNativeAspect)
            {
                if (CanvasScaleScanResult[0x18] == 0xC5 && CanvasScaleScanResult[0x19] == 0xF2 &&
                    CanvasScaleScanResult[0x1A] == 0x59 && CanvasScaleScanResult[0x1B] == 0xCA)
                {
                    LogInfo("HIT: CanvasFitHeight: %s+0x%llx - NOPing lerp mul at +0x18 (fill-width -> fit-height)",
                        sExeName.c_str(), ModOffset(CanvasScaleScanResult));
                    Memory::PatchBytes((uintptr_t)CanvasScaleScanResult + 0x18, "\x90\x90\x90\x90", 4);
                }
                else
                {
                    LogError("CanvasFitHeight: bytes at +0x18 are not the expected vmulss - patch skipped");
                }
            }

            if (fAspectRatio != fNativeAspect)
            {
                if (CanvasScaleScanResult[0x40] == 0x48 && CanvasScaleScanResult[0x41] == 0x8B &&
                    CanvasScaleScanResult[0x42] == 0x35)
                {
                    LogInfo("HIT: UIMarkersCanvas: %s+0x%llx (hook at +0x40)",
                        sExeName.c_str(), ModOffset(CanvasScaleScanResult + 0x40));

                    static SafetyHookMid UIMarkersCanvasMidHook{};
                    UIMarkersCanvasMidHook = safetyhook::create_mid(CanvasScaleScanResult + 0x40,
                        [](SafetyHookContext& ctx)
                        {
                            float fScale = *reinterpret_cast<float*>(ctx.rax + 0x17C);
                            float fOldW = *reinterpret_cast<float*>(ctx.rax + 0x1BC);
                            float fOldH = *reinterpret_cast<float*>(ctx.rax + 0x1C0);

                            // +0x1BC/+0x1C0 (current W/H) is a DERIVED value: the layout
                            // recalc at RVA 0x0261C5D0 copies +0x1B4/+0x1B8 (source W/H)
                            // over it whenever the canvas is dirty - which happens
                            // constantly in combat. Writing only +0x1BC gets washed back
                            // to 3840 (round-2 offline analysis, writer W4). So write the
                            // SOURCE fields too: the recalc then propagates our value.
                            if (fAspectRatio > fNativeAspect)
                            {
                                *reinterpret_cast<float*>(ctx.rax + 0x1B4) = (float)2160 * fAspectRatio;
                                *reinterpret_cast<float*>(ctx.rax + 0x1BC) = (float)2160 * fAspectRatio;
                            }
                            else if (fAspectRatio < fNativeAspect)
                            {
                                *reinterpret_cast<float*>(ctx.rax + 0x1B8) = (float)3840 / fAspectRatio;
                                *reinterpret_cast<float*>(ctx.rax + 0x1C0) = (float)3840 / fAspectRatio;
                            }

                            static std::atomic<int> fireCount{ 0 };
                            int iFire = ++fireCount;
                            if (iFire <= 5)
                                LogInfo("FIRED: UIMarkersCanvas #%d: scale=%.4f canvas %.0fx%.0f -> %.0fx%.0f",
                                    iFire, fScale, fOldW, fOldH,
                                    *reinterpret_cast<float*>(ctx.rax + 0x1BC),
                                    *reinterpret_cast<float*>(ctx.rax + 0x1C0));
                        });
                }
                else
                {
                    LogError("UIMarkersCanvas: bytes at +0x40 are not the expected mov rsi,[rip+disp32] - hook skipped, world-anchored UI will be offset");
                }
            }
        }
        else
        {
            LogError("MISS: CanvasFitHeight pattern not found - UI stays fill-width (zoomed/cropped) and world-anchored UI stays offset");
        }

        // [REMOVED v2.0.2] v1's per-widget "UIMarkers" hook. Its v2 relocation (RVA
        // 0x026812CD) turned out to be a mode-gated corner-anchored one-off widget path
        // that never executes in gameplay; the real world->screen positioning is inlined
        // ~51x and reads the global canvas manager, fixed by UIMarkersCanvas above.
        // Full story in docs/adr/0006-canvas-manager-invariant-fix.md.

        // Runtime-verified consumers of the manager W (2026-07-10 diagnostic session):
        // the "HP bar shaped" positioner fn 0x02648970 and fn 0x02652B90 both fired in
        // combat reading W=5160 from the manager; the shared WorldToScreen helper is
        // RVA 0x00962FD0. Kept here as breadcrumbs for the next game-version port.

        // [FIXED v2.0.2] Span backgrounds - struct offsets shifted -0x38, width now in xmm1,
        // <16:9 hook moved base+0x28 -> base+0x29 (v1 offset landed inside the 8-byte vmovss xmm4,[rax+0x1C0])
        //
        // [CROSS-VERIFIED 2026-07-10 against community ultrawide research for this exe
        //  build 0x6A3E573A]
        // - ID lists match exactly: width 12 IDs (incl. main-menu bg 2384707215), height
        //   11 IDs (= width list minus dialogue bg 2454207042). No changes needed.
        // - Formulas match: width path gates on [obj+0x1BC]==3840 and writes
        //   xmm1 = 2160*fAspectRatio; height path writes xmm4 = 3840/fAspectRatio;
        //   bSpanAllBackgrounds override (w==3840 && h==2160) writes 2160*fHUDAspectRatio.
        // - Hook offsets: width at pattern+0x2F (our base). A height hook at pattern+0x57
        //   would land MID-INSTRUCTION in this exe (an off-by-one seen in the wild, never
        //   exercised - it only installs at <16:9); the correct post-load boundary is
        //   pattern+0x58 = our base+0x29, which is what we already use. KEEP base+0x29.
        //
        // [Debug - Backgrounds] Probe = true (dev builds only - release builds compile
        // it out) logs every unique object id flowing through the >16:9 width hook
        // (first 512) as a greppable "PROBE-BG:" line with id/size/widen-verdict - the
        // capture workflow for full-screen overlay quads that still render at 16:9
        // width because their id is not in BackgroundWidthIDs (e.g. the Io
        // charge-complete flash, GitHub issue #1).
        uint8_t* UIBackgroundsScanResult = Memory::PatternScan(baseModule, "41 ?? ?? ?? ?? 00 E8 ?? ?? ?? ?? 80 ?? ?? ?? 00 0F ?? ?? ?? ?? ?? 48 ?? ?? ?? ?? 48 ?? ?? E8 ?? ?? ?? ?? 48 ?? ?? ?? C5 ?? ?? ?? ?? ?? ?? 00");
        if (UIBackgroundsScanResult)
        {
            UIBackgroundsScanResult += 0x2F; // hook offset (Lyall: pattern + 0x2F)
            LogInfo("HIT: UIBackgrounds: %s+0x%llx", sExeName.c_str(), ModOffset(UIBackgroundsScanResult));

            // Fade to black = 1932007245 | Pause screen bg = 1611295806 | Dialogue bg = 2454207042  | Title menu bg = 4291119775
            // Main menu bg = 2384707215  | Lyria's journal = 3818795736 | Load save bg = 3969399384 | Title fade white = 1646463024
            // Main menu transition bg = 2056445562 | Title menu fade black = 3970768321 | Title options bg 1 = 603087221 | Title options bg 2 = 61148732
            static std::vector<int> BackgroundWidthIDs = { (int)1932007245, (int)1611295806, (int)2454207042, (int)4291119775, (int)2384707215, (int)3818795736, (int)3969399384, (int)1646463024, (int)2056445562, (int)3970768321, (int)61148732, (int)603087221 };
            static std::vector<int> BackgroundHeightIDs = { (int)1932007245, (int)1611295806, (int)4291119775, (int)2384707215, (int)3818795736, (int)3969399384, (int)1646463024, (int)2056445562, (int)3970768321, (int)61148732, (int)603087221 };

            if (fAspectRatio > fNativeAspect)
            {
                static SafetyHookMid UIBackgroundsWidthMidHook{};
                // v2.0.2: width was just loaded into xmm1 (vmovss xmm1,[rax+0x1BC]); v1 modified xmm0,
                // but xmm0 is overwritten by the very next instruction here, so we modify xmm1 instead.
                UIBackgroundsWidthMidHook = safetyhook::create_mid(UIBackgroundsScanResult,
                    [](SafetyHookContext& ctx)
                    {
                        static std::atomic<bool> logged{ false };
                        if (!logged.exchange(true)) LogInfo("FIRED: UIBackgrounds (width path)");

                        int iObjectID = *reinterpret_cast<int*>(ctx.rax + 0x1C4);
                        float fObjectWidth = *reinterpret_cast<float*>(ctx.rax + 0x1BC);
                        float fObjectHeight = *reinterpret_cast<float*>(ctx.rax + 0x1C0);

#ifdef GBFR_DEVBUILD
                        // Probe diagnostic ([Debug - Backgrounds] Probe, dev builds
                        // only): one greppable PROBE-BG line per unique object id
                        // (first 512 - menus/lobby alone consume ~100 unique ids, and
                        // 64 slots were exhausted ~30s after boot before any combat
                        // VFX could appear; field-tested 2026-07-10) - ALL ids
                        // passing this site, not only w==3840, in
                        // case a target quad is authored at a non-3840 width. Capture
                        // workflow for un-whitelisted full-screen overlay quads (e.g.
                        // the Io charge-complete flash, GitHub issue #1): ids whose
                        // FIRST sighting lands at the artifact moment are the
                        // candidates to add to BackgroundWidthIDs. A single bool check
                        // when disabled; release builds compile this whole block out.
                        if (bBackgroundProbe)
                        {
                            constexpr int kBgProbeCap = 512;
                            static std::mutex bgProbeMtx;
                            static uint32_t bgProbeSeen[kBgProbeCap];
                            static int bgProbeCount = 0;

                            std::lock_guard<std::mutex> lock(bgProbeMtx);
                            bool bSeen = false;
                            for (int i = 0; i < bgProbeCount; ++i)
                            {
                                if (bgProbeSeen[i] == (uint32_t)iObjectID) { bSeen = true; break; }
                            }
                            if (!bSeen && bgProbeCount < kBgProbeCap)
                            {
                                bgProbeSeen[bgProbeCount++] = (uint32_t)iObjectID;
                                // Verdict mirrors the widen logic below.
                                const char* verdict = "not-widened";
                                if (fObjectWidth == (float)3840 &&
                                    std::find(BackgroundWidthIDs.begin(), BackgroundWidthIDs.end(), iObjectID) != BackgroundWidthIDs.end())
                                    verdict = "list-hit";
                                else if (bSpanAllBackgrounds && fObjectWidth == (float)3840 && fObjectHeight == (float)2160)
                                    verdict = "spanall";
                                LogInfo("PROBE-BG: #%d id=%u w=%.0f h=%.0f verdict=%s",
                                    bgProbeCount, (uint32_t)iObjectID, fObjectWidth, fObjectHeight, verdict);
                            }
                        }
#endif // GBFR_DEVBUILD

                        // If it is 3840px wide then it must span the entire screen
                        if (fObjectWidth == (float)3840)
                        {
                            // Check if object ID matches anything in the vector
                            if (std::find(BackgroundWidthIDs.begin(), BackgroundWidthIDs.end(), iObjectID) != BackgroundWidthIDs.end())
                            {
                                ctx.xmm1.f32[0] = (float)2160 * fAspectRatio;
                            }
                        }

                        if (bSpanAllBackgrounds)
                        {
                            if (fObjectWidth == (float)3840 && fObjectHeight == (float)2160)
                            {
                                ctx.xmm1.f32[0] = (float)2160 * fHUDAspectRatio;
                            }
                        }
                    });
            }
            else if (fAspectRatio < fNativeAspect)
            {
                static SafetyHookMid UIBackgroundsHeightMidHook{};
                // v2.0.2: base+0x29, right after "vmovss xmm4,[rax+0x1C0]" (height) - Lyall's base+0x28
                // landed on the last byte of that 8-byte instruction.
                UIBackgroundsHeightMidHook = safetyhook::create_mid(UIBackgroundsScanResult + 0x29,
                    [](SafetyHookContext& ctx)
                    {
                        static std::atomic<bool> logged{ false };
                        if (!logged.exchange(true)) LogInfo("FIRED: UIBackgrounds (height path)");

                        int iObjectID = *reinterpret_cast<int*>(ctx.rax + 0x1C4);
                        float fObjectWidth = *reinterpret_cast<float*>(ctx.rax + 0x1BC);
                        float fObjectHeight = *reinterpret_cast<float*>(ctx.rax + 0x1C0);

                        // If it is 3840px wide then it must span the entire screen
                        if (fObjectWidth == (float)3840)
                        {
                            // Check if object ID matches anything in the vector
                            if (std::find(BackgroundHeightIDs.begin(), BackgroundHeightIDs.end(), iObjectID) != BackgroundHeightIDs.end())
                            {
                                ctx.xmm4.f32[0] = (float)3840 / fAspectRatio;
                            }
                        }

                        if (bSpanAllBackgrounds)
                        {
                            if (fObjectWidth == (float)3840 && fObjectHeight == (float)2160)
                            {
                                ctx.xmm4.f32[0] = (float)3840 / fAspectRatio;
                            }
                        }
                    });
            }
        }
        else
        {
            LogError("MISS: UIBackgrounds pattern not found - feature disabled");
        }
    }

    if (bSpanHUD)
    {
        // [REWORKED 2026-07-10] Span HUD - three-layer menu/story filter, derived from
        // independent analysis of the v2.0.2 exe (build 0x6A3E573A) and cross-validated
        // against community ultrawide research.
        //
        // Site (pattern hit RVA 0x0261C638, hook at +0x1C - both unchanged since v1):
        //   +0x00 mov   rax,[rcx+0x108]      ; rcx = child element, rax = parent canvas
        //   +0x07 test  rax,rax / jz +0x26   ; jz jumps PAST +0x1C, so parent != null here
        //   +0x0C vmovss xmm2,[rax+0x1BC]    ; parent width  (3840 on a full canvas)
        //   +0x14 vmovss xmm0,[rax+0x1C0]    ; parent height (2160 on a full canvas)
        //   +0x1C vmovsd xmm1,[rax+0x194]    ; <<== hook here
        // Child struct (v2.0.2 layout, -0x38 vs v1): +0x19C px.x | +0x1A4 anchorA.x |
        // +0x1AC anchorB.x | +0x1BC w | +0x1C0 h | +0x1C4 id.
        //
        // Why the rewrite: the previous body was Lyall's v1 ID-gated logic - none of the
        // v1 object IDs ever appear on v2.0.2, and +0x194/+0x198 are normalized anchors,
        // not pixel offsets, so it was inert (ADR-0006 appendix). Replaced with logic
        // field-tested on this exact exe build:
        //   (A) refresh the game's nameplate scale global (see NameplateFix)
        //   (B) EdgeSnapIds / MoveIds (dev builds only): ini-driven per-child-id px.x
        //       overrides, applied BEFORE any blocklist decision so specific menu
        //       elements (e.g. corner button prompts) can be pushed to the true screen
        //       edge. Position only. Compiled out of release builds.
        //   (C) Combat Prompts recenter: children of host 2939675107 with 0.5/0.5 anchors
        //       and |px.x| >= 1600 -> px.x = capturedBase * fAspectMultiplier (wide only)
        //   (D) gameplay HUD root 1719602056: widen unconditionally
        //   (E) SpanAllHUD "register mode": widen every full-canvas (3840x2160) parent,
        //       EXCEPT fixed-anchor children of menu/story containers. Three block layers:
        //       parent id in kSpanHudBlocklist, parent id in the transitive menuTree
        //       (seeded with 3 menu roots; a marked parent marks its children on sight;
        //       never cleared), or child id in kSpanHudChildBlock. Stretch-anchored
        //       children (anchorA.x != anchorB.x) are widened even inside menus.
        // Widen writes are REGISTER-ONLY (wide: xmm2 = 2160*fHUDAspectRatio, narrow:
        // xmm0 = 3840/fHUDAspectRatio) - no struct memory is touched in the widen paths,
        // unlike the removed v1 marker-based struct writes.
        //
        // [Debug - Span HUD] Probe = true (dev builds only) additionally logs every
        // unique child id that flows through here (bounded, first 512) with geometry +
        // verdict - the workflow for discovering ids to feed into EdgeSnapIds /
        // MoveIds. Costs one bool check when disabled; release builds compile the
        // whole probe machinery out (probeLog becomes a no-op).
        //
        // Interaction with UIMarkersCanvas (ADR-0006): that hook rewrites the GLOBAL
        // canvas manager's source/current width to 2160*fAspectRatio. If the manager
        // object itself ever flows through this site as "parent", its width is no longer
        // 3840 and the full-canvas gate here simply skips it - which is correct either
        // way, the root is already widened. The intended targets of this gate are the 41
        // named 3840x2160 canvases, whose own W/H the ADR-0006 fix leaves untouched.
        //
        // Confidence: HIGH on site semantics (verified disasm, runtime-tested on this
        // build); MEDIUM on ID-list completeness - hence the span diagnostics
        // below, which name the first 20 unique widened child ids for post-test triage.
        uint8_t* HUDConstraintsScanResult = Memory::PatternScan(baseModule, "48 ?? ?? ?? ?? ?? 00 48 ?? ?? 74 ?? C5 ?? ?? ?? ?? ?? ?? 00 C5 ?? ?? ?? ?? ?? ?? 00 C5 ?? ?? ?? ?? ?? ?? 00 EB ??");
        if (HUDConstraintsScanResult)
        {
            HUDConstraintsScanResult += 0x1C; // hook offset (same as Lyall's v1: pattern + 0x1C)
            LogInfo("HIT: HUDConstraints: %s+0x%llx", sExeName.c_str(), ModOffset(HUDConstraintsScanResult));

            static SafetyHookMid HUDConstraintsMidHook{};
            HUDConstraintsMidHook = safetyhook::create_mid(HUDConstraintsScanResult,
                [](SafetyHookContext& ctx)
                {
                    static std::atomic<bool> logged{ false };
                    if (!logged.exchange(true)) LogInfo("FIRED: HUDConstraints");

                    uint8_t* child = reinterpret_cast<uint8_t*>(ctx.rcx);
                    uint8_t* parent = reinterpret_cast<uint8_t*>(ctx.rax);
                    // The site's test/jz at +0x07 jumps past +0x1C when rax is null, so
                    // parent is non-null whenever this runs; the check is defensive only.
                    if (!child || !parent)
                        return;

                    const uint32_t parentId = *reinterpret_cast<uint32_t*>(parent + 0x1C4);
                    const uint32_t childId = *reinterpret_cast<uint32_t*>(child + 0x1C4);

#ifdef GBFR_DEVBUILD
                    // Probe diagnostic ([Debug - Span HUD] Probe, dev builds only): one
                    // greppable line per unique child id (first 512 - same capacity
                    // bump as PROBE-BG: menu traffic alone can eat hundreds of unique
                    // ids before the interesting combat elements appear), logged at
                    // the point the verdict for this pass is known. Dedup means each
                    // id gets its FIRST verdict only. A single bool check when disabled.
                    auto probeLog = [&](const char* verdict)
                    {
                        if (!bHUDProbe)
                            return;
                        constexpr int kHudProbeCap = 512;
                        static std::mutex probeMtx;
                        static uint32_t probeSeen[kHudProbeCap];
                        static int probeCount = 0;

                        std::lock_guard<std::mutex> lock(probeMtx);
                        for (int i = 0; i < probeCount; ++i)
                        {
                            if (probeSeen[i] == childId) return;
                        }
                        if (probeCount >= kHudProbeCap)
                            return;
                        probeSeen[probeCount++] = childId;
                        LogInfo("PROBE: parent=%u child=%u wh=%gx%g anchors=%g/%g px=%g verdict=%s",
                            parentId, childId,
                            *reinterpret_cast<float*>(child + 0x1BC),
                            *reinterpret_cast<float*>(child + 0x1C0),
                            *reinterpret_cast<float*>(child + 0x1A4),
                            *reinterpret_cast<float*>(child + 0x1AC),
                            *reinterpret_cast<float*>(child + 0x19C), verdict);
                    };
#else
                    // Release: no probe machinery. The inline no-op keeps the verdict
                    // call sites below readable and lets the optimizer discard them.
                    auto probeLog = [](const char*) {};
#endif // GBFR_DEVBUILD

                    // (A) keep the game's nameplate scale global corrected on frames
                    // where the Nameplate hook's own site does not execute.
                    if (bFixNameplates && g_pNameplateScalar && fAspectRatio > fNativeAspect)
                        *g_pNameplateScalar = 1.0f / fAspectRatio;

#ifdef GBFR_DEVBUILD
                    // (B) EdgeSnapIds / MoveIds (wide only, dev builds only): ini-driven
                    // per-id px.x overrides, applied BEFORE any blocklist/menuTree
                    // decision so even menu children kept at 16:9 can be pushed to the
                    // true screen edge.
                    // Position only - the widen registers are never touched here.
                    // Same capture-base-then-rewrite style as the Combat Prompts map:
                    //   MoveIds     : px.x = base + explicit deltaX (overrides EdgeSnapIds)
                    //   EdgeSnapIds : px.x = base + sign(base) * (2160*fHUDAspectRatio - 3840)/2
                    //                 (the canvas half-widening delta; right-side elements
                    //                 move right, left-side ones left). A base of ~0 gives
                    //                 no side to push toward - left unchanged, warned once.
                    if (fAspectRatio > fNativeAspect && (iMoveIdCount > 0 || iEdgeSnapCount > 0))
                    {
                        bool bMatched = false;
                        bool bEdgeSnap = false;
                        float fMoveDelta = 0.0f;
                        for (int i = 0; i < iMoveIdCount; ++i)
                        {
                            if (uMoveIds[i] == childId) { bMatched = true; fMoveDelta = fMoveDeltas[i]; break; }
                        }
                        if (!bMatched)
                        {
                            for (int i = 0; i < iEdgeSnapCount; ++i)
                            {
                                if (uEdgeSnapIds[i] == childId) { bMatched = true; bEdgeSnap = true; break; }
                            }
                        }
                        if (bMatched)
                        {
                            static std::mutex moveMtx;
                            static uint32_t moveIds[kMaxMoveEntries * 2];
                            static float moveBase[kMaxMoveEntries * 2];
                            static int moveCount = 0;

                            std::lock_guard<std::mutex> lock(moveMtx);
                            float fPxX = *reinterpret_cast<float*>(child + 0x19C);
                            int idx = -1;
                            for (int i = 0; i < moveCount; ++i)
                            {
                                if (moveIds[i] == childId) { idx = i; break; }
                            }
                            if (idx < 0 && moveCount < kMaxMoveEntries * 2)
                            {
                                idx = moveCount++;
                                moveIds[idx] = childId;
                                moveBase[idx] = fPxX; // one-shot base capture
                                if (bEdgeSnap && fabsf(fPxX) < 1.0f)
                                    LogWarn("FIRED: HUDConstraints EdgeSnap id=%u base px.x=%g is ~0 - side unknown, left unchanged", childId, fPxX);
                                else
                                    LogInfo("FIRED: HUDConstraints %s id=%u base px.x=%g", bEdgeSnap ? "EdgeSnap" : "MoveIds", childId, fPxX);
                            }
                            if (idx >= 0)
                            {
                                float fBase = moveBase[idx];
                                if (bEdgeSnap)
                                {
                                    if (fabsf(fBase) >= 1.0f)
                                    {
                                        float fSnapDelta = ((float)2160 * fHUDAspectRatio - (float)3840) * 0.5f;
                                        *reinterpret_cast<float*>(child + 0x19C) = fBase + (fBase > 0.0f ? fSnapDelta : -fSnapDelta); // struct write [child+0x19C]
                                        probeLog("edgesnap");
                                    }
                                    // |base| < 1: no write (warned once at capture)
                                }
                                else
                                {
                                    *reinterpret_cast<float*>(child + 0x19C) = fBase + fMoveDelta; // struct write [child+0x19C]
                                    probeLog("move");
                                }
                            }
                        }
                    }
#endif // GBFR_DEVBUILD

                    // (C) Combat Prompts recenter (wide only): centered-anchor children of
                    // the combat-prompt host sitting >= 1600px out get their base px.x
                    // captured once, then rescaled by fAspectMultiplier every pass.
                    if (fAspectRatio > fNativeAspect && parentId == 2939675107u)
                    {
                        float fAnchorAx = *reinterpret_cast<float*>(child + 0x1A4);
                        float fAnchorBx = *reinterpret_cast<float*>(child + 0x1AC);
                        float fPxX = *reinterpret_cast<float*>(child + 0x19C);
                        if (fAnchorAx == 0.5f && fAnchorBx == 0.5f && fabsf(fPxX) >= 1600.0f)
                        {
                            static std::mutex promptMtx;
                            static uint32_t promptIds[16];
                            static float promptBase[16];
                            static int promptCount = 0;

                            std::lock_guard<std::mutex> lock(promptMtx);
                            int idx = -1;
                            for (int i = 0; i < promptCount; ++i)
                            {
                                if (promptIds[i] == childId) { idx = i; break; }
                            }
                            if (idx < 0 && promptCount < 16)
                            {
                                idx = promptCount++;
                                promptIds[idx] = childId;
                                promptBase[idx] = fPxX; // one-shot base capture
                                LogInfo("FIRED: HUDConstraints combat prompt id=%u px.x %g -> %g (x%g)",
                                    childId, fPxX, fPxX * fAspectMultiplier, fAspectMultiplier);
                            }
                            if (idx >= 0)
                            {
                                *reinterpret_cast<float*>(child + 0x19C) = promptBase[idx] * fAspectMultiplier; // struct write [child+0x19C]
                                probeLog("prompt-recenter");
                            }
                        }
                    }

                    // (D) gameplay HUD root: span unconditionally. This is the one v1 id
                    // community research still carries for this exe build; our 2026-07-10
                    // diagnostic never saw it, so it may be dead - harmless if absent, and
                    // the one-shot log tells us if it is alive after all.
                    if (parentId == 1719602056u)
                    {
                        static std::atomic<bool> rootLogged{ false };
                        if (!rootLogged.exchange(true)) LogInfo("FIRED: HUDConstraints gameplay HUD root matched - spanning to %g", (float)2160 * fHUDAspectRatio);

                        if (fAspectRatio > fNativeAspect)
                            ctx.xmm2.f32[0] = (float)2160 * fHUDAspectRatio;
                        else if (fAspectRatio < fNativeAspect)
                            ctx.xmm0.f32[0] = (float)3840 / fHUDAspectRatio;
                        probeLog("widen-root");
                    }

                    // (E) SpanAllHUD register mode with the three-layer filter
                    if (!bSpanAllHUD)
                    {
                        probeLog("skip-spanallhud-off");
                        return;
                    }

                    // menuTree: transitive menu-subtree marking. If the parent is marked,
                    // mark the child. Seeds = the three menu-root ids identified for this
                    // exe build. Grows for the process lifetime; NEVER cleared (intentional).
                    bool bInMenuTree;
                    {
                        static std::mutex menuMtx;
                        static std::unordered_set<uint32_t> menuTree = { 1465589452u, 141651223u, 584127281u };
                        std::lock_guard<std::mutex> lock(menuMtx);
                        bInMenuTree = menuTree.count(parentId) != 0;
                        if (bInMenuTree)
                            menuTree.insert(childId);
                    }

                    // Parent-id blocklist (story/menu containers) + child-id blocklist.
                    static const uint32_t kSpanHudBlocklist[8] = {
                        1579537302u, 584127281u, 141651223u, 3723338869u,
                        2229826448u, 1465589452u, 2464430819u, 368881640u };
                    static const uint32_t kSpanHudChildBlock[3] = { 3646400251u, 3659745599u, 178979338u };

                    const char* blockReason = bInMenuTree ? "skip-menutree" : nullptr;
                    if (!blockReason)
                    {
                        for (uint32_t id : kSpanHudBlocklist)
                        {
                            if (parentId == id) { blockReason = "skip-blocklist"; break; }
                        }
                    }
                    if (!blockReason)
                    {
                        for (uint32_t id : kSpanHudChildBlock)
                        {
                            if (childId == id) { blockReason = "skip-childblock"; break; }
                        }
                    }

                    // A block only suppresses FIXED-ANCHOR children (anchorA.x == anchorB.x
                    // - exact compare, NaN-safe like the site's own ucomiss); children with
                    // stretch anchors are widened even inside menus.
                    if (blockReason && *reinterpret_cast<float*>(child + 0x1AC) == *reinterpret_cast<float*>(child + 0x1A4))
                    {
                        probeLog(blockReason);
                        return;
                    }

                    // Only widen full-canvas parents (w==3840 && h==2160, exact compare).
                    if (*reinterpret_cast<float*>(parent + 0x1BC) != (float)3840 ||
                        *reinterpret_cast<float*>(parent + 0x1C0) != (float)2160)
                    {
                        probeLog("skip-not-fullcanvas");
                        return;
                    }

                    if (fAspectRatio > fNativeAspect)
                        ctx.xmm2.f32[0] = (float)2160 * fHUDAspectRatio;   // register-only: parent width
                    else if (fAspectRatio < fNativeAspect)
                        ctx.xmm0.f32[0] = (float)3840 / fHUDAspectRatio;   // register-only: parent height
                    else
                        return; // exactly 16:9 - nothing widened, skip diagnostics
                    probeLog("widen");

                    // Diagnostics: count widened children and log the first 20 unique ids
                    // (parent id, child id, child w/h) for post-test triage of bad ids.
                    {
                        static std::mutex diagMtx;
                        static uint32_t seenIds[64];
                        static int seenCount = 0;
                        static int uniqueWidened = 0;

                        std::lock_guard<std::mutex> lock(diagMtx);
                        bool bSeen = false;
                        for (int i = 0; i < seenCount; ++i)
                        {
                            if (seenIds[i] == childId) { bSeen = true; break; }
                        }
                        if (!bSeen)
                        {
                            ++uniqueWidened;
                            if (seenCount < 64)
                                seenIds[seenCount++] = childId;
                            if (uniqueWidened <= 20)
                                LogInfo("FIRED: HUDConstraints span #%d: parent=%u child=%u child %gx%g",
                                    uniqueWidened, parentId, childId,
                                    *reinterpret_cast<float*>(child + 0x1BC),
                                    *reinterpret_cast<float*>(child + 0x1C0));
                        }
                    }
                });
        }
        else
        {
            LogError("MISS: HUDConstraints pattern not found - feature disabled");
        }
    }
}

void NameplateFix()
{
    // [NEW 2026-07-10] Nameplate Fix - derived from independent analysis of the v2.0.2
    // exe (build 0x6A3E573A), cross-validated against community ultrawide research.
    //
    // Site (expected unique hit RVA 0x00847F6B; long, fully concrete pattern):
    //   +0x00 mov rcx,[rax+0x60]
    //   +0x04 vmovss xmm1,[rcx+0x9A0]        ; width
    //   +0x0C vmovss [g_hudW],xmm1           ; rip-relative game global
    //   +0x14 mov rcx,[rax+0x60]
    //   +0x18 vmovss xmm1,[rcx+0x9A4]        ; height
    //   +0x20 vmovss [g_hudH],xmm1
    //   +0x28 mov rax,[rax+0x60]
    //   +0x2C vmovss xmm7,[g_scale]          ; game global, ~1.0
    //   +0x34 vdivss xmm1,xmm7,[rax+0x9D0]   ; xmm1 = scale / hudProjectionAspect
    //   +0x3C vmovss [g_npScale],xmm1        ; <<== hook here (pattern+0x3C), before the store
    // Hook: xmm1 = (xmm7 != 0 ? xmm7 : 1.0) / fAspectRatio - divide by the LIVE screen
    // aspect (NOT 1.7778): [rax+0x9D0] is the HUD-projection aspect that our other hooks
    // force/alter, so re-deriving the quotient from the true aspect re-projects
    // world-anchored nameplates correctly. Register-only; the site's own store then
    // publishes the corrected value to the game's scale global.
    //
    // g_pNameplateScalar: the store target at +0x3C, resolved from the instruction's own
    // rip-relative disp32 at scan time (never hardcoded; byte-checked first). The Span
    // HUD hook refreshes it to 1/fAspectRatio every pass so the value stays corrected on
    // frames where this site does not run.
    //
    // Install gates: bFixNameplates ([Fix Nameplates] Enabled) AND fAspectRatio > 16:9.
    // Confidence: HIGH (concrete 0x3C-byte pattern, runtime-tested on this build).
    if (!bFixNameplates)
        return;
    if (fAspectRatio <= fNativeAspect)
    {
        LogInfo("Nameplate Fix: skipped (screen not wider than 16:9)");
        return;
    }

    uint8_t* NameplateScanResult = Memory::PatternScan(baseModule, "48 8B 48 60 C5 FA 10 89 A0 09 00 00 C5 FA 11 0D ?? ?? ?? ?? 48 8B 48 60 C5 FA 10 89 A4 09 00 00 C5 FA 11 0D ?? ?? ?? ?? 48 8B 40 60 C5 FA 10 3D ?? ?? ?? ?? C5 C2 5E 88 D0 09 00 00");
    if (NameplateScanResult)
    {
        LogInfo("HIT: Nameplate: %s+0x%llx (hook at +0x3C)", sExeName.c_str(), ModOffset(NameplateScanResult));

        // Resolve the game's scale global BEFORE hooking - the mid-hook rewrites the
        // bytes at +0x3C. Expected store: C5 FA 11 0D disp32 (vmovss [rip+disp32],xmm1,
        // 8 bytes) => target = site + 0x3C + 8 + disp32.
        if (NameplateScanResult[0x3C] == 0xC5 && NameplateScanResult[0x3D] == 0xFA &&
            NameplateScanResult[0x3E] == 0x11 && NameplateScanResult[0x3F] == 0x0D)
        {
            int32_t disp32 = *reinterpret_cast<int32_t*>(NameplateScanResult + 0x40);
            g_pNameplateScalar = reinterpret_cast<float*>(NameplateScanResult + 0x3C + 0x8 + disp32);
            LogInfo("Nameplate Fix: scale global cached at %s+0x%llx (Span HUD refreshes it to 1/%g = %g per pass)",
                sExeName.c_str(), ModOffset(reinterpret_cast<uint8_t*>(g_pNameplateScalar)),
                fAspectRatio, 1.0f / fAspectRatio);
        }
        else
        {
            LogWarn("Nameplate Fix: bytes at +0x3C are not the expected rip-relative vmovss store - per-frame refresh disabled, hook still installed");
        }

        static SafetyHookMid NameplateMidHook{};
        NameplateMidHook = safetyhook::create_mid(NameplateScanResult + 0x3C,
            [](SafetyHookContext& ctx)
            {
                static std::atomic<bool> logged{ false };
                if (!logged.exchange(true)) LogInfo("FIRED: Nameplate (scale %g -> %g)", ctx.xmm1.f32[0], (ctx.xmm7.f32[0] == 0.0f ? 1.0f : ctx.xmm7.f32[0]) / fAspectRatio);

                float fScale = ctx.xmm7.f32[0];   // the game global the site loaded at +0x2C
                if (fScale == 0.0f)               // upstream substitutes 1.0 for exact 0
                    fScale = 1.0f;
                ctx.xmm1.f32[0] = fScale / fAspectRatio;
            });
    }
    else
    {
        LogError("MISS: Nameplate pattern not found - nameplates stay mispositioned at ultrawide");
    }
}

void GraphicalTweaks()
{
    if (bShadowQuality)
    {
        // [FIXED v2.0.2] Shadow Quality
        uint8_t* ShadowQualityScanResult = Memory::PatternScan(baseModule, "8B ?? ?? ?? C4 ?? ?? ?? ?? C5 ?? ?? ?? ?? ?? ?? ?? C5 ?? ?? ?? ?? ?? ?? 00");
        if (ShadowQualityScanResult)
        {
            LogInfo("HIT: ShadowQuality: %s+0x%llx", sExeName.c_str(), ModOffset(ShadowQualityScanResult));

            static SafetyHookMid ShadowQualityMidHook{};
            // v2.0.2: the instruction is "42 8B 44 00 04" (mov eax,[rax+r8*1+0x04]) - the new REX.X
            // prefix 0x42 means the pattern hits 1 byte into the instruction, so hook at scan-0x1.
            // Addressing changed from v1's rcx+rdx to rax (row = qualityIndex*0x4C) + r8 (table base).
            ShadowQualityMidHook = safetyhook::create_mid(ShadowQualityScanResult - 0x1,
                [](SafetyHookContext& ctx)
                {
                    if (iShadowQuality > 2048)
                    {
                        *reinterpret_cast<int*>(ctx.rax + (ctx.r8 + 0x4)) = iShadowQuality;
                        *reinterpret_cast<int*>(ctx.rax + (ctx.r8 + 0x8)) = iShadowQuality;
                    }
                    *reinterpret_cast<int*>(ctx.rax + (ctx.r8 + 0xC)) = iShadowQuality;
                    *reinterpret_cast<int*>(ctx.rax + (ctx.r8 + 0x10)) = iShadowQuality;
                });
        }
        else
        {
            LogError("MISS: ShadowQuality pattern not found - feature disabled");
        }
    }

    if (fLODMulti != (float)1)
    {
        // [REFOUND v2.0.2] Level of Detail: v1 site was compiled away entirely (all byte-skeleton
        // permutations 0 hits). Semantically relocated to the per-object LOD threshold loop:
        // "vdivss xmm2, xmm0, [rsi+0x3BC]" computes the LOD distance ratio, then the loop
        // vucomiss-compares xmm2 against each LOD entry distance. Scaling xmm2 up keeps
        // high-detail models visible further away. Unique hit RVA 0x020E56C0, hook +0x8
        // (vdivss encodes as 8 bytes; xmm2 only holds the ratio after it). Confidence: MEDIUM -
        // the engine may have other LOD paths this does not cover; fallback candidate
        // RVA 0x0322ADDE (see hunt_lod_fpscap.txt).
        uint8_t* LODDistanceScanResult = Memory::PatternScan(baseModule, "C5 FA 5E 96 BC 03 00 00 48 C1 F8 04 48 83 C2 10 C5 FA 10 05");
        if (LODDistanceScanResult)
        {
            LogInfo("HIT: LODDistance: %s+0x%llx (hook at +0x8)", sExeName.c_str(), ModOffset(LODDistanceScanResult));

            static SafetyHookMid LODDistanceMidHook{};
            LODDistanceMidHook = safetyhook::create_mid(LODDistanceScanResult + 0x8,
                [](SafetyHookContext& ctx)
                {
                    ctx.xmm2.f32[0] *= fLODMulti;
                });
        }
        else
        {
            LogError("MISS: LODDistance pattern not found - feature disabled");
        }
    }

    if (bDisableTAA)
    {
        // [OK v2.0.2] Disable TAA
        uint8_t* TemporalAAScanResult = Memory::PatternScan(baseModule, "0F ?? ?? ?? 88 ?? ?? 48 ?? ?? ?? 48 ?? ?? ?? 48 ?? ?? ?? 48 ?? ?? ?? 48 ?? ?? ?? 5E");
        if (TemporalAAScanResult)
        {
            LogInfo("HIT: TemporalAA: %s+0x%llx", sExeName.c_str(), ModOffset(TemporalAAScanResult));
            // xor ecx, ecx
            Memory::PatchBytes((uintptr_t)TemporalAAScanResult, "\x31\xC9\x90\x90", 4);
            LogInfo("Temporal AA: Patched instruction to disable TAA.");
        }
        else
        {
            LogError("MISS: TemporalAA pattern not found - feature disabled");
        }
    }
}

void FPSCap()
{
    if (bFPSCap)
    {
        // [REFOUND v2.0.2] Raise FPS cap: the v2 frame limiter loads the target frame time
        // (double) from a 3-entry table (1/30, 1/60, 1/120 @ RVA 0x054D6BF0, indexed by the
        // fps menu setting) into xmm10, then busy-waits on vucomisd+pause. Unique hit
        // RVA 0x001B6E63; hook +0xC = right AFTER the 5-byte "vmovsd xmm10,[rax+rcx*8]"
        // (7-byte lea + 5-byte vmovsd) - hooking earlier would let the original load
        // overwrite our value. v1 used xmm6 at +0x5.
        uint8_t* FPSCapScanResult = Memory::PatternScan(baseModule, "48 8D 05 ?? ?? ?? ?? C5 7B 10 14 C8");
        if (FPSCapScanResult)
        {
            LogInfo("HIT: FPSCap: %s+0x%llx (hook at +0xC)", sExeName.c_str(), ModOffset(FPSCapScanResult));

            static SafetyHookMid FPSCapMidHook{};
            FPSCapMidHook = safetyhook::create_mid(FPSCapScanResult + 0xC,
                [](SafetyHookContext& ctx)
                {
                    // Menus seem to speed up beyond 240fps.
                    ctx.xmm10.f64[0] = (double)1 / 240;
                });
        }
        else
        {
            LogError("MISS: FPSCap pattern not found - feature disabled");
        }
    }
}

// One-shot diagnostic dump of every resolution-related location we know of
// (v2.0.2 RVAs from hunt_present_path.txt). Tells us which layer still thinks 16:9.
static void DiagDump()
{
    Sleep(20000);
    auto rd = [](uintptr_t rva) { return *reinterpret_cast<uint32_t*>((uintptr_t)baseModule + rva); };
    LogInfo("DIAG: globals fallback=%ux%u active_render=%ux%u src_render=%ux%u window_ref=%ux%u",
        rd(0x06B84080), rd(0x06B84084), rd(0x06B84088), rd(0x06B8408C),
        rd(0x06B84090), rd(0x06B84094), rd(0x06B84098), rd(0x06B8409C));
    LogInfo("DIAG: quality row idx=%u | row5 render=%ux%u window=%ux%u",
        rd(0x070364D0),
        rd(0x06B84210 + 5 * 0x4C + 0x24), rd(0x06B84210 + 5 * 0x4C + 0x28),
        rd(0x06B84210 + 5 * 0x4C + 0x2C), rd(0x06B84210 + 5 * 0x4C + 0x30));
    LogInfo("DIAG: swapchain=%ux%u | uiCachedW=%u", rd(0x07193038), rd(0x07193040), rd(0x07021290));
}

#ifdef GBFR_DEVBUILD
// [DEV 2026-07-10] DIAG-CAMDIST - camera-distance live watchdog (dev builds only).
//
// Why it exists: two static hunts for the live camera-distance path both produced
// hooks that HIT but never FIRED across full gameplay sessions (first the
// v1-relocated GameplayCamera message site, then the community-derived
// CamDistPreset x4 / FollowCamDist / RoamCamDist trio - see AspectFOVFix). The next
// hunt has to be observation-driven, so this watchdog runs on the Main thread after
// DiagDump and samples every ~2s, logging ONLY on change (bounded per stream):
//
//   1) The published preset-distance global [exe+0x07C25720] (float, meters,
//      default 4.8 - PATTERNS.md 2.5 / FOV_CAMDIST_SPEC Bug B). Interpretation:
//      - value NEVER changes while the in-game camera visibly zooms in/out
//        (lock-on, combat pull-back, indoor cameras) => the global is a COLD
//        preset copy, not the live distance; stop hunting its writers.
//      - value tracks visible camera-distance changes => the global IS live and
//        the preset-publish path just runs before our scan; re-hunt its writers
//        (e.g. hardware write-watch on 0x07C25720).
//
//   2) The most recent camera/view-params object cached by the AspectRatio /
//      ProjMatrixAspect hooks (g_DiagCamObj): +0x9D0 aspect, +0x9D4 FOV(radians).
//      These are the only two float fields FOV_CAMDIST_SPEC verifies on this
//      struct; the spec names NO distance field on it (the preset struct's +0x14
//      and the roam-config +0x38/+0x54 copies live on DIFFERENT structs), so no
//      distance-candidate window is scanned here - observation only, no guessing.
//
// Reads are VirtualQuery-guarded (same defensive practice as the pattern scanner,
// ADR-0002): the exe global is always committed, but the cached camera object can
// go stale between hook fire and watchdog tick.
static bool DiagSafeReadFloat(uintptr_t addr, float* out)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)))
        return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & kReadable))
        return false;
    *out = *reinterpret_cast<const volatile float*>(addr);
    return true;
}

static void DiagCamDistWatchdog()
{
    constexpr int kMaxLines = 64; // change-line budget PER STREAM per session
    const uintptr_t presetGlobal = (uintptr_t)baseModule + 0x07C25720;

    float fLastPreset = 0.0f;
    bool  bHavePreset = false;
    int   iPresetLines = 0;

    float fLastAspect = 0.0f, fLastFov = 0.0f;
    bool  bHaveCam = false;
    int   iCamLines = 0;

    LogInfo("DIAG-CAMDIST: watchdog armed - sampling preset-distance global %s+0x7C25720 "
        "(meters, default 4.8) and latest camera object (+0x9D0 aspect / +0x9D4 FOV) every 2s; "
        "log-on-change only, max %d lines per stream", sExeName.c_str(), kMaxLines);

    while (iPresetLines < kMaxLines || iCamLines < kMaxLines)
    {
        Sleep(2000);

        // Stream 1: preset-distance global.
        if (iPresetLines < kMaxLines)
        {
            float v = 0.0f;
            if (DiagSafeReadFloat(presetGlobal, &v) && (!bHavePreset || v != fLastPreset))
            {
                if (!bHavePreset)
                    LogInfo("DIAG-CAMDIST: preset global baseline %g (%s+0x7C25720; "
                        "watch whether this EVER moves when the in-game camera zooms)",
                        v, sExeName.c_str());
                else
                    LogInfo("DIAG-CAMDIST: preset global %g -> %g", fLastPreset, v);
                fLastPreset = v;
                bHavePreset = true;
                ++iPresetLines;
            }
        }

        // Stream 2: cached camera object fields. Compared by VALUE (not pointer)
        // so several camera objects alternating with identical aspect/FOV do not
        // burn the line budget; the pointer is still printed for context.
        if (iCamLines < kMaxLines)
        {
            const uintptr_t cam = g_DiagCamObj.load(std::memory_order_relaxed);
            float fAspect = 0.0f, fFov = 0.0f;
            if (cam
                && DiagSafeReadFloat(cam + 0x9D0, &fAspect)
                && DiagSafeReadFloat(cam + 0x9D4, &fFov)
                && (!bHaveCam || fAspect != fLastAspect || fFov != fLastFov))
            {
                if (!bHaveCam)
                    LogInfo("DIAG-CAMDIST: camera obj 0x%llx baseline aspect=%g fov=%g rad (%.1f deg)",
                        (unsigned long long)cam, fAspect, fFov, fFov * 180.0f / 3.14159265f);
                else
                    LogInfo("DIAG-CAMDIST: camera obj 0x%llx aspect %g -> %g | fov %g -> %g rad (%.1f deg)",
                        (unsigned long long)cam, fLastAspect, fAspect, fLastFov, fFov,
                        fFov * 180.0f / 3.14159265f);
                fLastAspect = fAspect;
                fLastFov = fFov;
                bHaveCam = true;
                ++iCamLines;
            }
        }
    }
    LogInfo("DIAG-CAMDIST: all change-line budgets exhausted - watchdog stopped");
}
#endif // GBFR_DEVBUILD

DWORD __stdcall Main(void*)
{
    Logging();
    ReadConfig();
    ApplyResolution();
    Sleep(iInjectionDelay);
    GraphicalFixes();
    AspectFOVFix();
    NameplateFix();     // before HUDFix: resolves g_pNameplateScalar, which the
                        // HUDConstraints hook reads once it starts firing
    HUDFix();
    GraphicalTweaks();
    FPSCap();
    DiagDump();
#ifdef GBFR_DEVBUILD
    // Dev builds: the Main thread has nothing left to do, so it becomes the
    // DIAG-CAMDIST sampling thread (returns once all change budgets are spent).
    DiagCamDistWatchdog();
#endif // GBFR_DEVBUILD
    return true;
}

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        HANDLE mainHandle = CreateThread(NULL, 0, Main, 0, NULL, 0);
        if (mainHandle)
        {
            SetThreadPriority(mainHandle, THREAD_PRIORITY_HIGHEST); // set our Main thread priority higher than the games thread
            CloseHandle(mainHandle);
        }
    }
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
