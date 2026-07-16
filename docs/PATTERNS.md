# PATTERNS.md — memory pattern reference & re-hunting guide (game v2.0.2)

This document is the maintainer's (human or AI) guide to **every memory pattern** in
`src/dllmain.cpp`: what each one does, the exact v2.0.2 pattern, where the hook lands and
what it touches, how it was located, and — most importantly — **how to relocate it when a
future game update breaks it again.**

The authoritative source of the *current* code is always `src/dllmain.cpp` and its
`=== PATTERN STATUS (game v2.0.2) ===` header block. This file records the *reasoning and
method* behind those patterns. Where this document and the source ever disagree, the
source wins.

---

## 1. Why patterns break, and the tooling used to fix them

### Why they break

A byte pattern (a "signature") is a short sequence of machine-code bytes, with `??`
wildcards for bytes that vary. It works only as long as the compiler emits that exact
instruction shape. When the game is recompiled for a new version, the optimizer can:

- **reallocate registers** (`mov rax,[r14]` → `mov rax,[rbx]` — different bytes, same
  meaning; this alone killed the v1 AspectRatio pattern);
- **re-order or hoist instructions** (a `mov` moved above the loads the pattern anchored
  on — killed the v1 GameplayCamera pattern);
- **switch a stack frame** from `rsp`-relative to `rbp`-relative (6-byte SIB stores become
  5-byte disp8 stores);
- **change branch encodings** (short `75 xx jnz` → near `0F 85 rel32` — killed v1 UIAspect);
- **inline or delete code entirely** (the v1 LOD site was optimized away completely);
- **shift struct layouts** — in v2.0.2 the UI object struct moved by **−0x38** (see §2).

Any of these turns a `HIT` into a `MISS`. The mod's per-feature `HIT`/`MISS`/`FIRED`
logging (`scripts\GBFRUltrawide.log`) is designed so that a single broken pattern disables
*only* its own feature and names the offset, making the next hunt easier.

### The offline analysis tool: `gbfr_analyze.exe`

Re-hunting is done **offline against the game exe on disk** with a small Zydis-based CLI
(`tools/gbfr_analyze.cpp` in this repo; build it to `gbfr_analyze.exe`). It never runs the
game and never attaches a debugger — it just reads `granblue_fantasy_relink.exe` bytes.
Three subcommands:

| Subcommand | Purpose |
| --- | --- |
| `disasm <fileOffsetHex> <numBytes>` | Zydis disassembly from a file offset; addresses are printed as **RVA**. Used to verify instruction boundaries and semantics before choosing a hook offset. |
| `scan <pattern with ?? wildcards>` | Byte-pattern scan over `.text`; prints each hit's fileOffset **and RVA**, plus a total count. Used to test candidate patterns for uniqueness. |
| `xref <targetRVAHex>` | Brute-force scan for rip-relative `disp32` references that resolve to a target RVA. Used to find every reader/writer of a global or constant. |

**Section → RVA mapping (v2.0.2, module timestamp `1782470458`):**

```
.text : RawPtr 0x400,  VirtAddr 0x1000   =>  RVA = fileOffset + 0xC00
.rdata:                                   =>  RVA = fileOffset + 0x1200
.text file-offset range: [0x400, 0x049AFE00)
```

`gbfr_analyze` bakes in `RVA = fileOffset + 0xC00` (`kRvaDelta`). All RVAs in this document
are module-relative and match the `HIT: <name>: exe+0x…` lines in the log. The `xref`
computation is `target = RVA(disp32 field) + 4 + disp32` (the standard rip-relative rule:
displacement is measured from the *end* of the instruction).

> **Caveat — `xref` only finds rip-relative `disp32` in the *last 4 bytes* of an
> instruction.** It misses immediate stores (`C6 05`/`C7 05 imm`) whose disp32 is not the
> trailing field, and it will surface `xchg`-form (`0x87`) opcodes as false matches. When
> confirming a *writer* of a global, dump and read the opcode with `disasm`, don't rely on
> `xref` alone.

Two supporting patterns are useful during a hunt and easy to reproduce with any hex/PS
tool: a **whole-file constant scan** (find a specific `float`/`double` bit pattern in
`.rdata`, e.g. `1/64`, `1/3840`, `1.7777778f`) and an **xref opcode filter** (narrow the
`xref` hits down to one form, e.g. only rip-relative `cmp`, opcodes `39`/`3B`). Several
patterns below were pinned exactly this way.

> **Do NOT blindly scan the whole image.** Relink ships with **SteamStub** DRM: the
> `.bind` section is packed/obfuscated and produces junk pattern hits. Scan `.text` only
> (as `gbfr_analyze` does — see [ADR 0002](adr/0002-scan-only-committed-executable-sections.md)),
> and treat hits outside the `[0x400, 0x049AFE00)` `.text` file range with suspicion.

---

## 2. v2.0.2 key findings (reusable anchors)

These are the stable landmarks the whole hunt leans on. When re-hunting after an update,
**re-confirm these first** — most are data (tables, struct offsets, constants) and survive
recompiles far better than code shapes.

### 2.1 The UI object struct shifted −0x38 vs v1

Every v1 UI-object offset moved down by 0x38 in v2.0.2:

| Field | v1 offset | v2.0.2 offset |
| --- | --- | --- |
| width | 0x1F4 | **0x1BC** |
| height | 0x1F8 | **0x1C0** |
| object ID | 0x1FC | **0x1C4** |
| offset X | 0x1CC | **0x194** |
| offset Y | 0x1D0 | **0x198** |
| our "spanned" marker | 0x200 | **0x1C8** |

This table alone re-derives the UIBackgrounds, HUDConstraints, UIMarkers and Span-HUD
lambdas. The width/height pair `0x1BC`/`0x1C0` (bytes `BC 01 00 00` / `C0 01 00 00`) is a
*very* strong scan anchor for UI code.

### 2.2 Resolution preset table (5 entries) — `.rdata`

- width table  @ RVA **0x054BEA78** = `{3840, 2560, 1920, 1600, 1280}`
- height table @ RVA **0x054BEA8C** = `{2160, 1440, 1080, 900, 720}`

Indexed by the clamped preset id (`[reg+0x3C]`, clamped to ≤4). The Resolution hook reads
`ecx=width`, `eax=height` from these. `ApplyResolution` derives the two table addresses at
runtime from hit #1's two `lea` disp32s (at +0x11 / +0x1B) and patches all 5 entries.

### 2.3 Quality table (6 rows) — global data

- base @ RVA **0x06B84210**, **6 rows, stride 0x4C**, static defaults all 1920×1080.
- per-row fields: **+0x24/+0x28 = render W/H**, **+0x2C/+0x30 = window W/H**.
- active-row index @ RVA **0x070364D0**.

The boot-time "apply saved settings" path switches the active row here — **not** through
the preset table — which is why the screen reverted to 16:9 until we also patched this.

### 2.4 Resolution globals — four adjacent W/H pairs

At RVA **0x06B84080 … 0x06B8409C** (each pair is W then H):

| RVA | meaning |
| --- | --- |
| 0x06B84080 / 84 | fallback (default) render W/H |
| 0x06B84088 / 8C | **window W/H** (relabeled 2026-07-11: consumed as window dims by the ShadowBufferComponent temp-texture sizing fn 0x02479B90; previously listed as "active render") |
| 0x06B84090 / 94 | render W/H (previously listed as "src render") |
| 0x06B84098 / 9C | window W/H second copy, published from quality-table row +0x2C/+0x30 (**also** the 1920/1080 that HUDConstraints reads; the fhd/4K asset decisions §2.10 read 0x06B84098) |

Other resolution globals: swapchain size @ 0x07193038 / 0x07193040, UI cached width @
0x07021290. (These are read by the one-shot `DiagDump` in `dllmain.cpp`.)

### 2.5 Camera struct offsets

- **+0x9D0 = aspect**, **+0x9D4 = FOV** (radians), **+0x9DE = projection dirty flag**.
- Aspect writer verified @ RVA 0x00691D3A (`vdivss w/h` → `vmovss [rdx+0x9D0]`).
- **True projection-matrix builder @ RVA 0x00750970**: dirty(+0x9DE)-triggered,
  `yscale = 1/tan(fov/2)`, `xscale = yscale/aspect`, writes camera `+0x40` / `+0x100`.
  This — not the AspectRatio hook — is what actually shapes the 3D frustum, so it hosts
  both the ProjMatrixAspect hook (§3.5) and the ProjMatrixFOV hook (§3.19). Data-driven
  16:9 writers set the dirty flag themselves, so overriding aspect/FOV right at the
  matrix builder covers every camera and aspect source.
- **`+0x9D4` has 14+ rip-visible writers** (RVAs 0x006923A1, 0x00947D98, 0x00947E15,
  0x00948128, 0x0094AE53, 0x0094D313, 0x0095F6E5, 0x00962E05, 0x009FBA27, 0x00B31BC7,
  0x013D8415, 0x02DA7BAA, 0x036BF05B, 0x036BF777 — plus register-indirect ones), several
  per-frame. **Never memory-multiply the FOV field** — the write is either overwritten or
  compounds every frame; multiply in a register at the final consumer instead (ADR-0011).
- **Camera preset struct** (source of the gameplay camera params): `+0x14` = distance
  (meters, default 4.8), `+0x18` = FOV (radians, default **0.8726646 = 50.0°**). Four
  inlined "apply active preset" sites publish these to the **global camera-params block**
  — dist → RVA **0x07C25720**, FOV → 0x07C25724 (§3.20). Block defaults initialized at
  boot from `.rdata 0x054A4B50` = `{4.8, 0.8726646, 0.1, 0}` (writer @ RVA 0x0022D4B6).
  These data anchors are the place to restart a hunt if the preset-publish patterns die.
  > **Correction (2026-07-10, CAMDIST_HUNT2):** this "block" is **mainView+0x3C0** — the
  > preset-publish *tail* of the static view ctx 0x07C25360 (§2.8) — and it is **COLD**:
  > zero rip-visible readers, constant 4.8 in every field session. It cannot carry the
  > live camera distance; the live path is the register-base eye/at writer 0x00691F60
  > (§2.9), shipping as CamDistCommit (§3.26). (The four rip-relative commit copies once
  > tracked here as §3.24 proved dead and were removed.)

### 2.6 Canvas fill-width mapping @ RVA 0x0015FAB8

Every named canvas (41 of them, all 3840×2160 units, center pivot) is mapped to the screen
by a single scale chosen here:

```
scaleX = windowRefW / 3840
scaleY = windowRefH / 2160
scale  = scaleX + (scaleY - scaleX) * t     with t HARDCODED to 0   => always fill-width
```

NOPing the lerp's `vmulss` turns this into `scaleX + (scaleY - scaleX) = scaleY` →
fit-height (the v1 look). This replaces the v1 "UIAspect" trick (see CanvasFitHeight below).

### 2.7 View constant block (+0x580 family) — producer @ RVA 0x020D0E20

The view CB (`[rcx+0x2A0]` at the producer's store sites) is filled by the producer
function at RVA **0x020D0E20** (callers @ RVA 0x0019DBAB and 0x001D73FA). Field map,
disasm-verified 2026-07-10 (all stored as floats):

| CB offset | value | source |
| --- | --- | --- |
| +0x580 | FOV (radians) | camera `+0x9D4` (§2.5) |
| +0x584 | FOV × `.rdata` const | `+0x580` value × `[0x054A6D84]` |
| +0x58C / +0x590 | render W/H | int globals 0x06B84088/8C (§2.4); conditionally replaced by `[0x054A4630]` when `[0x07032DE0]+0x65 & 1` |
| +0x594 / +0x598 | windowRef W/H | int globals 0x06B84098/9C (§2.4), **unconditional** `vcvtsi2ss` |
| +0x59C | scene crop factor | computed `vdivss` (the ScreenEffects hook site, §3.3) |

> **RVA correction (2026-07-10):** earlier notes placed the producer at 0x020D0DF0 —
> that address is an **unrelated wrapper**; the real producer entry is 0x020D0E20.

> **+0x59C disp32 scan mirage:** scanning `.text` for stores with `disp32 == 0x59C`
> yields 18 hits, but they belong to a **different struct family** whose +0x59C is the
> camera **near plane** — not view CBs. Only the §3.3 site (inside the producer above)
> writes the view CB's +0x59C; don't chase the other 18 when hunting crop-factor
> consumers/writers.
> **Correction (2026-07-10, CAMDIST_HUNT2):** earlier notes attributed the near-plane
> source to "the registry `[0x07C54830]` entry `+0xD2C`" — wrong on both counts.
> `[0x07C54830]` is a **generic engine singleton** (alloc size 0x63D10 @ RVA 0x000DBB33,
> vtable 0x054BD058, 500+ xrefs, always method-call access), NOT a camera registry, and
> the `+0xD2C/+0xD30/+0xD34` float triple actually hangs off **`[0x07C25220]`** (500+
> xrefs, per-frame render/scene context; triple read @ RVA 0x025EF8F2). The real camera
> registry is the view-context table in §2.8.

**Scene crop factor +0x59C:** the store the **ScreenEffects** hook sits on writes the
view CB's **+0x59C**, which in v2.0.2 is a shader-consumed **scene crop factor**: `1.0`
= uncropped (what the game computes at 16:9), `(H*16/9)/W` at wider aspects (0.744186
at 21:9 = fill-width crop). Writing `1.0` at >16:9 lets the 3D scene span the full
window width. `rax = [rcx+0x2A0]` is the view CB pointer at the hook site.

**windowRef pair +0x594/+0x598:** hosts the dev-only WindowRefOverride experiment
(§3.23) — theory: a shader sizes full-screen combat-flash quads as
`renderH * (+0x594 / +0x598)`, which is 16:9 whenever the pair holds 1920/1080.

### 2.8 The real camera registry: view-context table + main view ctx (CAMDIST_HUNT2, 2026-07-10)

Offline hunt #2 for the live camera-distance path (full spec: `CAMDIST_HUNT2.md`, session
scratchpad; condensed in the auto-memory `camdist-hunt-findings`). The headline: **live
camera distance is geometric** — the length of `eye − at` on the main view context — not a
hot scalar global. The gameplay follow camera's "cameraLength" is data-driven (Cygames
reflection fields `cameraLength_`, `cameraLengthMax_`, `lockOnLengthBattle_`,
`lockOnLengthChase_`, `AddCameraLength@BT`, …) and surfaces only as eye/at output.

- **View-context table** @ RVA **0x054BF400** (static, 8 slots × 8 bytes), indexed by the
  current view index `[0x07021320]`. Slot 0 = the **main view ctx, a STATIC object @ RVA
  0x07C25360**; slots 3..7 = aux views (0x0701ED30 / 0x072D8070 / 0x0701F160 / 0x0701F550 /
  0x0701F940, stride ≈ 0x3F0); slots 1, 2 = 0. The view-CB producer (§2.7) resolves the ctx
  as `mov edx,[0x07021320]; lea rax,[0x054BF400]; mov r8,[rax+rdx*8]` @ 0x020D0E20..31.
- **Main view ctx struct** (offsets proven on ctx0; static aliases for ctx0 in brackets):

| ctx offset | static (ctx0) | meaning |
| --- | --- | --- |
| +0x10 / +0x20 / +0x30 | 0x07C25370 / 80 / 90 | **committed EYE / LOOK-AT / up** (vec4) — the pair the view-CB producer reads every frame |
| +0x44 | 0x07C253A4 | float → CB +0x588 (near-plane candidate) |
| +0x60 | 0x07C253C0 | ptr → camera object (+0x9D0 aspect, +0x9D4 FOV — the §2.5 struct) |
| +0xD0 / +0xD8 / +0xE0 | 0x07C25430 / 38 / 40 | **behavior handle idx / object ptr / generation** (validated vs handle table `[0x070214E8]`) |
| +0x100 / +0x110 / +0x120 | 0x07C25460 / 70 / 80 | stage-1 eye/at/up — written by the active behavior's virtual `+0x2C8` |
| +0x140 / +0x150 | 0x07C254A0 / B0 | previous-frame committed eye/at (commit feedback) |
| +0x160.. +0x178 | 0x07C254C0.. | manual/free-cam override block (written with flag +0x3B1=1 @ 0x0201B5xx) |
| +0x310 / +0x320 / +0x330 | **0x07C25670 / 80 / 90** | **STAGED eye/at/up** — the last stop before commit |
| +0x3B1 | 0x07C25711 | byte flag "manual camera active" (photo/debug cam) |
| +0x3B3 | 0x07C25713 | byte flag set when eye/at recommitted from ctx (0x028D526D) |
| +0x3C0 / +0x3C4 | **0x07C25720 / 24** | preset publish tail (dist 4.8 / FOV 0.8726646) — **COLD**, zero rip-visible readers (the §3.20 target and the retired DIAG-CAMDIST stream) |

- **Commit routine** (four inlined copies — the presumed per-frame choke point at hunt-2
  time; §3.24): copies staged eye/at → committed and calls `0x007513C0(ctx)` after each
  store to rebuild the view. RVAs 0x01A2D8F3 / 0x01F4185F / 0x01FF3AAC / 0x0320150C.
  > **Correction (CAMDIST_HUNT3, 2026-07-11):** these four are **dead** (0 fires in the
  > field) — relocation-only staging copies, NOT the live path. The live per-frame writer
  > is the register-base function 0x00691F60 (§2.9), shipping as §3.26; §3.24 was removed.
- **Camera-behavior struct family** (`[ctx+0xD8]`; RTTI subclasses `CameraPhotoMode`,
  `CameraBattleCutscene`, `RouteFollowCamera@cy`, …): +0x40 type id, +0x1388 msg id,
  **+0x1398 distance (cm**, 300/800 clamps**)**, +0x139C secondary distance, +0x13A0 pitch
  (rad), +0x13A8 yaw (rad). Exhaustive writer scan: the ONLY dynamic scalar writer of
  +0x1398 is the id-6 message consumer @ **0x00A840E5** (v1's path; §3.25 counts its
  liveness) plus hardcoded `1000.0` scripted setters (12 `vbroadcastss [obj+0x1398]`
  position computes) — multiplying +0x1398 writers can NOT be the general fix.
- **Registry corrections vs earlier notes:** `[0x07C54830]` is a generic engine singleton,
  not the camera registry, and the `+0xD2C` triple belongs to `[0x07C25220]` (§2.7 note).

### 2.9 The LIVE register-base eye/at writer: CamDistCommit 0x00691F60 (CAMDIST_HUNT3, 2026-07-11)

Offline hunt #3 (full spec: `CAMDIST_HUNT3.md`, session scratchpad; auto-memory
`camdist-hunt-findings`). **This is the writer the first five disp32-xref hunts all
missed**, because it writes eye/at *register-relative* (`[rsi+0x10]` / `[rsi+0x20]`, rsi=ctx),
which no rip-relative cross-reference can see. It supersedes the §3.24 four-site commit
family (those are rip-relative COMMIT copies that field-testing proved **never fire** in
v2.0.2 gameplay — see §3.24 correction).

- **Writer function** @ RVA **0x00691F60** (`push rsi; push rbx; sub rsp,0x88; vmovaps
  [rsp+0x70],xmm6`). Entry gate `cmp byte [rcx+0xC0],0; jnz 0x0069255B` skips the whole
  body when a mode byte is set. `mov rsi,rcx` @ 0x00691F7C — **rsi = ctx for the whole body**.
- **The commit basic block** (eye/at both live in xmm0/xmm1; disasm-verified):

  ```
  0x00692487  C5 F8 58 8E 10 01 00 00   vaddps  xmm1, xmm0, [rsi+0x110]   ; at  = pivot + at-offset
  0x0069248F  C5 F8 58 86 00 01 00 00   vaddps  xmm0, xmm0, [rsi+0x100]   ; eye = pivot + eye-offset
  0x00692497  C5 F8 29 46 10            vmovaps [rsi+0x10], xmm0          ; COMMIT EYE  (ctx+0x10)  <- HOOK
  0x0069249C  C5 F8 29 4E 20            vmovaps [rsi+0x20], xmm1          ; COMMIT AT   (ctx+0x20)
  0x006924A1  C5 F8 28 86 20 01 00 00   vmovaps xmm0, [rsi+0x120]
  0x006924A9  C5 F8 29 46 30            vmovaps [rsi+0x30], xmm0          ; commit up (ctx+0x30)
  0x006924AE  48 89 F1                  mov rcx, rsi
  0x006924B1  E8 0A EF 0B 00            call 0x007513C0                   ; rebuild view matrix
  ```

- **Per-frame + main-view proven statically:** called 10× per frame from the fixed dispatch
  loop in fn 0x00231A00 @ 0x00231CB9..0x00231D2C, each `lea rcx,[ctx]; call 0x00691F60`. Call
  **#0 = `lea rcx,[0x07C25360]` = ctx0 main view** (rip-relative → definitely main); #1..#9 =
  aux views. Frame path 0x00153E10 → 0x00154A10 → 0x00231A00 (delta-time gated).
- **Hook target for the multiply (Form A, NOW WIRED and shipping):** mid-hook at the eye store
  **0x00692497**, guard `rsi == 0x07C25360`, `xmm0.xyz = xmm1 + (xmm0 − xmm1) × m` (keep w);
  installed only at m != 1.0. Commits once per view per frame → no idempotency guard needed.
  Verified 2026-07-11 (in-game FIRED, confidence HIGH) — see §3.26 and ADR-0013.
- **Statics:** ctx0 = 0x07C25360, live eye = 0x07C25370 (ctx+0x10), live at = 0x07C25380
  (ctx+0x20) — the same statics §2.8 lists, now with their live WRITER identified.

### 2.10 fhd/4K UI asset-set selection — five decision sites, all `windowW > 1920` (FHD_HUNT, 2026-07-11, issue #3)

Relink ships two UI asset sets (`ui/fhd/...` FHD vs `ui/...` 4K — see Nenkai/relink-modding).
The exe decides between them at **five** sites, and the axis is uniformly the **window WIDTH**
global `[0x06B84098]` (published from quality-table row +0x2C by the two xchg publish sites
§2.4 lists; no other writer among 274 classified rip references). Height is never consulted.

| # | Role | Decision RVA | Compare | >1920 (4K) branch selects | imm32 @ / file offset |
|---|------|-------------|---------|---------------------------|------------------------|
| 1 | UI texture pack path (fn 0x0253B630) | 0x0253B77D | `cmp eax,0x780; jg` | `"ui/{0}.wtb"` @ 0x0621FD2C (else `"ui/fhd/{0}.wtb"` @ 0x0621FD37) | 0x0253B77E / 0x0253AB7E |
| 2 | Movie directory (title bg video) | 0x0330C911 | `cmp eax,0x781; cmovl` | `"ui/movie"` (else `"ui/fhd/movie"`; (ptr,len) pairs @ 0x05CA0D38/48) | 0x0330C912 / 0x0330BD12 |
| 3 | Copyright screen image | 0x0045C3FB | `cmp eax,0x781; jb` | `"system/copyright/copyright_4K.png"` (std::string @ 0x071BB9E0) | 0x0045C402 / 0x0045B802 |
| 4 | UI texture scale factor → obj+0x2C | 0x00C3480C | `cmp eax,0x781; setnb` | 0.5f (else 1.0f; float pair @ 0x054A5298) — **must stay in lockstep with #1** | 0x00C3480D / 0x00C33C0D |
| 5 | ≤1920-only special scaling path (`index==6`) | 0x0394E7E4 | `cmp eax,0x780; jg` | skips the special path | 0x0394E7EB / 0x0394DBEB |

To force the 4K set unconditionally, patch each imm32: sites 1/5 `80 07 00 00 → 00 00 00 00`,
sites 2/3/4 `81 07 00 00 → 01 00 00 00` (branch structure unchanged). Reverse direction
(force FHD): imm32 → 0x7FFFFFFF.

**Issue #3 status note:** at 3840×1080 windowW = 3840, so all five sites consistently pick
the 4K set — statically the asset-set choice itself does NOT distinguish 3840×1080 from
5120×1440. A **height-gated UI sampling bias** mismatch was the follow-up suspect (§3.27) —
FHD-calibrated 0.25 whenever windowH ≤ 1080, so W>1920 ∧ H≤1080 mixes 4K assets with the
FHD bias — but forcing the 4K bias was field-FALSIFIED as the issue #3 cause. A full
height-axis sweep (all `cmp` forms vs 0x438/0x439, plus 0x2D0/0x5A0/0x870/0xF00) is in the
2026-07-11 session scratchpad `height_sweep.txt`; other height sites are shadow temp-texture
sizing (0x02479D53: H≥1080→1280×720 else 1024×576 — same branch for both 32:9 cases),
derived-buffer sizing (0x023A55E8), default-window clamps (0x00847EDC/EF9), movie source-size
followers (0x0330EF2B/F55D). Scanner tooling (FhdScan/XrefClass/AbsScan/FindFunc/Cmp81) lives
in the same scratchpad; rebuildable from gbfr_analyze + csc.

---

### 2.11 UI element ids are `XXHash32Custom(name)` — data-layer identification via GBFRDataTools

The runtime UI element id (struct **+0x1C4**, §2.1) is the **XXHash32-custom of the element's
authored name**. This turns "which element is id N?" from a live-probe grind into a data
lookup, and it survives game updates (names are stable), unlike the exe hook patterns.

- **Tool:** [Nenkai/GBFRDataTools](https://github.com/Nenkai/GBFRDataTools) (needs **.NET 10
  runtime**). Relevant commands: `extract -i <data.i> -o <dir> -f <path>`, `b-convert -i x.prfb
  -o x.yaml`, `hash-string -i <name>` (prints the id as hex — the authoritative oracle). The
  bundled `filelist.txt` (~400k paths) is greppable for the file that hosts an element.
- **Workflow to name an id:** grep `filelist.txt` for the likely `ui/layouts/...` file →
  `extract` its `.prfb` → read printable ASCII runs (element/locator names are stored verbatim:
  `root`, `root01`, `loc_*`, `guide_sub_*`, …) → `hash-string -i <name>` each → match id.
  **Caveat:** `.prfb` b-convert **fails** on files with custom `Controller*` components
  (`ControllerGuideCommand`, `ControllerTutorialCursor`, …) — so use the raw-strings path, not
  YAML. `.view.viewb` files *do* convert but carry only layout transforms + child `AssetPath`s
  (no name fields).
- **Hash algorithm** (to reproduce offline without the tool; from
  `GBFRDataTools.Hashing/XXHash32Custom.cs`): stock XXHash32 with init `h32=0x178A54A4`, lane
  seeds `0x2557311B/0x871FB76A/0x0133ECF3/0x62FC7342`, **ASCII** bytes, **no lowercasing**,
  block-loop bound `> 16` (not `>= 16`). KATs: `""` → `0x887AE0B0`, `root` → `0xFE1DAE6D`
  (= 4263358061, the menu-root parent seen dominating the Span-HUD probe logs — the validation
  vector). Note: FSM/generic *node* names use a different hash (CRC32 of the lowercased string,
  per `generic_node_names.txt`); this XXHash32-custom form is the one for UI element ids.
- **Named anchors (this mod's own adjusted elements, confirmed 2026-07-17):** combat-prompt host
  `2939675107` = `root01`; prompts `717607238` = `guide_sub_r_02`, `2156071331` = `guide_sub_l_02`,
  `3412483092` = `guide_sub_l_01` — all in `ui/layouts/hud/guide_sub01.prfb`; the gameplay HUD
  tree is assembled by `ui/layouts/hud/hud01.view.viewb`. The Span-HUD blocklist / childblock
  ids resolve to **generic story/menu container** names (confirming the §3.12 comment):
  `1579537302`=`loc_bg01`, `584127281`=`loc_chr01`, `2229826448`=`loc_list01`,
  `368881640`=`loc_title01`, `2464430819`=`loc_base01`, `3646400251`=`loc_guide02`,
  `178979338`=`loc_text01`. Because these are generic, one id recurs across many prefabs.
- **Scope:** GBFRDataTools is a *data-layer* aid (identification, reading authored layout, and
  update-resilient re-finding), adopted per **ADR-0016**, and complementary to — not a
  replacement for — the runtime ASI hooks. It does **not** unlock the issue #7 tutorial-highlight
  fix (that box is a separate custom-component subsystem; see **ADR-0017**).

---

## 3. Per-pattern reference (v1 lineage + v2 additions)

Confidence legend: **HIGH** = unique hit + verified semantics; **MEDIUM-HIGH** = unique +
semantic lineage, runtime check recommended; **MEDIUM** = verified boundary but engine may
have other paths, needs in-game confirmation.

### 3.1 Resolution (`ApplyResolution`)

- **Feature:** custom resolution — the preset→dimensions conversion.
- **Pattern:** `41 ?? ?? ?? 3C 04 B9 04 00 00 00 0F ?? ?? 0F ?? ??`
- **Hits / RVA:** 2 hits (two inlined copies) @ RVA 0x002167FE and 0x0021BE77; **both** hooked at **+0x25**.
- **Hook semantics:** override `ctx.rcx = iCustomResX`, `ctx.rax = iCustomResY` (width/height just read from the preset tables). +0x25 is the first store after the two table reads.
- **v1→v2:** hook offset unchanged, but v1 assumed only the *first* copy is live. Runtime testing showed both hooks fire (`game wanted 2560x1440`) yet the screen stayed 16:9 — a **third** copy (different register allocation) sizes the swapchain and our pattern can't see it. Hence the additional table/quality/publish patches below.
- **Hunt method:** `disasm` confirmed both hits read the same 5-entry preset tables (§2.2) via two `lea`s at +0x11 / +0x1B; the table contents are sanity-checked (`3840/1920/2160/1080`) before patching.
- **Confidence:** HIGH (for these two sites).

#### Resolution — preset table patch (F1)

- Patches all 5 entries of the width/height tables (§2.2) to the target resolution, covering every consumer that reads the tables at once. Addresses derived from Resolution hit #1's two `lea` disp32s; contents verified first.

#### Resolution late-apply fixes (v2 additions: F2 QualityTable, F3 ResPublish, F4 ResolutionAlt)

The boot-time "apply saved settings" path does **not** go through the preset table. It switches the active *quality-table* row instead, then republishes that row into the resolution globals — which is what reverted the screen to 16:9 on the second boot-time flicker.

- **F2 — QualityTable patch.** Pattern `48 6B C0 4C 4C 8D 05 ?? ?? ?? ?? 42 8B 44 00 04` (`imul rax,rax,0x4C; lea r8,[table]; mov eax,[rax+r8+0x04]`). Table base derived from the +0x7 disp32 (base = site+0xB+disp32). Row0 verified 1920×1080, then all 6 rows' +0x24/+0x28/+0x2C/+0x30 (§2.3) patched. **Anchor: the stride-0x4C indexing site.**
- **F3 — ResPublish (2 sites).** The two xchg-quad publish sites that copy a quality row into the resolution globals. Registers: `ecx=render W, edx=render H, r8d=window/UI W, eax=window/UI H`; the hook forces all four to ours.
  - ResPublish1: `42 8B 4C 28 24 42 8B 54 28 28 46 8B 44 28 2C 42 8B 44 28 30` @ RVA 0x006C126B, hook **+0x14**.
  - ResPublish2 (rbx-indexed encoding): `8B 4C 18 24 8B 54 18 28 44 8B 44 18 2C 8B 44 18 30` @ RVA 0x001C8F33, hook **+0x11**.
- **F4 — ResolutionAlt (S3/S4).** Two more inlined preset→dims copies whose prologue is `8B 41 3C` (not `41 8B ?? 3C`), invisible to the main pattern. `8B 41 3C 3C 04 B9 04 00 00 00 0F 42 C8 0F B6 C1`, hook **+0x24**. Data-covered by the table patch; hooking is belt-and-braces.
- **Hunt method:** `xref` on the resolution globals (§2.4) + `disasm` of the quality-table indexing site, the two publish sites, and the projection builder.

### 3.2 GfxCorruption1 / GfxCorruption2 (`GraphicalFixes`)

- **Feature:** fix graphical corruption at widths not divisible by 64 (e.g. 3440). The corrupted value is a packed float4 `{w/64, h/60, w/32, h/30}` stored to a cbuffer; lane0 (`w/64`) is stored raw. The hook ceils lane0.
- **Patterns** (both start at the 8-byte `vmulss xmm3, xmm3, [rip]` = `renderWidth * 1/64`, then the vinsertps triple that packs the float4, then `vmovaps [reg]`):
  - GfxCorruption1: `C5 E2 59 1D ?? ?? ?? ?? C4 E3 61 21 C0 10 C4 E3 79 21 C2 20 C4 E3 79 21 C1 30 C5 F8 29 00` — **4 hits** @ RVA 0x02487219 / 0x024872D0 / 0x02487557 / 0x02487672.
  - GfxCorruption2: `C5 E2 59 1D ?? ?? ?? ?? C4 E3 61 21 D2 10 C4 E3 69 21 C9 20 C4 E3 71 21 C0 30 C5 F8 29 00 C5 F8 28 05 ?? ?? ?? ?? C5 F8 29 40 10` — **2 hits** @ RVA 0x021A8D86 / 0x021A8E4E.
- **Hook:** **+0x8** (first `vinsertps`, 6 bytes, no rip-relative operand → safe to relocate). `ctx.xmm3.f32[0] = ceilf(ctx.xmm3.f32[0])`. **Every hit must be hooked** — they are the scaled/unscaled × alloc-success/fallback branch arms; which runs depends on runtime state. `ceilf` is idempotent on integral values, so over-hooking is harmless.
- **v1→v2:** v2.0.2's own code already ceils lanes 2/3 (a new `vroundss`), reordering the code and killing the v1 pattern; and the fixed value is `w/64`, not `w/32` as v1 assumed. So the mod's gate now checks both `w/32` and `w/64` fractional.
- **Hunt method:** relaxed v1 skeletons gave 200+ junk hits. The anchor was the `.rdata` constant **1/64 @ RVA 0x054AE7E0**: `xref 0x054AE7E0` returned exactly 8 refs (4 for F1 + 2 for F2 + 2 splines excluded). `disasm` around RVA 0x024871D3 reconstructed the full packing sequence and revealed that the v1 `10/20/30` bytes were `vinsertps` imm8 lane selectors, not `+0x10/+0x20/+0x30` stores.
- **Confidence:** semantics HIGH; branch-arm mapping MEDIUM (mitigated by hooking all sites + idempotent ceil).

### 3.3 ScreenEffects (`GraphicalFixes`) = scene crop factor

- **Feature:** scene crop factor (view CB +0x59C, §2.7) — makes the 3D scene span the full window width at ultrawide.
- **Pattern:** `C5 ?? ?? ?? 48 ?? ?? ?? ?? ?? 00 C5 ?? ?? ?? ?? ?? ?? 00 C3`
- **Hits / RVA:** unique @ RVA 0x020D117B; hook **+0xB** (RVA 0x020D1186 = `vmovss [rax+0x59C], xmm0`, `rax = [rcx+0x2A0]` = view CB).
- **Hook semantics:** `if aspect != 16:9 → xmm0 = fAspectMultiplier` (both directions; = aspect/(16/9)). **Corrected 2026-07-10 (ADR-0012):** the factor is consumed by BOTH the scene pass and full-screen combat-VFX quads (charge flashes / finisher overlays). The original v2 choice of `1.0` at >16:9 left those quads at 16:9 width — vertical bars exactly on the 16:9 boundaries (GitHub issue #1). Field A/B at 3440×1440 with the FOV confound removed showed the scene renders identically under 1.0 and 1.34375, so the earlier "fAspectMultiplier zooms the scene" reading was confounded.
- **v1→v2:** pattern/hook offset unchanged; the *semantic meaning* of the stored value changed (crop factor, not a scale).
- **Hunt method:** `disasm` of the store (`vmovss [rax+0x59C]`) plus shader-side reasoning showed +0x59C is consumed as a crop factor; the disassembly is `vdivss` → `vmovss [rax+0x59C]` → `ret`. The 16:9 value the game computes here is `(H*16/9)/W` (0.744186 at 21:9); the producer function is at RVA **0x020D0E20** (corrected 2026-07-10 — the previously recorded 0x020D0DF0 is an unrelated wrapper; full CB field map in §2.7). Beware the +0x59C disp32 scan mirage (§2.7) when re-hunting.
- **Confidence:** HIGH.

### 3.4 AspectRatio (`AspectFOVFix`)

- **Feature:** camera aspect written to camera `+0x9D0` (§2.5).
- **Pattern:** `74 ?? 48 ?? ?? 48 ?? ?? ?? ?? ?? 00 48 ?? ?? 74 ?? C5 ?? ?? ?? ?? ?? ?? 00` (…5× AVX loads).
- **Hits / RVA:** unique @ RVA 0x00751089; hook **+0x11** (RVA 0x0075109A, first `vmovss`).
- **Hook semantics:** `*(float*)(ctx.rax + 0x9D0) = fAspectRatio`, written before the game loads `[rax+0x9D0]` into xmm2.
- **v1→v2:** register-allocation only — v1 `mov rax,[r14]` became `mov rax,[rbx]`, i.e. **only byte[2] changed `49→48`**. Struct offset unchanged.
- **Hunt method:** anchored on the adjacent aspect/FOV loads. `scan "C5 FA 10 ?? D0 09 00 00 C5 FA 10 ?? D4 09 00 00"` (the `+0x9D0`/`+0x9D4` pair) → 1 hit; `disasm` of the writer @ RVA 0x00691D3A confirmed +0x9D0 semantics.
- **Confidence:** HIGH.

### 3.5 ProjMatrixAspect (`AspectFOVFix`, v2 addition)

- **Feature:** aspect at the **true** projection-matrix builder @ RVA 0x00750970 (§2.5). The AspectRatio hook only feeds a secondary consumer (culling/shared constants @ 0x0216ABE0), so this covers the actual 3D frustum.
- **Pattern:** `C4 41 7A 10 86 D0 09 00 00 C5 FA 10 3D` (`vmovss xmm8,[r14+0x9D0]` then a rip-relative load).
- **Hits / RVA:** unique; hook **+0x9** (after the 9-byte `vmovss xmm8,[r14+0x9D0]`).
- **Hook semantics:** `ctx.xmm8.f32[0] = fAspectRatio`. Because data-driven 16:9 writers set the dirty flag (+0x9DE) themselves, overriding here reaches the matrix regardless of aspect source.
- **ORDERING:** the ProjMatrixFOV pattern (§3.19) is this pattern extended through the `vmulss`/`call` bytes, and this hook's install at +0x9 rewrites bytes +0x9..+0x10 inside it. `AspectFOVFix()` scans all four aspect/FOV patterns (AspectRatio, ViewParamsFOV, ProjMatrixAspect, ProjMatrixFOV) before installing any of the four hooks. The installed hooks coexist — stolen ranges +0x9..+0x10 vs +0x1A..+0x1E don't overlap (the 9-byte `vmulss` between them stays intact).
- **v1→v2:** new in v2 (v1 had no separate projection hook — the single aspect write sufficed on v1's code).
- **Hunt method:** traced why AspectRatio alone left the 3D at 16:9 (that hook feeds a consumer *after* the matrix is built); `disasm` of 0x00750970 revealed the dirty-flag-triggered `1/tan(fov/2)/aspect` builder reading `+0x9D0` into xmm8.
- **Confidence:** HIGH.

### 3.6 GameplayCamera (`AspectFOVFix`) — REMOVED 2026-07-10

> **Status: REMOVED (dead site).** The v2-relocated pattern HIT (unique @ RVA 0x009D8C70)
> but the distance hook **never FIRED in any session** — ours or bug reporters' — because
> v2.0.2 gameplay no longer drives the camera through this message path. Community
> ultrawide research for this exact build reached the same conclusion (its equivalent
> pattern has 0 hits / never installs). The hook was deleted when the
> CamDistPreset / FollowCamDist / RoamCamDist families (§3.20–3.22) took over the
> distance multiplier: keeping it would double-multiply the distance if the message path
> ever revives. Everything below is retained as **historical reference** so the next
> game-version port doesn't rediscover this site the hard way.

- **Feature (historical):** gameplay camera-distance multiplier at the camera message-block builder.
- **Pattern:** `C5 7A 10 ?? ?? ?? 00 00 C5 7A 11 ?? ?? C5 FA 10 ?? ?? ?? 00 00 C5 FA 11 ?? ?? C5 7A 10 ?? ?? ?? 00 00 C5 7A 11 ?? ?? 48 85 C0 74` (three load+store pairs + `test/jz`).
- **Hits / RVA:** unique @ RVA 0x009D8C70; the distance hook was at **+0x8** (before `vmovss [rbp-0x14], xmm9`), `ctx.xmm9.f32[0] *= fCamDistMulti`. HIT, never FIRED.
- **FOV — the still-relevant trap:** v1's camera message was `{id, dist, FOV, yaw}`; v2.0.2 sends `{id, dist, pitch, yaw}`. The old FOV slot (loaded via xmm7 at pattern+0xD, camera field +0x13A0) is now a **pitch angle in radians** — readers multiply by 1/(2π) and wrap to [−π,π). A v1-style FOV hook here **tilts the camera** instead of zooming. The FOV multiplier lives at ProjMatrixFOV (§3.19); the distance multiplier at §3.20–3.22.
- **v1→v2:** stack frame changed `rsp`→`rbp` (6-byte SIB stores → 5-byte disp8), and `mov rax,[rsi+0x4358]` was hoisted above the float loads — killing the v1 pattern. Struct offsets (`+0x1398` dist, `+0x13A0` pitch, `+0x13A8` yaw) unchanged.
- **Hunt method (historical):** `scan "C5 7A 10 ?? ?? ?? ?? 00 C5 7A 11"` → 13 hits, narrowed by `disasm` to 0x009D8C70; cross-checked against consumer handler 0x00A840D7, the 300/800 cm clamp compares, and the distance-squared / atan2-yaw / ÷2π writers to prove the field roles (dist +0x1398, pitch +0x13A0, yaw +0x13A8).
- **Lesson:** a `HIT` proves bytes exist; only a `FIRED` proves the path executes — and even a FIRED does not prove visual effect (ADR-0011, §3.17).

### 3.7 CutsceneFOV (`AspectFOVFix`)

- **Feature:** cutscene FOV correction at <16:9 (only installed when `aspect < 16:9`, so 21:9 users are unaffected).
- **Pattern:** `48 ?? ?? ?? C5 ?? ?? ?? ?? ?? ?? 00 C5 ?? ?? ?? ?? ?? ?? 00 C5 ?? ?? ?? ?? ?? ?? ?? C5 ?? ?? ?? ?? ?? ?? ?? 00 48 ?? ??`
- **Hits / RVA:** 4 identical inline hits @ RVA 0x03118049 / 0x03118339 / 0x03118629 / 0x03118A30; **only the first is hooked**. Hook **+0x1C**.
- **Hook semantics:** `ctx.xmm2.f32[0] /= fAspectMultiplier`.
- **v1→v2:** hook moved **+0xC → +0x1C**. v2 loads xmm2 via `vmovaps xmm2,[rbp+0x110]` at +0x14; Lyall's +0xC ran *before* that load, so any xmm2 write was immediately overwritten. +0x1C is after the load, before the stores.
- **Hunt method:** `disasm` of all 4 hits showed identical `imul; vmovaps xmm0/xmm1/xmm2` sequences and located the true post-load boundary.
- **Confidence:** HIGH (boundary verified). The other 3 hits (cutscene-path copies) are candidates if the first ever proves inactive.

### 3.8 CanvasFitHeight (`HUDFix`, replaces v1 "UIAspect")

- **Feature:** UI fit-height — makes the 16:9 UI sit centered instead of overflowing (v1's UIAspect trick is the wrong tool for v2). Only when `aspect > 16:9`.
- **Pattern:** `C5 FA 59 05 ?? ?? ?? ?? C5 F2 59 0D ?? ?? ?? ?? C5 F2 5C C8`
- **Action:** **byte patch, not a hook.** Verify bytes at +0x18 are `C5 F2 59 CA` (`vmulss xmm1,xmm1,xmm2`, the canvas-scale lerp multiply, §2.6), then NOP 4 bytes → the lerp collapses to `scaleY` (fit-height). Works with the ScreenEffects crop-factor fix so 3D still spans full width.
- **v1→v2:** conceptual replacement — v1 forced the UI ortho width to 16:9; v2 needs the canvas-scale lerp neutralized instead.
- **Hunt method:** the crop was traced to the UI side (not the UI ortho matrix): `disasm` of the canvas mapping @ RVA 0x0015FAB8 exposed the scale lerp whose `t` is compiled to a constant 0 (`vxorps xmm2,xmm2,xmm2`), i.e. always fill-width. The `.rdata` constants `1/3840 @ 0x054A43A0` and `1/2160 @ 0x054A43A4` (the two `vmulss` operands) confirm scaleX/scaleY. On 16:9 `scaleX==scaleY` so the bug is invisible; at 21:9 the overflow ratio is exactly 1.34375.
- **Confidence:** HIGH.

### 3.9 UIAspect (`HUDFix`) — v1 pattern, now two sites

> Note: on v2.0.2 the *UI fit* is done by CanvasFitHeight (§3.8). "UIAspect" here refers to the v1 ortho-width recompute path, refound as the two sites below. It is documented for completeness / lineage; the shipped fix path is CanvasFitHeight.

- **Feature:** force UI ortho width to 16:9 (recompute-trigger + 10-object canvas loop).
- **P1 (byte-patch site):** `8B 05 ?? ?? ?? ?? 3B 05 ?? ?? ?? ?? 0F 85 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 01` @ RVA 0x00231BDD. Patch **P1+0xC**: `0F 85 rel32` → `90 E9 rel32` (force-jump; rel32 unchanged, target 0x00231F10). Equivalent to v1's `75→EB`.
- **P2 (hook site):** `8B 05 ?? ?? ?? ?? C5 F2 2A C0 89 05 ?? ?? ?? ?? C5 F2 2A 0D` @ RVA 0x00231F10, hook **P2+0xA** (RVA 0x00231F1A, after the 6-byte mov + 4-byte `vcvtsi2ss`). `ctx.xmm0.f32[0] = iCustomResY * fNativeAspect` (must be after +0x6 `vcvtsi2ss` or it's overwritten). **Both P1 and P2 must hit or the fix is skipped.**
- **v1→v2:** v1 short `75 xx jnz` became near `0F 85` (near jnz) + mov/cmp reorder.
- **Hunt method:** anchored on the HUD default globals 1920/1080 (§2.4, 0x06B84098/9C). `xref 06B84098` → 274 candidates; `classify_xrefs.ps1` (rip-relative `cmp`, opcode 39/3B) narrowed to the single `.text` resolution `cmp` @ RVA 0x00231BE3, which localized both sites.
- **Confidence:** HIGH.

### 3.10 UIMarkersCanvas (`HUDFix`) — replaces v1 "UIMarkers"

- **Feature:** world-anchored UI positioning (enemy HP bars, damage numbers, lock-on marker) offset horizontally at non-16:9. Uniform +440 px right shift at 3440×1440 = the pillarbox width (3440−2560)/2.
- **Root cause (v2):** world→screen widget positioning is inlined ~51× (all copies share the behind-camera guard const @ 0x054A5BD0 — `xref` it to enumerate them) and computes `canvasX = screenX/scale − canvasW/2` from the **global canvas manager** `[0x07C02358]` (scaleX +0x17C, −scaleY +0x180, scale +0x184, source W/H +0x1B4/+0x1B8, current W/H +0x1BC/+0x1C0). Rendering maps back through `windowW/2 + canvasX·scale`, so positions are only correct while **canvasW × scale == windowW**. Vanilla fill-width satisfies this by construction; the ADR-0004 fit-height patch (scale = H/2160) broke it.
- **Fix site:** the CanvasFitHeight pattern (§2.6) **+0x40** (RVA 0x0015FB08, `mov rsi,[rip+…]` right after the three scale stores; **rax = canvas manager**). Mid-hook writes **both** source and current fields: W (+0x1B4 and +0x1BC) = `2160*fAspectRatio` at >16:9; H (+0x1B8 and +0x1C0) = `3840/fAspectRatio` at <16:9. Byte-checked (`48 8B 35`) before hooking.
- **Why the source field matters:** +0x1BC/+0x1C0 are DERIVED — the dirty-layout recalc (RVA 0x0261C5D0, invoked for every dirty canvas from 0x02524B80 / 0x0214E641) copies +0x1B4/+0x1B8 over them each dirty frame. Writing only +0x1BC gets washed back to 3840 before combat; writing the source field makes the game's own recalc propagate 5160 for us. Runtime-verified: zero clobbers across a full combat session.
- **v1 "UIMarkers" is dead in v2:** the relocated per-widget site (RVA 0x026812CD) is a mode-gated corner-anchored one-off widget (unique −95/−30 px constants, one xref in the exe) that HITs but never FIREs in gameplay — hook removed.
- **Runtime-verified consumers** (breadcrumbs for the next port): positioner fn 0x02648970 ("HP-bar shaped", r14=mgr, W read @ 0x026489F4) and fn 0x02652B90 (rbx=mgr, W read @ 0x02652C27) both fired in combat reading W=5160; shared world→screen helper @ RVA 0x00962FD0.
- **Known residual risk:** off-screen culling reads the widget's own canvas node (`[widget+0x10]`, ±W/2·margin bounds @ 0x026D4FF0 family), not the manager — if world-anchored widgets vanish near the ultrawide edges, the hosting named canvas (41-node table @ 0x05A3CA70) needs the same width treatment.
- **Confidence:** **HIGH** (runtime-verified in combat at 3440×1440). See ADR-0006 for the full diagnosis.

### 3.11 UIBackgrounds (`HUDFix`)

- **Feature:** span full-screen backgrounds (fades, menus) to fill the screen, matched by object ID.
- **Pattern:** `41 ?? ?? ?? ?? 00 E8 ?? ?? ?? ?? 80 ?? ?? ?? 00 0F ?? ?? ?? ?? ?? 48 ?? ?? ?? ?? 48 ?? ?? E8 ?? ?? ?? ?? 48 ?? ?? ?? C5 ?? ?? ?? ?? ?? ?? 00`
- **Hits / RVA:** base @ RVA 0x03340AD5; `base += 0x2F` → RVA 0x03340B04.
  - **>16:9 (width) hook at base+0x0**: modify **xmm1** (v2 loads width into xmm1 via `vmovss xmm1,[rax+0x1BC]`; v1 modified xmm0 but xmm0 is overwritten by the next instruction). If `width==3840` and the ID is in `BackgroundWidthIDs`, `xmm1 = 2160 * fAspectRatio`.
  - **<16:9 (height) hook at base+0x29**: modify **xmm4** (right after `vmovss xmm4,[rax+0x1C0]`; v1's base+0x28 landed inside that 8-byte instruction). `xmm4 = 3840 / fAspectRatio`.
  - Struct offsets in the lambda: ID +0x1C4, width +0x1BC, height +0x1C0 (§2.1).
- **v1→v2:** struct shift −0x38; width register xmm0→xmm1; `<16:9` hook +0x28→+0x29 (v1's +0x28 landed on the last byte of the 8-byte `vmovss xmm4,[rax+0x1C0]`).
- **Hunt method:** `disasm` at base showed the width load into xmm1 and the mid-instruction boundary at +0x28.
- **Cross-verified 2026-07-10** against community ultrawide research for this exe build (0x6A3E573A): the ID lists (12 width / 11 height = width minus dialogue bg 2454207042), formulas and gating shape match ours exactly. A height hook at pattern+0x57 would land **mid-instruction** in this exe (an off-by-one seen in the wild, never exercised — it only installs at <16:9); the correct boundary is pattern+0x58 = our base+0x29, which we already use.
- **Probe diagnostic** (`[Debug - Backgrounds] Probe = true` in `GBFRUltrawide.dev.ini`; **dev builds only** — release builds compile the machinery out, per ADR-0010): the >16:9 width hook logs one greppable `PROBE-BG: #N id=… w=… h=… verdict=list-hit|spanall|not-widened` line per unique object id (first 64; **all** ids passing the site, not only w==3840, in case a target quad is authored at a non-3840 width). Costs one bool check when disabled. This is the capture workflow for full-screen overlay quads that still render as a centered 16:9 band because their id is missing from `BackgroundWidthIDs` — e.g. the Io charge-complete flash (GitHub issue #1): ids whose *first sighting* lands at the artifact moment are the candidates; whitelist every candidate with `w=3840`. The permanent fix is always an in-code id addition, never an ini knob.
- **Confidence:** HIGH.

### 3.12 HUDConstraints (`HUDFix`) — REWORKED 2026-07-10

- **Feature:** Span HUD — widen full-canvas HUD parents with a three-layer menu/story filter, recenter combat prompts, apply per-id position overrides (dev builds only), refresh the nameplate scale global. Derived from independent analysis of the v2.0.2 exe (build 0x6A3E573A), cross-validated against community ultrawide research; replaces Lyall's v1 ID-gated body, which was inert on v2 (none of the v1 object IDs appear; +0x194/+0x198 are normalized anchors, not pixel offsets — ADR-0006 appendix).
- **Pattern:** `48 ?? ?? ?? ?? ?? 00 48 ?? ?? 74 ?? C5 ?? ?? ?? ?? ?? ?? 00 C5 ?? ?? ?? ?? ?? ?? 00 C5 ?? ?? ?? ?? ?? ?? 00 EB ??`
- **Hits / RVA:** @ RVA 0x0261C638; hook **+0x1C** (RVA 0x0261C654). Site registers: **rcx = child element, rax = parent canvas** (non-null — the site's `test/jz` at +0x07 jumps past the hook), `xmm2 = parent width [rax+0x1BC]`, `xmm0 = parent height [rax+0x1C0]`.
- **Hook semantics** (child struct: +0x19C px.x, +0x1A4/+0x1AC anchorA.x/anchorB.x, +0x1BC w, +0x1C0 h, +0x1C4 id):
  1. **Nameplate refresh** (`bFixNameplates`, >16:9): `*g_pNameplateScalar = 1/fAspectRatio` (see §3.18).
  2. **EdgeSnapIds / MoveIds** (wide only; **dev builds only** — `#ifdef GBFR_DEVBUILD`, set via `build.ps1 -Dev` / CMake `GBFR_DEV=ON`; `GBFRUltrawide.dev.ini` `[Debug - Span HUD] EdgeSnapIds = id,id,…` / `MoveIds = id:deltaX,…`): per-child-id px.x overrides applied **before** any blocklist decision, so even blocklisted menu children can be pushed to the true screen edge. Capture base px.x on first sight (fixed-capacity, 32 ids per list, shared 64-slot base map), then every pass write `[child+0x19C] = base + delta` where delta is explicit (MoveIds, canvas units) or `sign(base) · (2160·fHUDAspectRatio − 3840)/2` (EdgeSnapIds — the canvas half-widening delta). MoveIds overrides EdgeSnapIds for the same id. `|base| < 1` in EdgeSnap mode = side unknown → left unchanged, warned once. **Position only** — the widen registers are never touched here.
  3. **Combat Prompts** (wide only): children of host **2939675107** with anchors 0.5/0.5 and `|px.x| ≥ 1600` → capture base px.x on first sight (fixed-capacity map, 16 slots), then write `[child+0x19C] = base * fAspectMultiplier` every pass.
  4. **Gameplay HUD root 1719602056**: span unconditionally (the one v1 id still carried by community research for this build; possibly dead on v2 — one-shot FIRED log tells).
  5. **SpanAllHUD register mode** (`bSpanAllHUD`): block if (parent id ∈ blocklist{1579537302, 584127281, 141651223, 3723338869, 2229826448, 1465589452, 2464430819, 368881640} OR parent id ∈ menuTree OR child id ∈ {3646400251, 3659745599, 178979338}) AND child anchorA.x == anchorB.x; menuTree = `unordered_set` seeded {1465589452, 141651223, 584127281}, transitively marks children of marked parents, never cleared. Then widen only full-canvas parents (w==3840 && h==2160 exact): wide → `xmm2 = 2160*fHUDAspectRatio`, narrow → `xmm0 = 3840/fHUDAspectRatio`. **Register-only** — no struct writes in the widen paths.
- **Probe diagnostic** (`[Debug - Span HUD] Probe = true` in `GBFRUltrawide.dev.ini`; **dev builds only** — release builds compile the probe machinery out, `probeLog` becomes a no-op): logs one greppable `PROBE: parent=… child=… wh=…x… anchors=…/… px=… verdict=…` line per unique child id (first 200, including skipped/blocklisted ones); verdicts: `edgesnap|move|prompt-recenter|widen-root|widen|skip-spanallhud-off|skip-menutree|skip-blocklist|skip-childblock|skip-not-fullcanvas`. Costs a single bool check when disabled. This is the discovery workflow for EdgeSnapIds/MoveIds candidates.
- **Interaction with UIMarkersCanvas (§3.10):** the global canvas manager's width is rewritten to 2160·aspect by that hook; if the manager ever appears as "parent" here its w≠3840 so the full-canvas gate skips it — correct either way (root already widened).
- **Diagnostics:** one-shot `FIRED: HUDConstraints`, per-unique-child `FIRED: HUDConstraints span #N: parent=… child=… …` (first 20) for post-test ID triage.
- **Hunt method:** `disasm` confirmed the +0x1C boundary; body semantics derived from independent runtime analysis of the v2.0.2 exe, cross-validated against community ultrawide research.
- **Confidence:** HIGH on site semantics (runtime-tested on this build); MEDIUM on ID-list completeness (hence the diagnostics).

### 3.13 ShadowQuality (`GraphicalTweaks`)

- **Feature:** override shadow-map resolution (quality-table row +0x4/+0x8/+0xC/+0x10).
- **Pattern:** `8B ?? ?? ?? C4 ?? ?? ?? ?? C5 ?? ?? ?? ?? ?? ?? ?? C5 ?? ?? ?? ?? ?? ?? 00`
- **Hits / RVA:** @ RVA 0x006A59E0; hook **scan−0x1** (RVA 0x006A59DF).
- **Hook semantics:** addressing is `rax` (row = qualityIndex*0x4C) + `r8` (table base 0x06B84210, §2.3). Writes `iShadowQuality` to `rax+r8+0x4/0x8/0xC/0x10` (only +0x4/+0x8 when >2048).
- **v1→v2:** the instruction is now `42 8B 44 00 04` (`mov eax,[rax+r8*1+0x04]`) — the **new REX.X prefix 0x42** means the pattern hits 1 byte into the instruction, so hook at scan−0x1. Addressing changed from v1's rcx+rdx to rax+r8.
- **Hunt method:** `disasm` of the quality-table indexing site; the table dump confirmed four int shadowmap fields per row.
- **Confidence:** HIGH.

### 3.14 TemporalAA (`GraphicalTweaks`)

- **Feature:** disable TAA.
- **Pattern:** `0F ?? ?? ?? 88 ?? ?? 48 ?? ?? ?? 48 ?? ?? ?? 48 ?? ?? ?? 48 ?? ?? ?? 48 ?? ?? ?? 5E`
- **Hits / RVA:** @ RVA 0x02165150; **byte patch** (not a hook).
- **Action:** patch 4 bytes `0F B6 48 10` (`movzx ecx,byte[rax+0x10]`) → `31 C9 90 90` (`xor ecx,ecx; nop; nop`), zeroing the TAA flag. `disasm` confirms the instruction is exactly 4 bytes so the patch is valid.
- **v1→v2:** unchanged.
- **Confidence:** HIGH.

### 3.15 LODDistance (`GraphicalTweaks`)

- **Feature:** LOD pop-in distance multiplier.
- **Pattern:** `C5 FA 5E 96 BC 03 00 00 48 C1 F8 04 48 83 C2 10 C5 FA 10 05`
- **Hits / RVA:** unique @ RVA 0x020E56C0; hook **+0x8** (after the 8-byte `vdivss xmm2,xmm0,[rsi+0x3BC]`).
- **Hook semantics:** `ctx.xmm2.f32[0] *= fLODMulti` — scaling the LOD-distance ratio keeps high-detail models visible further away. (v1 used xmm1.)
- **v1→v2:** the v1 site was **compiled away entirely** (all skeleton variants 0 hits). Semantically relocated to the per-object LOD threshold loop.
- **Hunt method:** every v1 skeleton permutation returned 0 hits, confirming the site was optimized away. Anchored instead on struct offset `+0x3BC` (the LOD base-distance denominator; `+0x3B8` is the numerator); `scan "C5 FA 5E 96 BC 03 00 00"` located the divide; `disasm` verified the loop (`vdivss` → repeated `vucomiss` compares).
- **Confidence:** **MEDIUM** — the engine may have other LOD paths this doesn't cover; needs in-game verification. **Fallback:** RVA 0x0322ADDE (a distance-scaling routine closest to the v1 byte shape: `vmulss` → store → `mov byte [rcx+0xF0],1`). **Do NOT** hook the quality-preset setup at RVA 0x006A59C0 — the same xmm0 is immediately reused to build a direction vector, so scaling it corrupts geometry.

### 3.16 FPSCap (`FPSCap`)

- **Feature:** raise the fps cap to 240 (experimental; physics may misbehave >30 fps). The in-game **120** option becomes 240; 30/60 keep their meaning.
- **Pattern:** `48 8D 05 ?? ?? ?? ?? C5 7B 10 14 C8` (`lea rax,[table]` + `vmovsd xmm10,[rax+rcx*8]`), unique @ RVA 0x001B6E63. Used only to LOCATE the table — **no hook is installed** (see below).
- **Mechanism (REWORKED 2026-07-11, GitHub issue #4 / ADR-0014):** pure **data patch**. The lea's rip-relative displacement (`Memory::GetAbsolute64(hit+3)`) resolves the 3-entry double frame-time table `{1/30, 1/60, 1/120}` @ RVA 0x054D6BF0; the `1/120` entry is verified and rewritten to `1/240`.
- **Why the v1-style mid-hook was removed:** full disasm of the main-loop function (RVA 0x001B5FE0..0x001BC3CB, `.pdata`-confirmed) showed the hook site +0xC (0x001B6E6F) is a **1-byte nop immediately followed by the spin-loop head at 0x001B6E70** — the loop's back-edge (`jmp` @ 0x001B6EC0) targets hookAddr+1, i.e. it jumps into the middle of safetyhook's 5-byte jmp. The hook was structurally unsafe on this build ever since the v1 port; field report (issue #4) was "no effect".
- **Why the data patch is sufficient:** exhaustive xref sweep (rip-relative disp32 over all exec sections + absolute-VA pointer scan over the whole file, 2026-07-11) found the lea at 0x001B6E63 is the table's **only** reader; xmm10 is never spilled and the table is re-read every frame. Index selection: `byte[[0x07C26B70]+0x3D]` (fps menu setting), 0→1/30, 1→1/60, ≥2→1/120. No branch bypasses the limiter (vsync just makes the first `vucomisd` pass immediately).
- **Why 240 and not higher:** the engine's timestep computes `timeScale = clamp(avgFrameSec × 60.0, 0.25, 3.0)` (4-frame ring buffer @ 0x0703F440). `1/240` sits exactly on the 0.25 bound — beyond 240 fps, game logic runs faster than real time. 240 is the hard ceiling for a correct-speed game.
- **Confidence:** HIGH — offline-verified end to end, and field-verified 2026-07-11 (local, 144 Hz: fps exceeds 120 with the data patch; the old v0.2.2 mid-hook build **crashes at launch** with the feature enabled, confirming the corrupted-back-edge analysis). Full 240 confirmation awaits a ≥240 Hz tester (issue #4).

### 3.17 ViewParamsFOV (`AspectFOVFix`, NEW 2026-07-10; ROLE CHANGE 2026-07-10)

> **Role change (ADR-0011):** this hook was shipped as "the gameplay FOV multiplier" and
> logged a genuine FIRED — but it **does not affect the rendered projection**. Its xmm3 is
> stored only to `[rsp+0x20]` and passed as the 5th argument to `0x0216ABE0`, the
> culling / shared-view-constants builder; the projection matrix (§2.5, RVA 0x00750970)
> re-reads the FOV **from memory** `[r14+0x9D4]`, out of this register's reach. The
> rendered FOV is served by **ProjMatrixFOV** (§3.19). This hook is **kept** as the
> culling-consistency companion: without it, `fFOVMulti > 1` culls objects against the
> narrower unmultiplied frustum and pops them at the screen edges.

- **Feature:** culling/shared-view-constants FOV consistency for `[Gameplay FOV] Multiplier` (companion of §3.19).
- **Pattern:** `C5 ?? ?? ?? A0 09 00 00 C5 ?? ?? ?? A4 09 00 00 C5 ?? ?? ?? D0 09 00 00 C5 ?? ?? ?? D4 09 00 00` — the four adjacent view-params loads (viewport W `+0x9A0`, viewport H `+0x9A4`, aspect `+0x9D0`, FOV `+0x9D4`; rax = view-params object from the preceding `mov rax,[rbx]`).
- **Hits / RVA:** expected unique @ RVA **0x0075109A**; hook **+0x20**, immediately after `vmovss xmm3,[rax+0x9D4]` loads the FOV. xmm3's only consumer is the `[rsp+0x20]` store feeding `call 0x0216ABE0`.
- **Hook semantics:** `ctx.xmm3.f32[0] *= fFOVMulti` — register only, no memory write. Installed only when `fFOVMulti != 1.0` (same gate as ProjMatrixFOV, so the two stay in lockstep).
- **No aspect hook at +0x10:** pattern+0x10 is a viable spot to force `[obj+0x9D0]`; we don't hook it — our AspectRatio hook (§3.4) is installed at this very RVA and already force-writes `+0x9D0` before the +0x10 load.
- **ORDERING IS LOAD-BEARING:** this pattern starts at the exact address the AspectRatio mid-hook patches, and the AspectRatio pattern (RVA 0x00751089, 0x37 bytes) spans the +0x20 hook site. Together with the §3.5/§3.19 overlap this is why `AspectFOVFix()` scans **all four** aspect/FOV patterns before installing **any** hook — a scan after install MISSes on the trampoline `jmp` bytes.
- **Confidence:** site/boundary HIGH (disasm- and runtime-verified); the culling-consistency benefit is MEDIUM-HIGH (0x0216ABE0's downstream not fully traced; harmless either way).

### 3.18 Nameplate (`NameplateFix`, NEW 2026-07-10)

- **Feature:** world-anchored nameplate horizontal scale at >16:9. Derived from independent analysis of the v2.0.2 exe, cross-validated against community ultrawide research.
- **Pattern (long, fully concrete):** `48 8B 48 60 C5 FA 10 89 A0 09 00 00 C5 FA 11 0D ?? ?? ?? ?? 48 8B 48 60 C5 FA 10 89 A4 09 00 00 C5 FA 11 0D ?? ?? ?? ?? 48 8B 40 60 C5 FA 10 3D ?? ?? ?? ?? C5 C2 5E 88 D0 09 00 00`
- **Hits / RVA:** expected unique @ RVA **0x00847F6B**; hook **+0x3C**, right after `vdivss xmm1,xmm7,[rax+0x9D0]` (xmm1 = scale / hudProjectionAspect) and right before the game stores xmm1 to its nameplate-scale global.
- **Hook semantics:** `s = xmm7; if (s == 0) s = 1; xmm1 = s / fAspectRatio` — divide by the **live** aspect (bss `fAspectRatio`), not 1.7778: `[rax+0x9D0]` is the HUD-projection aspect our other hooks force, so re-deriving from the true screen aspect re-projects nameplates correctly.
- **g_pNameplateScalar:** the store at +0x3C is `C5 FA 11 0D disp32` (8 bytes); the game global (≈RVA 0x07194FCC) is resolved at scan time as `site + 0x3C + 8 + disp32` (byte-checked first, **before** the mid-hook rewrites the site). The HUDConstraints hook (§3.12 step 1) refreshes it to `1/fAspectRatio` every pass, covering frames where this site doesn't run.
- **Install gates:** `bFixNameplates` (`[Fix Nameplates] Enabled`, default true) AND `fAspectRatio > 16:9`.
- **Confidence:** HIGH (0x3C bytes of concrete pattern; runtime-tested on this build).

### 3.19 ProjMatrixFOV (`AspectFOVFix`, NEW 2026-07-10)

- **Feature:** the **rendered** gameplay-FOV multiplier (`[Gameplay FOV] Multiplier`) — the visual half of the FOV feature (ADR-0011; §3.17 is the culling half). Applied inside the projection-matrix builder (§2.5, RVA 0x00750970), so it reaches the actual frustum. Design cross-derived from community ultrawide research for this exact build and re-verified instruction-by-instruction offline.
- **Pattern:** `C4 41 7A 10 86 D0 09 00 00 C5 FA 10 3D ?? ?? ?? ?? C4 C1 42 59 86 D4 09 00 00 E8` — the ProjMatrixAspect pattern (§3.5) extended through `vmovss xmm7,[rip→0.5]`, `vmulss xmm0,xmm7,[r14+0x9D4]` (xmm0 = FOV/2, read **from memory**) and the `call` opcode.
- **Hits / RVA:** expected unique @ RVA **0x00750970** (fileOffset 0x0074FD70); hook **+0x1A** = RVA 0x0075098A, the 5-byte `E8` call to tanf. safetyhook steals exactly the call and relocates its rel32; the hook body runs *before* the relocated call.
- **Hook semantics:** `ctx.xmm0.f32[0] *= fFOVMulti` ⇒ `tan((m·FOV)/2)` — exact angular multiply; `yscale = 1/tan(FOV/2)` and `xscale = yscale/aspect` both follow. Installed only when `fFOVMulti != 1.0`. **Register-only — never write `[obj+0x9D4]`** (14+ writers, §2.5: overwritten or compounding).
- **Scope:** the builder serves **every** camera whose dirty flag (+0x9DE) is set — gameplay, cutscenes, menu 3D scenes. No "is gameplay" discriminator exists at this site; the multiplier is **global by design**. A gameplay-only fallback (scale the preset-FOV publish at the §3.20 sites, preset field `+0x18` → global 0x07C25724) exists but is indirect and unverified — MEDIUM confidence only, use only if cutscene exclusion is ever demanded.
- **ORDERING IS LOAD-BEARING:** first 13 bytes of this pattern ARE the ProjMatrixAspect pattern, whose mid-hook at +0x9 rewrites bytes +0x9..+0x10. Scan all four aspect/FOV patterns before installing any hook (§3.5, §3.17). The two builder hooks coexist once installed (+0x9..+0x10 vs +0x1A..+0x1E; the 9-byte `vmulss` between them stays intact).
- **Hunt method:** root cause of the "FOV does nothing" report (issue #2) — `disasm` of the ViewParamsFOV site proved xmm3 feeds only `0x0216ABE0`; `disasm` of 0x00750970 showed the builder re-reads `[r14+0x9D4]` and the `E8` boundary at +0x1A. If this pattern dies in a future build, re-find the builder from the §2.5 anchors: `xref` a writer of `+0x9D4` or scan for the `0x9D0/0x9D4` load pair near a tanf call.
- **Confidence:** root cause HIGH (disasm-verified); site/pattern/boundary HIGH (offline-verified 1 hit); visual effect needs one eyes-on confirmation per the ADR-0011 lesson.

### 3.20 CamDistPreset (`AspectFOVFix`, NEW 2026-07-10)

> **Status update (2026-07-10, CAMDIST_HUNT2):** the publish target 0x07C25720 is
> **mainView+0x3C0 — a cold preset tail with zero rip-visible readers** (§2.8), and none
> of the three CamDist* families ever FIRED in the field. This family cannot affect the
> live distance; it is kept installed under the same gate purely as an early-warning
> canary (a FIRED line would mean a future patch revived the path — then check for
> double-scaling against the shipping hook). The live path turned out to be the
> register-base writer 0x00691F60 (§2.9), now shipping as **CamDistCommit (§3.26)**;
> the once-RANK-1 §3.24 commit sites proved dead and were removed.

- **Feature:** camera distance multiplier (`[Gameplay Camera Distance] Multiplier`) — primary family, replacing the dead GameplayCamera site (§3.6). Scales the preset distance at the moment it is published to the global camera-params block. Design cross-derived from community ultrawide research for this build, re-verified offline.
- **Pattern:** `C5 FA 10 41 14 C5 FA 11 05 ?? ?? ?? ?? C5 F8 28 41 30 C5 F8` — `vmovss xmm0,[rcx+0x14]` (rcx = active preset, +0x14 = distance in meters, default 4.8) + `vmovss [rip→0x07C25720],xmm0` (publish) + the start of the rest of the block copy.
- **Hits / RVA:** **4 hits** (verified offline) @ RVA **0x0095A91F / 0x01F9245F / 0x0268DA8F / 0x02DB617F** — four inlined copies of "apply active camera preset" (each iterates a preset list, picks the active entry via flag bytes +0x74/+0x69, then publishes). All four store to the **same** global 0x07C25720 (§2.5). **Hook ALL FOUR** (like GfxCorruption): which copy runs depends on game mode; over-hooking is harmless since each publish is scaled exactly once.
- **Hook offset / semantics:** **+0x5** (after the 5-byte load, before the 8-byte rip-relative publish store; safetyhook relocates the rip operand — same class as FPSCap/Nameplate). `ctx.xmm0.f32[0] *= fCamDistMulti`. The preset *source* field is never written, so re-publishing never compounds. Installed only when `fCamDistMulti != 1.0`.
- **Expected runtime behavior:** FIRED on area load / camera-mode change (base 4.8 → e.g. 5.76 at 1.2×); the log warns if the hit count differs from 4.
- **Hunt method / re-hunt:** anchor on the global 0x07C25720 (`xref` it) or the `.rdata` defaults block 0x054A4B50 (§2.5); the publish sites are the writers of the global.
- **Confidence:** HIGH (all four sites disasm-verified; defaults read from `.rdata`).

### 3.21 FollowCamDist (`AspectFOVFix`, NEW 2026-07-10)

- **Feature:** camera distance multiplier — follow-camera (combat/lock-on) zoom track. Companion family of §3.20.
- **Pattern:** `C5 7A 10 ?? 7C 01 00 00 EB ?? C4 41 38 57 C0 EB ?? C5 7A 10 ?? 80 01 00 00 EB ?? C5 7A 10 ?? 84 01 00 00 C4 41 30 57 C9` — a 1/2/3-channel track-evaluation selector (`popcnt` on a channel mask) loading the follow-cam zoom into **xmm8** from `[rdi+0x17C/+0x180/+0x184]`, or `vxorps xmm8` when no channel; all arms converge on `vxorps xmm9,xmm9,xmm9`.
- **Hits / RVA:** unique (verified offline) @ RVA **0x022897CF**; hook **+0x23** = RVA 0x022897F2, the 5-byte `vxorps xmm9` where all arms converge (boundary verified; next insn `mov rax,[rsi]` @ 0x022897F7 confirms a clean split).
- **Hook semantics:** `ctx.xmm8.f32[0] *= fCamDistMulti` (xmm8 may be 0 on the vxorps arm — harmless). Installed only when `fCamDistMulti != 1.0`.
- **Units/semantics:** this value is a **normalized [0..1] zoom/pull-back fraction**, not meters — downstream (verified) the game computes `dist = min(1.0, max(0, xmm8 + xmm7))` when flag `[obj+0x5B5C]&0x80` is set. A multiplier > 1 pulls back and **saturates at the far end of the zoom range** via that clamp; expected behavior, not a bug.
- **Expected runtime behavior:** FIRED continuously in combat / lock-on.
- **Re-hunt:** anchor on the `+0x17C/+0x180/+0x184` offset triple and the popcnt-selector shape.
- **Confidence:** MEDIUM-HIGH (boundary/semantics verified offline; exact gameplay coverage pending runtime confirmation).

### 3.22 RoamCamDist (`AspectFOVFix`, NEW 2026-07-10)

- **Feature:** camera distance multiplier — free-roam camera config copy. Companion family of §3.20.
- **Pattern:** `C5 FA 10 ?? 38 C5 FA 11 ?? 54 C5 FA 10 ?? 3C C5 FA 11 ?? 58` — copies free-roam camera config → camera object: `[rsi+0x38]` (distance) → `[rcx+0x54]`, then `[rsi+0x3C]` → `[rcx+0x58]` (second field, intentionally NOT scaled).
- **Hits / RVA:** unique (verified offline) @ RVA **0x0095A625** (same camera-apply function family as §3.20 hit #1); hook **+0x5** = RVA 0x0095A62A (the 5-byte store; boundary verified).
- **Hook semantics:** `ctx.xmm0.f32[0] *= fCamDistMulti` — scales only the copied value; the source `[rsi+0x38]` is untouched ⇒ no compounding. Installed only when `fCamDistMulti != 1.0`.
- **Expected runtime behavior:** FIRED when the free-roam camera (re)initializes.
- **Re-hunt:** anchor on the `+0x38/+0x54/+0x3C/+0x58` offset quad near the §3.20 hit #1 function.
- **Confidence:** MEDIUM-HIGH (boundary verified offline; gameplay coverage pending runtime confirmation).

### 3.23 WindowRefOverride (`[Debug - Scene]` in `GBFRUltrawide.dev.ini`, DEV-ONLY EXPERIMENT, NEW 2026-07-10)

- **Purpose:** experiment instrument #2 for the combat skill-flash 16:9 vertical-bars bug (GitHub issue #1; instrument #1 is `CropFactorOverride`, §3.3/ADR-0004 notes). **Not a shipping feature** — compiled only into dev builds (`build.ps1 -Dev`), ignored by release builds.
- **Theory under test (MEDIUM):** a shader sizes the full-screen combat-flash quad as `renderH * (CB+0x594 / CB+0x598)` (the §2.7 windowRef pair); if the pair holds 1920/1080 the quad is exactly 16:9 wide → vertical bars at the 16:9 boundaries at 3440×1440.
- **Open contradiction the FIRED log settles:** the runtime DIAG dump shows the int globals 0x06B84098/9C already patched to **3440/1440** (ResPublish, §3.1 F3) — the ratio would already be 2.389, killing the theory, **unless** the producer runs before the patch or reads another source. The one-shot FIRED line prints the INCOMING width the producer actually converted, so one launch decides.
- **Pattern (46 bytes, both rip disp32s wildcarded):** `C5 CA 2A 0D ?? ?? ?? ?? 48 8B 91 A0 02 00 00 C5 FA 11 8A 94 05 00 00 C5 CA 2A 0D ?? ?? ?? ?? 48 8B 91 A0 02 00 00 C5 FA 11 8A 98 05 00 00` — `vcvtsi2ss xmm1,xmm6,[rip→windowRefW]` + `mov rdx,[rcx+0x2A0]` + `vmovss [rdx+0x594],xmm1`, then the same triple for windowRefH → +0x598. The `+0x594`/`+0x598` disp32 bytes carry the semantics; the volatile rip disp32s are wildcarded.
- **Hits / RVA:** unique (verified offline with `gbfr_analyze scan`, 2026-07-10) @ RVA **0x020D10C5**, inside the §2.7 producer. Hook **+0x8** = RVA 0x020D10CD, immediately after the width `vcvtsi2ss` (xmm1 = incoming width float), landing on the 7-byte `mov` (no rip-relative operand → safe relocation).
- **Hook semantics (register-only):** `xmm1 = (float)windowRefH * fAspectRatio`, where windowRefH is re-read from the **same int global the +0x598 store consumes a few instructions later**, resolved at install time from the second `vcvtsi2ss`'s disp32 (pattern+0x1B, `GetAbsolute64`). Chosen over a hardcoded `1080 * aspect` because the disasm shows the +0x598 store is **unconditional** from that global (no `[0x07032DE0]+0x65` branch, unlike the render pair at +0x58C/+0x590): if the global holds 1440 at runtime, `1080·aspect` would make the pair's ratio 1.79 instead of the screen aspect and corrupt the experiment. This way `+0x594/+0x598 == fAspectRatio` regardless of whether the producer sees pre-patch (1080) or post-patch (1440) values.
- **Gating / diagnostics:** the pattern is scanned in **every dev build** regardless of the ini key — the `HIT: WindowRefOverride site` line is a pattern-survival canary. The install log also dumps both resolved globals **and their current int values** (post-injection-delay evidence of ResPublish state). The hook installs only when `[Debug - Scene] WindowRefOverride = true` (in `GBFRUltrawide.dev.ini`) AND aspect > 16:9. One-shot `FIRED: WindowRefOverride (CB windowRef W in: <incoming> -> <new>; windowRefH global: <H>)`.
- **How to read the result:** incoming **1920** → producer sees pre-patch values, theory alive (and if the bars vanish with the override on, the consumer is found); incoming **3440** → the pair was already screen-sized, this theory is dead regardless of visuals. Run with `CropFactorOverride = -1` so the two instruments don't confound.
- **Confidence:** site/boundary HIGH (offline-verified unique hit + instruction lengths); theory MEDIUM (that is what the experiment is for).

### 3.24 CamCommitDist (4-site rip-relative commit) — REMOVED 2026-07-11 (dead, superseded by CamDistCommit §3.26)

> **Status: REMOVED (dead site).** These four rip-relative COMMIT copies were the RANK-1
> live-path candidate from offline hunt #2 (CAMDIST_HUNT2, 2026-07-10; §2.8), but were
> **field-proven DEAD** — every DIAG-CAM2 session counted **0 fires** on all four sites across
> full gameplay + cutscenes. They are relocation-only staging copies (cutscene/replay staging
> paths) that do not run in v2.0.2 live play. The **live** per-frame eye/at writer is the
> *register-base* function **0x00691F60** (§2.9), which stores eye/at as `[rsi+0x10]`/`[rsi+0x20]`
> (rsi=ctx) — invisible to disp32 xrefs, which is why five prior hunts missed it. **The hook
> has been REMOVED from `src/dllmain.cpp` entirely** — it would double-multiply if it ever
> revived. No code hooks these sites anymore; the shipping multiplier lives at **§3.26**.
> Everything below is retained as **historical reference** for the next game-version port.

- **Feature (historical):** camera distance multiplier at the per-frame commit choke point. The theory: scale the committed eye about the look-at at the commit that every camera mode's output was thought to pass through. Disproven in the field — the real live writer is register-base (§2.9/§3.26).
- **Pattern (historical):** `48 8D ?? ?? ?? ?? ?? C5 F8 28 05 ?? ?? ?? ?? C5 F8 29 05 ?? ?? ?? ?? 48 89 ?? E8 ?? ?? ?? ?? C5 F8 28 05`
- **Hits / RVA (historical):** **exactly 4 hits** (offline-verified 2026-07-10) @ RVA **0x01A2D8F3 / 0x01F4185F / 0x01FF3AAC / 0x0320150C** — four inlined commit copies in four game-mode drivers; the removed hook was at **+0xF** (the 8-byte `vmovaps [rip→0x07C25370],xmm0` committed-eye store). Site layout: +0x00 `lea r?,[rip→0x07C25360]` (ctx0), +0x07 `vmovaps xmm0,[rip→0x07C25670]` (staged eye), **+0x0F committed-eye store (the old hook)**, +0x17 `mov rcx,r?`, +0x1A `call 0x007513C0`, +0x1F staged-at load (0x07C25680).
- **Why they were relocation-only staging copies:** each copy reads from and writes to the ctx0 *statics* by rip-relative disp32 (staged eye 0x07C25670 → committed eye 0x07C25370, etc.), the shape a relocation/staging path takes. Live gameplay instead drives the per-frame writer 0x00691F60 register-relative off `rsi=ctx`, so these disp32 copies never executed in play (0 fires every DIAG-CAM2 session).
- **ctx-layout sanity gate (historical):** when the (now-removed) hook installed, all four rip targets were resolved from each site's own disp32 fields and required `stagedEye == ctx0+0x310 && committedEye == ctx0+0x10 && stagedAt == stagedEye+0x10`, all sites agreeing on the staged look-at; a site failing the gate was skipped. This is the structural check to reuse if the register-base site (§3.26) ever needs re-anchoring against the statics.
- **Alternatives ruled out at the time (CAMDIST_HUNT2 §4, still valid):** RANK-2 MsgCamDist (consumer @ 0x00A840D7) — the id-6 message path proved dead too (§3.25 stream D); memory-multiplying behavior `+0x1398` (only writer is the consumer — FOV-lesson compounding, §2.5/ADR-0011); CB-producer displacement (desyncs rendering vs culling); preset-publish scaling (§3.20 — cold).
- **Lesson:** a register-base `[reg+disp]` writer is invisible to rip-relative `xref`; when a per-frame-changing static has no rip-visible writer, hunt via the dispatch loop / rebuild caller instead of `xref` on the static (ADR-0013). The active shipping hook is **CamDistCommit (§3.26)**.

### 3.25 DIAG-CAM2 (`[Debug - Camera] CamDiag` in `GBFRUltrawide.dev.ini`, DEV-ONLY OBSERVATION, NEW 2026-07-10)

- **Purpose:** observation instrumentation for the camera-distance hunt (CAMDIST_HUNT2/HUNT3) — produced the data that identified and validated the live writer now shipping as §3.26. **Not a shipping feature** (dev builds only, ADR-0010). Nothing is modified; all output is log-on-change with prefix `DIAG-CAM2:` and a shared 128-line budget.
- **Streams** (watchdog `DiagCam2Watchdog`, 1 s sampling on the Main thread; counters every 5 s):
  - **A — distances:** committed |eye−at| (statics 0x07C25370/0x07C25380) and staged |eye−at| (0x07C25670/0x07C25680), with running min/max. Units confirmed **meters** (§3.26: zoom tracked 0.679–6.566 m).
  - **C — behavior object:** `[0x07C25438]` → type id (+0x40), distance family +0x1398/+0x139C (guarded heap reads, bitwise change compare). Sampled for reference — CAMDIST_HUNT2 showed +0x1398 does NOT drive the live path.
  - **D — message liveness:** counting hook at the id-6 camera-message consumer, pattern `8B 46 18 89 87 88 13 00 00 C5 FA 10 46 1C C5 FA 11 87 98 13 00 00`, **unique @ RVA 0x00A840D7** (re-verified offline 2026-07-10), hook at **+0x0** (3-byte `mov eax,[rsi+0x18]`, count + last dist only, message never modified). Proved the id-6 message path dead (0 fires) — MsgCamDist ruled out.
  - **E — mode flags:** bytes `[0x07C25711]` (manual/photo cam) / `[0x07C25713]` (recommit) to label samples by camera mode.
  - **F — live-writer counters:** `fires_ctx0` / `fires_other` / `gateskip` / `|eye−at|` for the live register-base writer 0x00691F60, fed by the **shipping** 0x00692497 mid-hook itself (counting when `CamDiag` is set — §3.26) plus a dev-only entry counter on 0x00691F60 (`entries_ctx0`, so `gateskip = entries_ctx0 − fires_ctx0`). Report line `DIAG-CAM2: camcommit fires_ctx0=N fires_other=N gateskip=N (entries_ctx0=N) |eye-at|=min..max`. This replaces the old stream B (the four dead rip-relative commit counters, removed with the §3.24 hook).
- **Retired predecessor:** DIAG-CAMDIST (preset global 0x07C25720 + camera-object aspect/FOV streams) — the preset global is explained (cold ctx+0x3C0 tail, §2.8) and the FOV question was concluded by ADR-0011.
- **Statics are v2.0.2-pinned RVAs** (module timestamp 1782470458), same convention as `DiagDump`; re-derive from §2.8 after a game update.

### 3.26 CamDistCommit — the shipping camera distance multiplier (`CamDistCommit`, ACTIVE 2026-07-11)

- **Feature:** camera distance multiplier (`[Gameplay Camera Distance] Multiplier` → global `fCamDistMulti`) — **the active shipping hook** in both build flavors, wired into the §2.9 live register-base writer 0x00691F60. Supersedes the dead four-site CamCommitDist family (§3.24, now removed). Provenance "offline hunt + in-game FIRED verification 2026-07-11"; see ADR-0013.
- **Pattern:** `C5 F8 29 46 10 C5 F8 29 4E 20 C5 F8 28 86 20 01 00 00 C5 F8 29 46 30` — offline-verified UNIQUE (`scan` = exactly 1 hit) @ **RVA 0x00692497** (fo 0x00691897), hook offset **+0x0**. The pattern STARTS on the 5-byte `vmovaps [rsi+0x10],xmm0` committed-eye store (a clean VEX boundary); the following `vmovaps [rsi+0x20],xmm1` gives 10 relocatable bytes (> the 5 a rel32 detour needs; verified no code jumps into the range). At the hook: `rsi`=ctx, `xmm0`=eye (vec4), `xmm1`=at (vec4).
- **Hook semantics (Form A, register-only):** guard `rsi == baseModule+0x07C25360` (ctx0 — the static **main-view ctx only**; the 9 aux views are left untouched). When it matches, `xmm0.xyz = xmm1.xyz + (xmm0.xyz − xmm1.xyz) × fCamDistMulti`, preserving xmm0 lane 3 (w) — the committed eye is dollied about the look-at. **No memory is written** (per the ADR-0011 lesson: the committed eye is republished every frame, so a memory multiply would be overwritten or compound). Installed only when `fCamDistMulti != 1.0` (or, in dev builds, counting-only mode when `[Debug - Camera] CamDiag = true`). One commit per view per frame → **no idempotency guard needed**.
- **Gameplay-only by gate:** the writer function's entry gate `cmp byte [rcx+0xC0],0; jnz …` closes during cutscenes/dialogue, so the eye store is never reached then — this multiplier does **NOT** affect scripted cutscene/dialogue framing. Contrast **ProjMatrixFOV** (§3.19/ADR-0011), which sits at the projection-matrix builder *downstream* of any such gate and therefore DOES affect cutscenes. This gate-based gameplay-only scope is the key architectural distinction (deliberate, desirable).
- **Known limitation:** camera wall-collision is computed UPSTREAM of this commit, so a multiplier > 1 can clip/push the eye into walls near geometry (documented, not fixed — the same property v1's hook had).
- **Verification recorded (2026-07-11, in-game FIRED, confidence HIGH):** at 120 fps `fires_ctx0` fired every gameplay frame; `fires_other ≈ 9× fires_ctx0` (the 9 aux views, confirming the dispatch loop); `|eye−at|` tracked zoom over **0.679–6.566 m** (units confirmed meters); the `[rcx+0xC0]` gate closed ONLY in cutscenes/dialogue (gameplay-only proven).
- **FIRED lines:** shipping one-shot `FIRED: CamDistCommit (main view) - |eye-at| <before> -> <after> (x<m>, eye dollied about look-at)`; dev counting one-shot `FIRED: CamDistCommit counting ctx0 (main view) - |eye-at|=<d> (pre-multiply sample)`.
- **Dev-build DIAG-CAM2 (stream F):** in dev builds the **same single** `SafetyHookMid` on 0x00692497 also does the stream-F counting when `CamDiag` is set (splits `fires_ctx0` (rsi==0x07C25360) vs `fires_other`, computes ctx0 `|eye−at|` from the live xmm0/xmm1, min/max tracked). A **separate dev-only entry counter** on 0x00691F60 (`push rsi` function entry, `rcx`=ctx before the `[rcx+0xC0]` gate) counts `entries_ctx0`, so `gateskip = entries_ctx0 − fires_ctx0` measures gate closure. Report line: `DIAG-CAM2: camcommit fires_ctx0=N fires_other=N gateskip=N (entries_ctx0=N) |eye-at|=min..max`. (There is exactly ONE mid-hook on 0x00692497 — shipping multiply and dev counting share it.)
- **ctx0 resolution:** `g_pCamCtx0 = baseModule + 0x07C25360` at install (the static main-view ctx from §2.8/§2.9); only compared against `rsi`/`rcx`, never dereferenced.
- **Confidence:** **HIGH** (both patterns offline-verified unique on the deployed v2.0.2 exe, timestamp 1782470458; per-frame main-view liveness and the gameplay-only gate FIRED-verified in-game 2026-07-11). See ADR-0013.

### 3.27 UIAtlasBias — issue #3 candidate, **FALSIFIED & REMOVED 2026-07-11** (kept as hunt record)

- **Feature:** fixes the garbled/tiled title screen at "wide but ≤1080-tall" resolutions
  (issue #3: 3840×1080; 2560×1080 predicted affected too) by keeping the game's UI sampling
  bias consistent with its width-selected asset set (§2.10).
- **Mechanism found (FHD_HUNT follow-up, 2026-07-11):** inside the giant canvas/display
  function `[0x00154A10, 0x00162972)` (the same one hosting the §2.6 fill-width lerp /
  CanvasFitHeight site), at **RVA 0x0015FB70**: `cmp r15d(windowH), 0x439; jl` — H ≤ 1080
  stores **0.25f**, H ≥ 1081 stores **0.0f**, into `[[0x07BB5330]+0x48]` (a name-hash
  registry object; dirty flag +0x4C, rebuild fn 0x02525CE0). Consumer: RVA 0x03BA0BF9 folds
  `per-object[+0x30] + bias` into 4×float4 GPU constant records — a shader-side sampling
  offset. This is the exe's **only** windowH-1080-thresholded behavior switch (full sweep in
  `height_sweep.txt`, session scratchpad).
- **Why it garbles:** asset set is chosen by **width** (>1920 → 4K, §2.10) but this bias by
  **height**. All factory-reachable combos are consistent (1920×1080 = FHD set + 0.25;
  ≥1440p-tall = 4K set + 0.0). `W>1920 ∧ H≤1080` (only reachable modded) = 4K atlas + the
  FHD-calibrated 0.25 → atlas sampling lands off-grid → squeezed duplicate copies with black
  gaps on the title screen. 5120×1440 gets 4K + 0.0 → clean, matching the field report.
- **Patch (no hook):** pattern `41 81 FF 39 04 00 00 7C ??` (`cmp r15d,0x439; jl`), expected
  unique @ 0x0015FB70 (`PatternScanAll` size==1 guard); `Memory::Write` imm32 at +0x3:
  `0x439 → 1` (jl never takes → bias stays 0.0). Applied only when
  `iCustomResX > 1920 && iCustomResY <= 1080`, so all factory setups keep factory behavior.
  Reverse experiment (force 0.25): `7C 06 → EB 06` at +0x7.
- **VERDICT (2026-07-11, local repro at 3840×1080): FALSIFIED.** With the patch applied
  before the resolution flip (an ordering subtlety: the bias store only re-runs on a
  resolution change, so the first attempt in `GraphicalFixes` landed a second too late and
  was inconclusive), a `DiagDump` readback confirmed `bias = 0` live at title time — and
  the tiling was unchanged. The bias and its width/height axis mismatch are real but are
  NOT the cause of issue #3. Patch removed; section kept so the hypothesis is not
  re-hunted. Root cause was instead found to be the downsampled blur buffer (§3.28).

### 3.28 GaussBufferSize — issue #3 candidate, **FALSIFIED & REMOVED 2026-07-11** (kept as hunt record)

- **FIELD VERDICT (2026-07-11, local repro at 3840×1080):** the renderH-compare patch below
  DID resize the buffer (log `DIAG: gauss_buffer=1920x1080`, identical to what the working
  5120×1440 gets) but the tiling behind modals was **unchanged**. Buffer SIZE is therefore
  NOT the cause — 3840×1080 and 5120×1440 now share the same 1920×1080 buffer yet only the
  former tiles. The mis-scale lives in the **downstream fullscreen pass that samples this
  buffer back across the backbuffer** (UV/scale/window rect, dependent on backbuffer dims),
  which is GPU-side. Patch removed (no benefit, alters factory behavior). The mechanism facts
  below stay as anchors; the residual lever is the sampling-side UV (see hunt notes / §2.10).
- **Feature (candidate, removed):** targeted the pause/modal **blur backdrop tiling** at wide-but-≤1080-tall
  resolutions (issue #3: 3840×1080; 2560×1080 also affected). Symptom: with a modal dialog
  or submenu open, the screen behind it becomes several horizontally-squeezed duplicate
  copies with black vertical gaps; closing the modal restores it. The title screen itself
  is fine — it is the *backdrop* (blurred snapshot behind a modal), not any UI widget
  (field-isolated: independent of Span HUD, Fix HUD, SpanAllBackgrounds, and confirmed by
  probe as not a whitelisted background element).
- **Mechanism (offline-verified):** the Cygames "cyan" renderer sizes a shared downsampled
  post-process buffer (`GaussScaledTarget` / `GaussTemporaryTarget`, the same RT family the
  pause/modal blur uses — `ControlPauseBg` / `SetIsOpenPauseMenu` / `bgGauss_`) by a two-way
  branch on the **src render resolution** @ **RVA 0x023A55E0**:
  `both(renderW≥1921 && renderH≥1081) ? 1920×1080 : 960×540`, reading src_render W/H
  (0x06B84090 / 0x06B84094), storing to 0x06B84078 (H) / 0x06B8407C (W); the single consumer
  @ RVA 0x001EB1C8 allocates the RT at exactly that size (RT-manager singleton 0x07192F48).
  At 3840×1080 the `renderH≥1081` test fails (1080 < 1081) → 960×540 buffer; a downstream
  fullscreen pass samples that 16:9 buffer across the 32:9 backbuffer → wrap/tiling. 5120×1440
  and 3440×1440 pass the test → full 1920×1080 buffer → clean. The `renderW≥1921` test already
  excludes native 16:9 1080p, so only wide (>1920) ≤1080-tall setups (this mod's territory)
  are affected.
- **Patch (no hook):** pattern `3D 81 07 00 00 0F 93 C0 81 F9 39 04 00 00 0F 93 C1 84 C8`
  (`cmp eax,0x781; setnb al; cmp ecx,0x439; setnb cl; test al,cl`), offline-verified unique
  @ 0x023A55E0; `Memory::Write` imm32 at **+10** (the imm32 of `cmp ecx,0x439` sits right
  after the 2-byte opcode+ModRM `81 F9`; +11 clips the next instruction and crashes): `0x439 → 1` so the renderH test always
  passes and the buffer stays 1920×1080. Pure imm rewrite (same length, no hook, no branch
  retarget). The size is recomputed per frame, so the patched compare takes effect every
  subsequent frame regardless of injection timing. Gated to `iCustomResX > 1920 &&
  iCustomResY <= 1080`, leaving every factory-reachable setup untouched. `DiagDump` logs
  `src_render` and `gauss_buffer` for confirmation.
- **Confidence:** mechanism (branch, globals, consumer, RT identity) **HIGH** (offline);
  the "buffer size causes the tiling" causal claim **FALSIFIED** in the field (see verdict
  above). This is the §4 rule-8/9 lesson again: a HIT and a confirmed data change (the buffer
  really did become 1920×1080) still proved nothing about the visible symptom until eyes-on.
  The real fix turned out to be §3.29.

### 3.29 GaussBackdropTiling (`GraphicalFixes`, NEW 2026-07-11 — issue #3 fix)

- **Feature:** fixes the pause/modal **blur backdrop tiling** at renderH == 1080 (3840×1080,
  5120×1080, 2560×1080, …). Symptom & isolation: see §3.28. Field-confirmed trigger table:
  3840×1080 broken, 5120×1080 broken, 5120×1440 fine, 3440×1440 fine → the trigger is purely
  **renderH == 1080**, independent of width/aspect.
- **Mechanism (offline-verified):** the backdrop's downsampled source (Cygames "cyan"
  `GaussScaledTarget`) is a **hardcoded 1920×1080** RT (RVA 0x007CE785 stores 0x780, 0x007CE791
  stores 0x438). It is drawn back to the backbuffer by replaying a projection matrix built each
  frame by the viewport reader **@ RVA 0x0330EF00** (called from 0x030E3E3E), which branches on
  render height @ **RVA 0x0330EF2B**: `cmp dword[rcx+0x284],0x438 ; jnz 0x0330EF66`:
  - **renderH == 1080** (fall-through) → builds the projection from the ACTUAL (wide) width
    `[rsi+0x280]` while the source stays 1920-wide → horizontal factor 3840/1920 = 2.0 → the
    blit wraps → two squeezed copies + black gaps. renderH is the only input, so every
    1080-tall width tiles.
  - **renderH != 1080** (jnz taken → 0x0330EF66) → normalizes to a canonical 1920×1080 basis;
    this is the path 5120×1440 / 3440×1440 already take, hence they are clean.
- **The draw-path flag +0x28A is a RED HERRING** (the failed first attempt): a sibling
  *producer* @ RVA 0x0330F55C does `cmp r8d,0x438 ; setnz byte[rcx+0x28A]`, and the consumer
  @ RVA 0x03956ECC branches on that flag. Patching the producer's `0x438→0` (imm at +3) had
  no field effect because its `setnz` sits behind a **resolution-CHANGE early-out** (`cmp
  [rcx+0x284],r8d ; jz` @ 0x0330F53F) — on a stable 1080-tall session it never executes, so
  the flag keeps its default. The per-frame authority that actually bakes the wrong geometry
  is the reader 0x0330EF00, not the flag. (Also a prior dead-end: §3.28 buffer resize.)
- **Patch (no hook):** pattern `81 B9 84 02 00 00 38 04 00 00` (`cmp dword[rcx+0x284],0x438`),
  offline-verified unique @ 0x0330EF2B; `Memory::Write` imm32 at **+6** (after the 2-byte
  opcode+ModRM `81 B9` and the 4-byte disp32 `0x284`): `0x438 → 0`, so renderH (never 0)
  always takes the jnz/normalize arm. Verify the imm reads 0x438 first (the §3.28 off-by-one
  crash is the cautionary tale — count opcode+ModRM+disp before the imm). This reader runs
  every frame, so unlike the producer it is not gated by a resolution-change check. Gated to
  `iCustomResX > 1920 && iCustomResY <= 1080`, so native 16:9 1080p (correct as-is) is untouched.
  **Fallback** if this regresses: flip the `jnz` (75)→`jmp` (EB) at 0x0330EF35 (equivalent),
  or inspect the matrix write at +0xB0 (0x0330F018 / the 0x0330EF98→0x760F70 call).
- **Confidence:** **HIGH** — offline mechanism (reader is the per-frame projection authority,
  branch decoded, trigger table matches renderH==1080 exactly, explains why both the §3.28
  resize and the producer-flag patch failed) **and field-verified 2026-07-11** at 3840×1080:
  the tiling behind modals is gone with this patch (after §3.28 and the producer-flag patch
  had both changed bytes yet left the symptom — the §4 rule-8/9 lesson, twice, before this).

---

## 4. General re-hunting procedure (step by step)

When the log shows a `MISS` (or a `HIT` with no `FIRED`) after a game update, relocate the
pattern like this:

1. **Confirm the version.** Check the module timestamp in the log (`1782470458` = v2.0.2).
   Every RVA and pattern here is version-specific.
2. **Start from a stable anchor, not the old code shape.** Prefer, in order:
   data tables (§2.2–2.4), struct offsets (§2.1, §2.5, `+0x3BC`, `+0x9D0`), and `.rdata`
   float constants (e.g. `1/64 @ 0x054AE7E0`, `1/30/60/120 @ 0x054D6BF0`). These survive
   recompiles; instruction shapes usually don't.
3. **Xref the anchor.** `gbfr_analyze xref <anchorRVA>` to find every reader/writer.
   If there are too many, filter to the opcode form you expect (as `classify_xrefs.ps1`
   did to isolate the single resolution `cmp`).
4. **Disassemble each candidate.** `gbfr_analyze disasm <fileOffset> <bytes>` and read the
   surrounding function. Confirm the register/memory semantics match the feature (which
   register holds the width/aspect/distance, which struct offset is written).
5. **Pin the exact instruction boundary for the hook.** A mid-hook must be placed **on an
   instruction boundary**, **after** the value you want to change is loaded and **before**
   it is used/stored, and ideally on an instruction with **no rip-relative operand** (so
   safetyhook can relocate it). Re-count byte lengths from the disassembly — this is where
   most of the +offset changes vs v1 came from (CutsceneFOV +0xC→+0x1C, ShadowQuality
   −0x1, FPSCap +0x5→+0xC, GfxCorruption +0x0→+0x8).
6. **Write a pattern and verify uniqueness.** `gbfr_analyze scan <pattern>`. Aim for a
   unique hit, or a known, enumerated set of hits (like GfxCorruption's 4+2 branch arms —
   hook all of them). Prefer bytes that encode *semantics* (struct offsets like
   `BC 01 00 00`, opcodes) over volatile bytes (register nibbles → wildcard them).
7. **Record RVA, hook offset, register/memory semantics, and confidence** here and in the
   `dllmain.cpp` header, and add a `HIT`/`FIRED` log line so the next failure is diagnosable.
8. **Verify at runtime.** A `HIT` only means the bytes exist; a `FIRED` means the hook ran.
   Watch for a `HIT` with no `FIRED` (found a dead/duplicate copy) and, for MEDIUM-confidence
   patterns (LODDistance), confirm the visible effect in-game. The v1 "UIMarkers" port is the
   cautionary tale: unique hit, plausible semantics, but a mode-gated path that never ran —
   only the missing `FIRED` line exposed it (see ADR-0006).
9. **A `FIRED` proves execution, not visual effect.** The original ViewParamsFOV hook (§3.17)
   fired on exactly the right value at a site whose output nothing visual consumes — the
   feature was "field-verified" by log for a while, yet changed nothing on screen (ADR-0011).
   Any hook whose purpose is visual must be verified **eyes-on** before its confidence is
   marked HIGH; log-only verification is valid only when the effect is itself observable in
   the log (e.g. resolution values, table patches).
