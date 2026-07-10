# ADR-0008: Gameplay FOV multiplier at the view-params consumer, not the camera message

- Status: Accepted (2026-07-10); superseded by [ADR-0011](0011-fov-multiplier-inside-projection-builder.md) — the view-params multiply never affected the rendered projection (it feeds culling only); the hook is retained as the culling-consistency companion of ProjMatrixFOV
- Date: 2026-07-10

## Context

v1 applied the FOV multiplier inside the gameplay-camera message hook. In v2 the camera
message struct changed shape: the slot that used to carry FOV now carries **pitch**
(radians) — `{id, dist, FOV, yaw}` became `{id, dist, pitch, yaw}`. Multiplying that slot
tilts the camera instead of zooming it, which is why the feature shipped disabled with a
warning ("FOV not supported on v2") until now.

Candidate sites for reinstating it:

- **Camera message hook** — dead end; the value is no longer FOV.
- **Projection-matrix builder** (RVA 0x00750970, already hooked by ProjMatrixAspect) —
  scaling fov there would entangle the FOV feature with the aspect override and also
  affect cutscene cameras.
- **View-params consumer** — the block that loads the camera struct's own fields
  (+0x9A0/+0x9A4/+0x9D0 aspect/+0x9D4 FOV) for gameplay view setup.

## Decision

Hook the view-params consumer (pattern
`C5 ?? ?? ?? A0 09 00 00 C5 ?? ?? ?? A4 09 00 00 C5 ?? ?? ?? D0 09 00 00 C5 ?? ?? ?? D4 09 00 00`,
unique hit; hook at +0x20, RVA 0x0075109A) immediately after `vmovss xmm3,[rax+0x9D4]`
loads the FOV, and apply a pure linear multiply: `xmm3 *= fFOVMulti`. The hook installs
only when the multiplier ≠ 1.0. No aspect write is added at this site — the AspectRatio
hook already owns `[obj+0x9D0]`.

**Ordering is load-bearing:** this pattern's bytes overlap the AspectRatio hook's install
site (RVA 0x00751089 vs 0x0075109A). Installing a mid-hook rewrites bytes at the site, so
*both patterns must be scanned before either hook is installed*. The scan-then-install
sequence in `AspectFOVFix()` is deliberate; reordering it breaks whichever pattern is
scanned second.

## Consequences

- Gameplay FOV works on v2.0.2 (field-verified: `FIRED: ViewParamsFOV (base FOV
  0.872665 x1.15)`, no camera tilt). The old "unsupported" warning is removed; only the
  <16:9 vert- compensation remains unsupported.
- Any future hook added near the aspect/FOV camera-struct block must join the
  scan-first-install-later sequence.
