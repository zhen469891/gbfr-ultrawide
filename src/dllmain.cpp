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
// Alive on v2.0.2 (15):
//   [OK]    Resolution      (ApplyResolution)  - verified; 2 hits (two inlined copies), BOTH hooked at +0x25
//                                                (diagnostic: runtime showed hooks install but 21:9 not applied,
//                                                so we no longer assume the first copy is the live path)
//   [OK]    ScreenEffects   (GraphicalFixes)   - verified; hook at +0xB
//   [FIXED] UIBackgrounds   (HUDFix)           - base at +0x2F; >16:9 hook (base+0x0) now writes xmm1 (v2 loads width
//                                                into xmm1; xmm0 is overwritten right after), <16:9 hook moved
//                                                base+0x28 -> base+0x29 (v1 offset landed inside an 8-byte vmovss);
//                                                lambda struct offsets shifted -0x38
//   [FIXED] HUDConstraints  (HUDFix)           - hook at +0x1C unchanged, xmm2/xmm0 unchanged; lambda struct
//                                                offsets shifted -0x38
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
//   [REFOUND v2.0.2] UIMarkers   (HUDFix)       - new pattern (unique hit RVA 0x026812CD), hook +0xA -> +0x12,
//                                                lambda base register rcx -> rax; confidence: MEDIUM-HIGH
//                                                (semantic lineage, runtime marker check recommended)
//   [REFOUND v2.0.2] GfxCorruption1 (GraphicalFixes) - new pattern, 4 hits (branch arms of one function) and ALL
//                                                are hooked at +0x8 (was: first hit @ +0x0). Hooked value is
//                                                width/64, not width/32 as v1 assumed; Cygames' v2.0.2 code
//                                                already ceils the w/32 & h/30 lanes itself (new vroundss),
//                                                only lane0 (w/64) is still stored raw - our hook fixes it
//   [REFOUND v2.0.2] GfxCorruption2 (GraphicalFixes) - new pattern, 2 hits (alloc success/fallback arms), both
//                                                hooked at +0x8; same lane0 = width/64 fix as GfxCorruption1
//   [REFOUND v2.0.2 - PARTIAL] GameplayCamera (AspectFOVFix) - new pattern (unique hit RVA 0x009D8C70). CamDist OK:
//                                                hook at +0x8, xmm9 (was -0xE, xmm8). FOV multiplier NOT SUPPORTED:
//                                                v2's camera message is {id, dist, pitch, yaw} - the v1 FOV slot is
//                                                now a pitch angle, so no FOV hook is installed (would tilt camera)
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
// MISS on v2.0.2: none - all 15 patterns relocated. The one intentionally unsupported feature is
// the GameplayCamera FOV multiplier (see PARTIAL note above).
// =====================================

#include "stdafx.h"
#include "helper.hpp"
#include <inipp/inipp.h>
#include <safetyhook.hpp>

HMODULE baseModule = GetModuleHandle(NULL);

// Logger and config setup
inipp::Ini<char> ini;
std::string sFixName = "GBFRUltrawide";
std::string sFixVer = "0.1.0";
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
bool bAspectFix;
bool bFOVFix;
bool bShadowQuality;
int iShadowQuality;
float fLODMulti;
bool bDisableTAA;
bool bFPSCap;

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
        LogInfo("%s v%s loaded.", sFixName.c_str(), sFixVer.c_str());
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
    inipp::get_value(ini.sections["Fix Aspect Ratio"], "Enabled", bAspectFix);
    inipp::get_value(ini.sections["Fix FOV"], "Enabled", bFOVFix);
    inipp::get_value(ini.sections["Shadow Quality"], "Enabled", bShadowQuality);
    inipp::get_value(ini.sections["Shadow Quality"], "Value", iShadowQuality);
    inipp::get_value(ini.sections["Level of Detail"], "Multiplier", fLODMulti);
    inipp::get_value(ini.sections["Disable TAA"], "Enabled", bDisableTAA);
    inipp::get_value(ini.sections["Raise Framerate Cap"], "Enabled", bFPSCap);

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

                    // v2.0.2 semantic change (hunt_scalecrop.txt): this store is the view
                    // constant block's +0x59C, now a shader-consumed SCENE CROP FACTOR:
                    // 1.0 = uncropped (what the game computes at 16:9), (H*16/9)/W at wider
                    // aspect ratios (0.744186 at 21:9 = fill-width crop). v1's value
                    // (fAspectMultiplier = 1.34375) is wrong here and was itself contributing
                    // to the zoomed/cropped frame - write 1.0 so the scene spans the full
                    // window width uncropped.
                    if (fAspectRatio > fNativeAspect)
                    {
                        ctx.xmm0.f32[0] = 1.0f;
                    }
                    else if (fAspectRatio < fNativeAspect)
                    {
                        ctx.xmm0.f32[0] = 1.0f * fAspectMultiplier;
                    }
                });
        }
        else
        {
            LogError("MISS: ScreenEffects pattern not found - feature disabled");
        }
    }
}

void AspectFOVFix()
{
    if (bAspectFix)
    {
        // [REFOUND v2.0.2] Aspect Ratio: only change vs v1 is byte[2] 49 -> 48 - v1's
        // "mov rax,[r14]" is compiled as "mov rax,[rbx]" in v2.0.2 (register allocation
        // change only). Verified unique hit at RVA 0x00751089; hook at +0x11 (first vmovss)
        // unchanged. Camera struct unchanged: +0x9D0 = aspect, +0x9D4 = FOV (writer site
        // verified at RVA 0x00691D3A: vdivss w/h then vmovss [rdx+0x9D0]).
        uint8_t* AspectRatioScanResult = Memory::PatternScan(baseModule, "74 ?? 48 ?? ?? 48 ?? ?? ?? ?? ?? 00 48 ?? ?? 74 ?? C5 ?? ?? ?? ?? ?? ?? 00 C5 ?? ?? ?? ?? ?? ?? 00 C5 ?? ?? ?? ?? ?? ?? 00 C5 ?? ?? ?? ?? ?? ?? 00 C5 ?? ?? ?? ?? ??");
        if (AspectRatioScanResult)
        {
            LogInfo("HIT: AspectRatio: %s+0x%llx", sExeName.c_str(), ModOffset(AspectRatioScanResult));

            static SafetyHookMid AspectRatioMidHook{};
            AspectRatioMidHook = safetyhook::create_mid(AspectRatioScanResult + 0x11,
                [](SafetyHookContext& ctx)
                {
                    static std::atomic<bool> logged{ false };
                    if (!logged.exchange(true)) LogInfo("FIRED: AspectRatio");

                    *reinterpret_cast<float*>(ctx.rax + 0x9D0) = fAspectRatio;
                });
        }
        else
        {
            LogError("MISS: AspectRatio pattern not found - feature disabled");
        }

        // [NEW v2.0.2] Projection-matrix builder (hunt_present_path.txt): the hook above
        // feeds only a secondary consumer (culling/shared constants at 0x0216ABE0). The REAL
        // projection matrix is built earlier in the same function, at RVA 0x00750970:
        // dirty(+0x9DE)-triggered, xscale = 1/tan(fov/2)/aspect, writes camera +0x40/+0x100.
        // Data-driven 16:9 aspect writers (e.g. cutscene camera data via 0x00ECFA78) set the
        // dirty flag themselves, so their value always reaches the matrix first - overriding
        // xmm8 right after "vmovss xmm8,[r14+0x9D0]" (9 bytes, hence +0x9) covers every
        // camera and every aspect source.
        uint8_t* ProjMatrixScanResult = Memory::PatternScan(baseModule, "C4 41 7A 10 86 D0 09 00 00 C5 FA 10 3D");
        if (ProjMatrixScanResult)
        {
            LogInfo("HIT: ProjMatrixAspect: %s+0x%llx (hook at +0x9)", sExeName.c_str(), ModOffset(ProjMatrixScanResult));

            static SafetyHookMid ProjMatrixAspectMidHook{};
            ProjMatrixAspectMidHook = safetyhook::create_mid(ProjMatrixScanResult + 0x9,
                [](SafetyHookContext& ctx)
                {
                    static std::atomic<bool> logged{ false };
                    if (!logged.exchange(true)) LogInfo("FIRED: ProjMatrixAspect");

                    ctx.xmm8.f32[0] = fAspectRatio;
                });
        }
        else
        {
            LogError("MISS: ProjMatrixAspect pattern not found - 3D camera may stay 16:9");
        }
    }

    // [REFOUND v2.0.2 - PARTIAL] Gameplay Camera: the v1 pattern died because the sender
    // switched to an rbp frame (5-byte stores, was 6-byte rsp+SIB) and hoisted
    // "mov rax,[rsi+0x4358]" above the float loads. The new pattern anchors the three
    // load+store pairs plus the following test/jz; verified unique hit at RVA 0x009D8C70.
    //
    // Only HALF of the v1 feature can be restored here. The camera message this site
    // builds changed layout: v1 sent {id, dist, FOV, yaw}, v2.0.2 sends {id, dist, pitch,
    // yaw}. The old FOV slot (v1 hook2's xmm10, now loaded via xmm7 at pattern+0xD) carries
    // a pitch angle in radians - readers multiply it by 1/(2*pi) to count turns, negate it
    // via sign-bit XOR, and wrap it into [-pi, pi) - so a v1-style FOV hook here would tilt
    // the camera instead of zooming. The FOV hook is therefore intentionally NOT installed.
    // Closest substitute would be the camera-blackboard flush sites (the 4 CutsceneFOV
    // inline hits, fov in xmm2 lane0), but those also affect cutscenes - not adopted.
    uint8_t* GameplayCameraScanResult = Memory::PatternScan(baseModule, "C5 7A 10 ?? ?? ?? 00 00 C5 7A 11 ?? ?? C5 FA 10 ?? ?? ?? 00 00 C5 FA 11 ?? ?? C5 7A 10 ?? ?? ?? 00 00 C5 7A 11 ?? ?? 48 85 C0 74");
    if (GameplayCameraScanResult)
    {
        LogInfo("HIT: GameplayCamera: %s+0x%llx", sExeName.c_str(), ModOffset(GameplayCameraScanResult));

        static SafetyHookMid GameplayCamDistMidHook{};
        // v2.0.2: hook at +0x8 (instruction boundary before "vmovss [rbp-0x14],xmm9") -
        // distance is already loaded into xmm9 (v1: xmm8) and not yet written to the
        // message block. The multiplied value still goes through the game's own per-area
        // distance clamps (300cm indoor / 800cm general) afterwards.
        GameplayCamDistMidHook = safetyhook::create_mid(GameplayCameraScanResult + 0x8,
            [](SafetyHookContext& ctx)
            {
                // Run camera distance multiplier
                if (fCamDistMulti != (float)1)
                {
                    ctx.xmm9.f32[0] *= fCamDistMulti;
                }
            });

        // No gameplay FOV hook on v2.0.2 (see block comment above) - surface the
        // limitation instead of silently ignoring the user's settings. bFOVFix only ever
        // acted on gameplay FOV at <16:9, so wider-than-16:9 users are not warned.
        if (fFOVMulti != (float)1)
        {
            LogWarn("Gameplay FOV multiplier is NOT SUPPORTED on game v2.0.2 (the camera message no longer carries FOV) - setting ignored");
        }
        if (bFOVFix && (fAspectRatio < fNativeAspect))
        {
            LogWarn("Gameplay FOV vert- compensation is NOT SUPPORTED on game v2.0.2 (the camera message no longer carries FOV) - gameplay FOV stays uncorrected below 16:9");
        }
    }
    else
    {
        LogError("MISS: GameplayCamera pattern not found - feature disabled");
    }

    if (bFOVFix && (fAspectRatio < fNativeAspect))
    {
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
        if (fAspectRatio > fNativeAspect)
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
            uint8_t* CanvasScaleScanResult = Memory::PatternScan(baseModule, "C5 FA 59 05 ?? ?? ?? ?? C5 F2 59 0D ?? ?? ?? ?? C5 F2 5C C8");
            if (CanvasScaleScanResult)
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
            else
            {
                LogError("MISS: CanvasFitHeight pattern not found - UI stays fill-width (zoomed/cropped)");
            }
        }

        // [REFOUND v2.0.2] Fix markers being off - confidence MEDIUM-HIGH.
        // New pattern is unique, hit at RVA 0x026812CD; v1 prefix codegen is gone so the
        // lineage is semantic: world->screen marker projection (world anchor * camera matrix,
        // viewport transform, behind-camera fallback) combined with the marker canvas w/h.
        // Hook moved +0xA -> +0x12 (start of the 8-byte "vmovss xmm2,[rax+0x1BC]", after
        // three 6-byte AVX instructions); the fallback branch jmp also lands exactly there,
        // so both control-flow paths are covered. Base register changed rcx -> rax in v2
        // (rax = marker canvas object, loaded from [rsi+0x2410]; verified not clobbered).
        // If markers are still off at runtime, fallback candidates are RVA 0x04276C42
        // (anchor switch) and 0x026D9446 (canvas-ratio) - see hunt_uimarkers.txt.
        uint8_t* UIMarkersScanResult = Memory::PatternScan(baseModule, "C4 ?? ?? 21 ?? 10 C4 ?? ?? 21 ?? 30 C4 ?? ?? 0C ?? 04 C5 ?? ?? ?? BC 01 00 00 C5 ?? ?? ?? C0 01 00 00");
        if (UIMarkersScanResult)
        {
            UIMarkersScanResult += 0x12; // hook offset (v2.0.2: pattern + 0x12)
            LogInfo("HIT: UIMarkers: %s+0x%llx", sExeName.c_str(), ModOffset(UIMarkersScanResult));

            static SafetyHookMid UIMarkersMidHook{};
            UIMarkersMidHook = safetyhook::create_mid(UIMarkersScanResult,
                [](SafetyHookContext& ctx)
                {
                    static std::atomic<bool> logged{ false };
                    if (!logged.exchange(true)) LogInfo("FIRED: UIMarkers");

                    if (fAspectRatio < fNativeAspect)
                    {
                        *reinterpret_cast<float*>(ctx.rax + 0x1C0) = (float)2160 + fHUDHeightOffset;
                    }
                    else if (fAspectRatio > fNativeAspect)
                    {
                        *reinterpret_cast<float*>(ctx.rax + 0x1BC) = (float)2160 * fAspectRatio;
                    }
                });
        }
        else
        {
            LogError("MISS: UIMarkers pattern not found - feature disabled");
        }

        // [FIXED v2.0.2] Span backgrounds - struct offsets shifted -0x38, width now in xmm1,
        // <16:9 hook moved base+0x28 -> base+0x29 (v1 offset landed inside the 8-byte vmovss xmm4,[rax+0x1C0])
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
        // [FIXED v2.0.2] Spanned HUD - hook offset and xmm registers unchanged
        // (xmm2 = width from [rax+0x1BC], xmm0 = height from [rax+0x1C0]);
        // lambda struct offsets shifted -0x38 for the v2.0.2 UI object layout.
        uint8_t* HUDConstraintsScanResult = Memory::PatternScan(baseModule, "48 ?? ?? ?? ?? ?? 00 48 ?? ?? 74 ?? C5 ?? ?? ?? ?? ?? ?? 00 C5 ?? ?? ?? ?? ?? ?? 00 C5 ?? ?? ?? ?? ?? ?? 00 EB ??");
        if (HUDConstraintsScanResult)
        {
            HUDConstraintsScanResult += 0x1C; // hook offset (Lyall: pattern + 0x1C)
            LogInfo("HIT: HUDConstraints: %s+0x%llx", sExeName.c_str(), ModOffset(HUDConstraintsScanResult));

            static SafetyHookMid HUDConstraintsMidHook{};
            HUDConstraintsMidHook = safetyhook::create_mid(HUDConstraintsScanResult,
                [](SafetyHookContext& ctx)
                {
                    static std::atomic<bool> logged{ false };
                    if (!logged.exchange(true)) LogInfo("FIRED: HUDConstraints");

                    // Gameplay HUD = 1719602056
                    if (*reinterpret_cast<int*>(ctx.rax + 0x1C4) == (int)1719602056)
                    {
                        // Span
                        if (fAspectRatio > fNativeAspect)
                        {
                            ctx.xmm2.f32[0] = (float)2160 * fHUDAspectRatio;
                        }
                        else if (fAspectRatio < fNativeAspect)
                        {
                            ctx.xmm0.f32[0] = (float)3840 / fHUDAspectRatio;
                        }
                    }
                    // Guard & Lock-On
                    if (*reinterpret_cast<int*>(ctx.rax + 0x1C4) == (int)605904162)
                    {
                        // Offset
                        if (fAspectRatio > fNativeAspect)
                        {
                            *reinterpret_cast<float*>(ctx.rax + 0x194) = (float)-(((2160 * fHUDAspectRatio) - 3840) / 2);
                        }
                        else if (fAspectRatio < fNativeAspect)
                        {
                            *reinterpret_cast<float*>(ctx.rax + 0x198) = (float)-(((3840 / fHUDAspectRatio) - 2160) / 2);
                        }
                    }
                    // Dodge
                    if (*reinterpret_cast<int*>(ctx.rax + 0x1C4) == (int)3550204025)
                    {
                        // Offset
                        if (fAspectRatio > fNativeAspect)
                        {
                            *reinterpret_cast<float*>(ctx.rax + 0x194) = (float)(((2160 * fHUDAspectRatio) - 3840) / 2);
                        }
                        else if (fAspectRatio < fNativeAspect)
                        {
                            *reinterpret_cast<float*>(ctx.rax + 0x198) = (float)-(((3840 / fHUDAspectRatio) - 2160) / 2);
                        }
                    }

                    if (bSpanAllHUD)
                    {
                        if (*reinterpret_cast<float*>(ctx.rax + 0x1BC) == (float)3840 && *reinterpret_cast<float*>(ctx.rax + 0x1C0) == (float)2160 && *reinterpret_cast<int*>(ctx.rax + 0x1C8) != (int)1234)
                        {
                            // Span
                            if (fAspectRatio > fNativeAspect)
                            {
                                *reinterpret_cast<float*>(ctx.rax + 0x1BC) = (float)2160 * fHUDAspectRatio;
                            }
                            else if (fAspectRatio < fNativeAspect)
                            {
                                *reinterpret_cast<float*>(ctx.rax + 0x1C0) = (float)3840 / fHUDAspectRatio;
                            }
                            // Write marker
                            *reinterpret_cast<int*>(ctx.rax + 0x1C8) = (int)1234;
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

DWORD __stdcall Main(void*)
{
    Logging();
    ReadConfig();
    ApplyResolution();
    Sleep(iInjectionDelay);
    GraphicalFixes();
    AspectFOVFix();
    HUDFix();
    GraphicalTweaks();
    FPSCap();
    DiagDump();
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
