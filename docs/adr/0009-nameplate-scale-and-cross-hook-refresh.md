# ADR-0009: Nameplate scale fixed at its projection divide, plus a cross-hook global refresh

- Status: Accepted
- Date: 2026-07-10

## Context

Nameplates (floating name/speech labels over characters) project through their own
world→screen path, separate from the canvas-manager path fixed in ADR-0006. That path
computes a horizontal scale via `vdivss xmm1, xmm7, [rax+0x9D0]` (divide by camera
aspect) and stores the result into a game-side global that later nameplate rendering
consumes. With our aspect override active, the stored scalar comes out wrong at >16:9
and nameplates drift off their characters.

Two complications shaped the fix:

1. The compute site only runs on frames where the projection executes; a fix applied
   only there leaves stale frames when the site is idle.
2. The consuming global's address is build-specific and must not be hardcoded
   (ADR-0005).

## Decision

- Mid-hook the divide site (RVA 0x00847F6B, hook +0x3C) and override
  `xmm1 = (xmm7 != 0 ? xmm7 : 1.0) / fAspectRatio` — dividing by the **live** aspect
  ratio, not the 16:9 constant.
- Resolve the game-side scalar global **at scan time** from the store instruction's own
  rip-relative disp32 (byte-checked as `C5 FA 11 0D` before decoding, and decoded before
  the mid-hook rewrites the site's bytes). On this build it resolves to
  exe+0x07194FCC.
- Refresh that global to `1.0f/fAspectRatio` from the **Span HUD (HUDConstraints) hook**
  on every pass. The UI layout hook runs far more often than the nameplate projection
  site, so it acts as the keep-alive for frames where the divide never executes.

The cross-hook refresh is deliberate and worth this ADR on its own: a future reader will
otherwise wonder why the UI-constraints hook writes a nameplate global that "belongs" to
a different site. Install order matters: `NameplateFix()` runs before `HUDFix()` so the
pointer is resolved before the first refresh.

## Consequences

- If the byte-check fails on a future build, the main divide hook still installs; only
  the keep-alive refresh is disabled (with a warning log), degrading gracefully.
- Field status: HIT + global resolution verified on this build
  (`scale global cached at exe+0x7194fcc`); the divide site's FIRED requires a scene
  that renders nameplates (town NPCs / co-op) and is still pending an in-game pass.
- The refresh couples the Span HUD hook to the nameplate feature: disabling
  `[Span HUD]` also stops the keep-alive (the divide-site hook remains).
