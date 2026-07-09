# ADR-0004: Fixing the fill-width zoom/crop on ultrawide

- Status: Accepted
- Date: 2026-07-09

## Context

After every data layer reported 3440×1440 (swapchain, globals, quality row, UI cached width)
and the projection matrix aspect was forced to 2.38889, the game *still* rendered a 16:9
composition scaled up to fill the width, cropping top and bottom. The overflow ratio was
exactly (3440/3840)/(1440/2160) = **1.34375**, matching the observed crop.

Offline analysis found two mechanisms new to v2.0 (the Switch 2 / Steam Deck generation),
neither present when the original mod was written:

1. **Canvas → screen scale (UI/compositor)** at RVA 0x0015FAB8: `scaleX = W/3840`,
   `scaleY = H/2160`, then `scale = scaleX + (scaleY - scaleX) * t` with `t` **hardcoded to
   0** — i.e. always fill-width. All 41 named canvases are 3840×2160 units, center pivot.
2. **Scene crop factor** stored at the view constant block +0x59C (`(renderH*16/9)/renderW`,
   = 0.744186 at 21:9, 1.0 at 16:9), consumed by a shader. This store site is the *same
   instruction* the original mod hooked as "ScreenEffects" — v2 repurposed that constant, and
   our inherited v1 value (1.34375) was actively contributing to the crop.

## Decision

- **Scene**: in the (formerly ScreenEffects) hook at 0x020D117B, write **1.0** to the crop
  factor when aspect > 16:9, so the 3D scene spans the full window width uncropped.
- **UI**: replace the v1 "force UI ortho width to 16:9" trick (wrong tool for v2) with a
  4-byte NOP of the lerp multiply at the canvas-scale site (pattern `CanvasFitHeight`,
  +0x18). `scale = scaleX + (scaleY - scaleX)` collapses to `scaleY` → fit-height: the UI
  sits in the centered 16:9 region (the v1 look the user expected), matched with the scene
  now spanning full width.

Both are guarded by byte-signature checks before patching.

## Consequences

- Correct 21:9: 3D is hor+ full-width, UI/HUD centered 16:9. Confirmed in-game.
- The "ScreenEffects" name is retained in code for lineage but documented as the scene crop
  factor (see `docs/PATTERNS.md`).
- **Known risk — photo mode**: it uses a different crop factor and we currently write 1.0
  unconditionally. If photo-mode framing looks wrong, gate the hook on the photo-mode flag
  (`[[0x07032DE0]+0x65] & 1`). Left un-gated until a user reports it.
- The center-pivot mapping is assumed to keep UI horizontally centered; if a future case
  shows UI drifting left, an offset site would need locating (noted, not yet needed).
