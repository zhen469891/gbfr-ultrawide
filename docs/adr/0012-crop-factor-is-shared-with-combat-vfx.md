# ADR-0012: The +0x59C factor is shared with combat VFX — ship fAspectMultiplier, not 1.0

- Status: Accepted
- Date: 2026-07-10

## Context

GitHub issue #1: combat skill flashes (Io charge complete, Narmaya combo finisher — a
shared VFX class) drew full-height vertical black bars exactly on the 16:9 boundary
positions at 3440×1440, beneath the HUD, tracking the VFX lifetime.

Elimination ran in three rounds:

1. Not the UI path: `SpanAllBackgrounds = true` (widens every 3840×2160 canvas element
   regardless of id) changed nothing, and a 512-slot probe at the UIBackgrounds site
   captured no plausible element at VFX time. Independent of SpanAllHUD.
2. Offline, the view-CB crop factor (+0x59C, ADR-0004) has exactly one writer (our
   ScreenEffects hook covers it) and **zero CPU-side readers** — the CB is uploaded
   wholesale and consumed by shaders, so shader-side consumers are invisible to exe
   analysis. A "second consumer" could neither be confirmed nor refuted offline.
3. A dev-build override (`[Debug - Scene] CropFactorOverride`) settled it in-game:
   writing v1's `fAspectMultiplier` (1.34375 at 21:9) instead of our shipping `1.0`
   made the flash span the full screen width — and, with the FOV-multiplier confound
   removed (multiplier reset to 1.0), an A/B at a fixed camera position showed the
   **3D scene renders identically** under 1.0 and 1.34375.

That second observation contradicts ADR-0004, which chose 1.0 because "v1's
fAspectMultiplier was itself contributing to the zoomed/cropped frame". That reading
was confounded: at diagnosis time several resolution layers (ADR-0003) were not yet
patched, and the zoom attributed to this value came from elsewhere.

## Decision

The ScreenEffects hook writes **fAspectMultiplier** (= aspect / (16/9)) at every
non-16:9 aspect, both wider and narrower — restoring v1 semantics. The +0x59C factor
is understood as a shader-side scale consumed by at least the scene pass and the
full-screen combat-VFX quad path; fAspectMultiplier is the value that sizes both
correctly at 3440×1440.

`CropFactorOverride` and `WindowRefOverride` (a second lever, built for the
windowRef-pair theory that was never needed) remain in dev builds as diagnostic
instruments (ADR-0010 gating).

## Consequences

- Field-verified at 3440×1440: flash spans the full width, no bars; scene framing
  unchanged; supersedes the >16:9 value choice of ADR-0004 (its site, pattern and
  fill-width goal stand).
- Process lessons, reinforcing ADR-0011's: (a) a value with no CPU readers can still
  have multiple *shader* consumers — offline consumer maps end at the CB upload
  boundary, and only in-game experiments see past it; (b) when re-testing a visual
  hypothesis, strip confounds first — the active FOV multiplier initially masked the
  scene-zoom comparison this decision hinged on.
- If a future report shows scene zoom at some other aspect ratio, re-run the A/B via
  `CropFactorOverride` before touching the shipping value: the two consumers may yet
  disagree at aspects we have not tested (32:9 untested as of this writing).
