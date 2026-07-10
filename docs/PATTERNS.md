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
| 0x06B84088 / 8C | active render W/H |
| 0x06B84090 / 94 | src render W/H |
| 0x06B84098 / 9C | window_ref W/H (**also** the 1920/1080 that HUDConstraints reads) |

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
> camera **near plane**, copied from the registry `[0x07C54830]` entry `+0xD2C` — not
> view CBs. Only the §3.3 site (inside the producer above) writes the view CB's +0x59C;
> don't chase the other 18 when hunting crop-factor consumers/writers.

**Scene crop factor +0x59C:** the store the **ScreenEffects** hook sits on writes the
view CB's **+0x59C**, which in v2.0.2 is a shader-consumed **scene crop factor**: `1.0`
= uncropped (what the game computes at 16:9), `(H*16/9)/W` at wider aspects (0.744186
at 21:9 = fill-width crop). Writing `1.0` at >16:9 lets the 3D scene span the full
window width. `rax = [rcx+0x2A0]` is the view CB pointer at the hook site.

**windowRef pair +0x594/+0x598:** hosts the dev-only WindowRefOverride experiment
(§3.23) — theory: a shader sizes full-screen combat-flash quads as
`renderH * (+0x594 / +0x598)`, which is 16:9 whenever the pair holds 1920/1080.

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

- **Feature:** raise the 240 fps cap (experimental; physics may misbehave >30 fps).
- **Pattern:** `48 8D 05 ?? ?? ?? ?? C5 7B 10 14 C8` (`lea rax,[table]` + `vmovsd xmm10,[rax+rcx*8]`).
- **Hits / RVA:** unique @ RVA 0x001B6E63; hook **+0xC** (after the 7-byte lea + 5-byte vmovsd).
- **Hook semantics:** `ctx.xmm10.f64[0] = (double)1/240` — the value is a **double** frame-time. Must be after the vmovsd, or the original load overwrites ours. (v1 used xmm6 at +0x5.)
- **v1→v2:** the limiter was rewritten — it now reads the target frame time from a 3-entry double table into xmm10 and busy-waits on `vucomisd + pause`.
- **Hunt method:** anchored on the 3-entry double frame-time table **@ RVA 0x054D6BF0** = `{1/30, 1/60, 1/120}` (indexed by the fps menu setting; the index comes from `[0x07C26B70]` + `byte[rax+0x3D]`) — the most reliable anchor; `disasm` located the load + the `vucomisd`/`pause` busy-wait spin. A minimal alternative pattern is `C5 7B 10 14 C8` (hook +0x5).
- **Confidence:** HIGH.

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
