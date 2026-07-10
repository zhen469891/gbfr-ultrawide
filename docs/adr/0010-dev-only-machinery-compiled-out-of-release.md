# ADR-0010: Element-discovery machinery is dev-build only, compiled out of release

- Status: Accepted
- Date: 2026-07-10

## Context

Pushing individual 16:9-kept menu elements (e.g. corner button prompts) to the true
screen edge requires knowing their element ids. The discovery workflow needs three
knobs: `Probe` (log every unique element flowing through the layout hook),
`EdgeSnapIds` and `MoveIds` (per-id position overrides tunable without recompiling).

The first field test showed why these are *not* user features:

- Candidate ids picked from probe data alone misidentified the targets (all three picks
  were wrong on screen).
- Element `px` positions are **anchor-relative, not center-relative** — the EdgeSnap
  "push in the direction of sign(px)" heuristic points the wrong way for corner-anchored
  elements (anchors 0/0 or 1/1). Getting this right needs iteration by a developer
  looking at the screen and the log together.
- The end state is to bake confirmed ids/offsets **into code** as shipped defaults, not
  to ship tunable knobs.

The alternative — runtime toggles in one binary — ships dead-weight diagnostics to every
user, invites "I set Probe=true and my log exploded" support cases, and presents dev
scaffolding as if it were a product surface.

## Decision

Split builds. A CMake option `GBFR_DEV` (default OFF; surfaced as `build.ps1 -Dev`)
defines `GBFR_DEVBUILD`. The `[Debug - Span HUD]` ini section (Probe, EdgeSnapIds,
MoveIds) is parsed, and its hook logic compiled in, **only** under `GBFR_DEVBUILD`;
release builds (the default, and what CI ships) contain none of it — verified by string
absence in the release binary. Dev builds identify themselves in the log
(`loaded. (dev)` plus a section-active notice). `probeLog` is an empty no-op lambda in
release so hook-body call sites stay readable and the optimizer removes them.

Combat-prompt recentering and the nameplate scalar refresh are product features, not
debug machinery — they stay in both builds.

## Consequences

- Setting `[Debug - Span HUD]` keys on a release build does nothing, by design; the ini
  section's comments say so explicitly. The installer neither shows nor edits these keys
  (its ini writer round-trips unknown sections untouched).
- Two build flavors exist; `build.ps1` passes `-DGBFR_DEV=ON/OFF` explicitly on every
  configure so the option can never stick from a previous build.
- The per-element edge-extension work itself is deferred: when resumed, iterate in a dev
  build, then land confirmed ids in code (growing the blocklist/move tables of
  ADR-0007), leaving release users zero configuration surface.
