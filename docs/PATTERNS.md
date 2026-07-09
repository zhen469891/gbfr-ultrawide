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
  `xscale = 1/tan(fov/2)/aspect`, writes camera `+0x40` / `+0x100`. This — not the
  AspectRatio hook — is what actually shapes the 3D frustum, so it gets its own
  ProjMatrixAspect hook. Data-driven 16:9 writers set the dirty flag themselves, so
  overriding aspect right at the matrix builder covers every camera and aspect source.

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

### 2.7 Scene crop factor = view constant block +0x59C

The store the **ScreenEffects** hook sits on writes the view CB's **+0x59C**, which in
v2.0.2 is a shader-consumed **scene crop factor**: `1.0` = uncropped (what the game
computes at 16:9), `(H*16/9)/W` at wider aspects (0.744186 at 21:9 = fill-width crop).
Writing `1.0` at >16:9 lets the 3D scene span the full window width. `rax = [rcx+0x2A0]`
is the view CB pointer at the hook site.

---

## 3. Per-pattern reference (15 patterns + v2 additions)

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
- **Hook semantics:** `if aspect > 16:9 → xmm0 = 1.0` (uncropped); `if aspect < 16:9 → xmm0 = 1.0 * fAspectMultiplier`. **v1 wrote `fAspectMultiplier` (1.34375) here, which was itself causing the zoomed/cropped frame** — the correct value at >16:9 is 1.0.
- **v1→v2:** pattern/hook offset unchanged; the *semantic meaning* of the stored value changed (crop factor, not a scale).
- **Hunt method:** `disasm` of the store (`vmovss [rax+0x59C]`) plus shader-side reasoning showed +0x59C is consumed as a crop factor; the disassembly is `vdivss` → `vmovss [rax+0x59C]` → `ret`. The 16:9 value the game computes here is `(H*16/9)/W` (0.744186 at 21:9); the producer function is at RVA 0x020D0DF0.
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
- **v1→v2:** new in v2 (v1 had no separate projection hook — the single aspect write sufficed on v1's code).
- **Hunt method:** traced why AspectRatio alone left the 3D at 16:9 (that hook feeds a consumer *after* the matrix is built); `disasm` of 0x00750970 revealed the dirty-flag-triggered `1/tan(fov/2)/aspect` builder reading `+0x9D0` into xmm8.
- **Confidence:** HIGH.

### 3.6 GameplayCamera (`AspectFOVFix`, PARTIAL)

- **Feature:** gameplay camera-distance multiplier. **FOV multiplier is intentionally NOT installed** (see below).
- **Pattern:** `C5 7A 10 ?? ?? ?? 00 00 C5 7A 11 ?? ?? C5 FA 10 ?? ?? ?? 00 00 C5 FA 11 ?? ?? C5 7A 10 ?? ?? ?? 00 00 C5 7A 11 ?? ?? 48 85 C0 74` (three load+store pairs + `test/jz`).
- **Hits / RVA:** unique @ RVA 0x009D8C70; distance hook **+0x8** (before `vmovss [rbp-0x14], xmm9`).
- **Hook semantics:** `if fCamDistMulti != 1: ctx.xmm9.f32[0] *= fCamDistMulti` — distance is in xmm9, not yet written to the message block, and the game's own per-area clamps (300 cm indoor / 800 cm general) still apply after. (v1 used xmm8.)
- **FOV — why not supported:** v1's camera message was `{id, dist, FOV, yaw}`; v2.0.2 sends `{id, dist, pitch, yaw}`. The old FOV slot (loaded via xmm7 at pattern+0xD, camera field +0x13A0) is now a **pitch angle in radians** — readers multiply by 1/(2π) and wrap to [−π,π). A v1-style FOV hook here would tilt the camera. `dllmain.cpp` logs a `NOT SUPPORTED` warning instead. FOV is now delivered via preset(id)/camera-blackboard paths.
- **v1→v2:** stack frame changed `rsp`→`rbp` (6-byte SIB stores → 5-byte disp8), and `mov rax,[rsi+0x4358]` was hoisted above the float loads — killing the v1 pattern. Struct offsets (`+0x1398` dist, `+0x13A0` pitch, `+0x13A8` yaw) unchanged.
- **Hunt method:** `scan "C5 7A 10 ?? ?? ?? ?? 00 C5 7A 11"` → 13 hits, narrowed by `disasm` to 0x009D8C70; cross-checked against consumer handler 0x00A840D7, the 300/800 cm clamp compares, and the distance-squared / atan2-yaw / ÷2π writers to prove the field roles (dist +0x1398, pitch +0x13A0, yaw +0x13A8).
- **Confidence:** distance HIGH; the "v1 FOV slot is now pitch" reconstruction MEDIUM (no v1 exe to diff against). **Fallback:** offset-anchored pattern `C5 7A 10 ?? 98 13 00 00 …`.

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

### 3.10 UIMarkers (`HUDFix`)

- **Feature:** world→screen marker projection (marker positions off at non-16:9).
- **Pattern:** `C4 ?? ?? 21 ?? 10 C4 ?? ?? 21 ?? 30 C4 ?? ?? 0C ?? 04 C5 ?? ?? ?? BC 01 00 00 C5 ?? ?? ?? C0 01 00 00` (vinsertps×2 + vblendps + `vmovss[+0x1BC]` + `vmovss[+0x1C0]`).
- **Hits / RVA:** unique @ RVA 0x026812CD; hook **+0x12** (RVA 0x026812DF, first `vmovss`, after three 6-byte AVX instructions). The fallback-branch jmp also lands here, covering both control-flow paths.
- **Hook semantics:** base register is **RAX** (v1 was rcx; loaded from `[rsi+0x2410]`, verified not clobbered). `if aspect<16:9: *(float*)(rax+0x1C0) = 2160 + fHUDHeightOffset; else if aspect>16:9: *(float*)(rax+0x1BC) = 2160 * fAspectRatio`.
- **v1→v2:** struct shift −0x38 (§2.1) + codegen change; v1 prefix `mov reg,[reg+0x20]` sequence gone entirely.
- **Hunt method:** v1 pattern + variants = 0 hits. `scan` the width/height pair `BC 01 00 00 … C0 01 00 00` → 42 hits, each classified by disassembling ~0x28 bytes before each hit; 29 STORE pairs rejected (clamp/copy boilerplate), LOAD pairs triaged (rejected keyframes, HUDConstraints, canvas-ratio, UI quads, flow-layout, a 20-case anchor jump table) until one unique site remained.
- **Confidence:** **MEDIUM-HIGH** (unique hit, semantic lineage, blast radius limited to the marker canvas; runtime marker check recommended). **Fallbacks:** anchor switch @ RVA 0x04276C42, canvas-ratio @ RVA 0x026D9446; simpler patterns `C4 ?? ?? 0C ?? 04 …` (hook +0x6) and `C5 ?? ?? ?? BC 01 00 00 … 76 ??` (hook +0x0).

### 3.11 UIBackgrounds (`HUDFix`)

- **Feature:** span full-screen backgrounds (fades, menus) to fill the screen, matched by object ID.
- **Pattern:** `41 ?? ?? ?? ?? 00 E8 ?? ?? ?? ?? 80 ?? ?? ?? 00 0F ?? ?? ?? ?? ?? 48 ?? ?? ?? ?? 48 ?? ?? E8 ?? ?? ?? ?? 48 ?? ?? ?? C5 ?? ?? ?? ?? ?? ?? 00`
- **Hits / RVA:** base @ RVA 0x03340AD5; `base += 0x2F` → RVA 0x03340B04.
  - **>16:9 (width) hook at base+0x0**: modify **xmm1** (v2 loads width into xmm1 via `vmovss xmm1,[rax+0x1BC]`; v1 modified xmm0 but xmm0 is overwritten by the next instruction). If `width==3840` and the ID is in `BackgroundWidthIDs`, `xmm1 = 2160 * fAspectRatio`.
  - **<16:9 (height) hook at base+0x29**: modify **xmm4** (right after `vmovss xmm4,[rax+0x1C0]`; v1's base+0x28 landed inside that 8-byte instruction). `xmm4 = 3840 / fAspectRatio`.
  - Struct offsets in the lambda: ID +0x1C4, width +0x1BC, height +0x1C0 (§2.1).
- **v1→v2:** struct shift −0x38; width register xmm0→xmm1; `<16:9` hook +0x28→+0x29 (v1's +0x28 landed on the last byte of the 8-byte `vmovss xmm4,[rax+0x1C0]`).
- **Hunt method:** `disasm` at base showed the width load into xmm1 and the mid-instruction boundary at +0x28.
- **Confidence:** HIGH.

### 3.12 HUDConstraints (`HUDFix`)

- **Feature:** span / offset specific HUD elements by object ID (gameplay HUD, guard & lock-on, dodge), plus `SpanAllHUD`.
- **Pattern:** `48 ?? ?? ?? ?? ?? 00 48 ?? ?? 74 ?? C5 ?? ?? ?? ?? ?? ?? 00 C5 ?? ?? ?? ?? ?? ?? 00 C5 ?? ?? ?? ?? ?? ?? 00 EB ??`
- **Hits / RVA:** @ RVA 0x0261C638; hook **+0x1C** (RVA 0x0261C654; `xmm2=width [rax+0x1BC]`, `xmm0=height [rax+0x1C0]` — registers unchanged from v1).
- **Hook semantics:** switch on `*(int*)(rax+0x1C4)`: gameplay HUD 1719602056 (span xmm2/xmm0), guard&lock-on 605904162 and dodge 3550204025 (offset via rax+0x194 / +0x198), plus `SpanAllHUD` (mark rax+0x1C8 = 1234). See §2.1 for offsets.
- **v1→v2:** hook offset and xmm registers unchanged; only the lambda struct offsets shifted −0x38.
- **Hunt method:** `disasm` confirmed +0x1C is the correct boundary (xmm2/xmm0 hold width/height from +0x1BC/+0x1C0) and that the defaults come from globals `0x06B84098/9C` (1920/1080, `vcvtsi2ss`'d into xmm2/xmm0).
- **Confidence:** HIGH.

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
   patterns (LODDistance, UIMarkers), confirm the visible effect in-game.
