# ADR-0011: Rendered FOV multiplier inside the projection-matrix builder, before tanf

- Status: Accepted
- Date: 2026-07-10
- Supersedes: [ADR-0008](0008-fov-multiplier-at-view-params-site.md)

## Context

ADR-0008 placed the gameplay-FOV multiplier at the view-params consumer site
(`xmm3 *= fFOVMulti` right after `vmovss xmm3,[rax+0x9D4]`, hook RVA 0x007510BA) and
considered it field-verified because the expected log line appeared with the correct base
value (`FIRED: ViewParamsFOV (base FOV 0.872665 x1.15)`). A user report (GitHub issue #2)
showed the rendered image does not change at any multiplier value.

Offline disassembly proved why. The multiplied xmm3 is stored only to `[rsp+0x20]` and
passed as the 5th argument to `0x0216ABE0` — the culling / shared-view-constants builder,
the *secondary* consumer already named in PATTERNS.md §3.5. The projection matrix that
actually shapes the image is built at RVA 0x00750970 (the ProjMatrixAspect site) and
re-reads the FOV **from memory** (`vmulss xmm0,xmm7,[r14+0x9D4]`); a register multiply at
the view-params site can never reach it. **The register multiply never affected the
rendered projection — it feeds culling only.** Both `HIT` and `FIRED` were genuine: the
hook ran, on the right value, at a site with no visual consumer.

The community ultrawide research the hook had been cross-validated against contains the
byte-identical design (same pattern, same +0x20 offset, same register multiply) — so that
build never changed the rendered FOV either, and was evidently never visually verified.
Cross-validation against it reproduced the flaw instead of catching it.

Alternatives considered for the real fix:

- **Memory write `[obj+0x9D4] *= fFOVMulti`** — rejected. A byte scan finds 14+
  rip-visible writers of `+0x9D4` (several per-frame camera-update paths). A memory
  multiply is either overwritten before the builder reads it (no effect) or, placed after
  a per-frame writer, compounds every frame (0.87 → 1.09 → 1.36 → … explodes within a
  second).
- **Scaling the camera-preset FOV publish** (the four preset-apply sites of the
  CamDistPreset family) — gameplay-only in principle, but indirect (downstream
  camera-update writers/lerps sit between the publish and `+0x9D4`) and unverified; kept
  as a MEDIUM-confidence fallback only if cutscene exclusion is ever demanded.

## Decision

Multiply **in-register at the final consumer**: a `ProjMatrixFOV` mid-hook inside the
projection-matrix builder (unique pattern hit RVA 0x00750970, hook +0x1A = the 5-byte
`call tanf`), where `xmm0 = FOV/2`: `ctx.xmm0.f32[0] *= fFOVMulti` ⇒ `tan((m·FOV)/2)`, an
exact angular multiply that both yscale and xscale follow. Game memory is never written.

The builder serves **every** camera whose dirty flag (+0x9DE) is set — gameplay,
cutscenes, menu 3D scenes. There is no clean "is gameplay" discriminator at this site, so
the multiplier is **global by design** (cutscenes are also affected; default 1.0 = off;
documented in the ini).

**ViewParamsFOV is kept**, re-documented as the culling-consistency companion: with the
rendered frustum widened, the culling path must see the same multiplied FOV, or
`fFOVMulti > 1` culls objects against the narrower unmultiplied frustum and pops them at
the screen edges. Same install gate (`fFOVMulti != 1.0`), unchanged body.

**Ordering:** the ProjMatrixFOV pattern's first 13 bytes are the ProjMatrixAspect
pattern, and the ProjMatrixAspect mid-hook at +0x9 rewrites bytes inside it. The
ADR-0008 scan-before-install rule therefore now covers **four** patterns — AspectRatio,
ViewParamsFOV, ProjMatrixAspect, ProjMatrixFOV: `AspectFOVFix()` completes all four scans
before installing any of the four hooks. (The installed hooks coexist: stolen ranges
+0x9..+0x10 vs +0x1A..+0x1E at the builder do not overlap.)

## Consequences

- `[Gameplay FOV] Multiplier` changes the rendered image on v2.0.2; cutscene cameras are
  also affected (by design, documented).
- Process lesson, now part of the porting checklist: **a FIRED log proves execution, not
  visual effect.** A hook can run on exactly the right value at a site whose output
  nothing visual consumes. Visual features need eyes-on verification before being called
  field-verified; log-only verification is valid only for hooks whose effect is itself
  observable in the log.
- Corollary: "cross-validated against community research" is not verification when the
  reference implementation was itself never verified — it can propagate a defect.
- Any future hook near the view-params site (0x0075109A) or the projection builder
  (0x00750970) must join the scan-all-then-install sequence in `AspectFOVFix()`.
