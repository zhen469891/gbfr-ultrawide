# ADR-0013: Camera distance multiplier at the register-base committed-eye writer

- Status: Accepted
- Date: 2026-07-11

## Context

The `[Gameplay Camera Distance] Multiplier` had no working home on v2.0.2. Five prior
hunts each found a plausible site that turned out dead:

- The **v1 message site** (GameplayCamera @ RVA 0x009D8C70, §3.6) HIT uniquely but never
  FIRED — v2.0.2 gameplay no longer drives the camera through that message path.
- The **CamDistPreset / FollowCamDist / RoamCamDist** families (§3.20–3.22) never fired in
  the field, and the preset publish target 0x07C25720 turned out to be **mainView+0x3C0**,
  a cold ctx tail with zero rip-visible readers holding a constant 4.8 (§2.5/§2.8) — it
  cannot carry the live distance.
- The **four rip-relative commit copies** (CamCommitDist / CAMDIST_HUNT2, RVAs
  0x01A2D8F3 / 0x01F4185F / 0x01FF3AAC / 0x0320150C) were the RANK-1 candidate but were
  **field-proven dead**: the DIAG-CAM2 counters recorded 0 fires on all four sites across
  full gameplay and cutscenes. They are relocation-only staging copies that never run in
  live play.

Every one of these was found by `xref` — a scan for rip-relative `disp32` references. That
is exactly why they all missed the real writer. The live per-frame eye/at writer stores
**register-relative** (`vmovaps [rsi+0x10], xmm0` / `[rsi+0x20], xmm1`, rsi = ctx), which
carries no rip-relative displacement and is therefore invisible to a disp32 cross-reference.
Offline hunt #3 (CAMDIST_HUNT3) instead walked the dispatch loop that rebuilds each view
and landed on writer function 0x00691F60, which is called 10× per frame (call #0 =
`lea rcx,[0x07C25360]`, the static main-view ctx0); a dev-only counting probe then
confirmed in-game that its committed-eye store fires every frame on the main view.

## Decision

Hook the register-base committed-eye writer with a single mid-hook at RVA 0x00692497
(the `vmovaps [rsi+0x10], xmm0` store inside 0x00691F60), guarded to fire only when
`rsi == baseModule+0x07C25360` (ctx0, the **main view only** — the nine aux views are left
untouched). At the hook `xmm0 = eye`, `xmm1 = at`; we apply **Form A**, a register-only
dolly about the look-at: `xmm0.xyz = xmm1.xyz + (xmm0.xyz − xmm1.xyz) × fCamDistMulti`,
preserving lane 3 (w). No game memory is written.

Register-only is deliberate, following the ADR-0011 lesson: the committed eye at ctx+0x10
is republished every frame, so a **memory** multiply would either be overwritten before the
view rebuild reads it or compound frame after frame until the camera flies away. Scaling the
value in-register at the exact store that feeds the immediately following
`call 0x007513C0` (view-matrix rebuild) reaches rendering, culling and the listener
consistently, with nothing to compound. The store commits once per view per frame, so no
idempotency guard is needed. The hook is installed only when `fCamDistMulti != 1.0` (dev
builds also install a counting-only mode under `[Debug - Camera] CamDiag`); at 1.0 with
CamDiag off it is never installed, so it costs nothing.

This multiplier is **gameplay-only by gate**, and that is the key architectural distinction
from the FOV multiplier. The writer function's entry gate (`cmp byte [rcx+0xC0],0; jnz …`)
closes during cutscenes and dialogue, so the eye store is never reached then — scripted
cinematic framing is untouched. ProjMatrixFOV (ADR-0011) sits at the projection-matrix
builder *downstream* of any such gate and therefore does affect cutscenes; this distance
multiplier does not. That is a desirable scope difference, not an inconsistency: players want
their chosen zoom in play without re-framing every cutscene.

The one known limitation is inherited from where the commit sits: camera **wall-collision is
computed upstream** of this store, so a multiplier > 1 can clip or push the eye into walls
near geometry. This is documented, not fixed — v1's hook had the same property.

## Consequences

- `[Gameplay Camera Distance] Multiplier` now changes the **live gameplay camera distance**
  on v2.0.2. Cutscenes and dialogue are unaffected (gameplay-only by the +0xC0 gate).
- The old four-site CamCommitDist hook has been **removed from `src/dllmain.cpp` entirely** —
  if it ever revived it would double-multiply. Only a historical note remains (§3.24).
- In-game verification (2026-07-11, provenance "offline hunt + in-game FIRED verification
  2026-07-11", confidence HIGH): at 120 fps `fires_ctx0` fired every gameplay frame,
  `fires_other ≈ 9× fires_ctx0` (the nine aux views), `|eye−at|` tracked zoom over
  0.679–6.566 m (units confirmed meters), and the +0xC0 gate closed only in
  cutscenes/dialogue.
- Process lesson, added to the porting checklist: **a register-base writer is invisible to
  rip-relative xref.** When a per-frame-changing static has no rip-visible writer, do not
  conclude it has none — suspect a `[reg+disp]` store and hunt via the dispatch loop or the
  function that calls the rebuild, not via `xref` on the static's address.
