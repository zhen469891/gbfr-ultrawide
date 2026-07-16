# ADR-0017: Tutorial-highlight boxes (issue #7) left as a known limitation

- Status: Accepted
- Date: 2026-07-17

## Context

Issue #7: during in-game tutorials the game highlights a HUD element with a box/border, and at
ultrawide those boxes stay at their 16:9 position while the HUD the mod repositions moves out to
the true screen edges. The investigation was deep; it is recorded here so it is not re-run from
scratch:

1. **Not in the widget tree we hook.** Moving ~29 candidate UI-constraint element ids via the
   dev `MoveIds` lever shifted lots of HUD text but moved *none* of the highlight boxes — so the
   boxes are not laid out by the `HUDConstraints` pass (§3.12) the mod owns.
2. **Separate subsystem.** `ui::component::ControllerTutorialHud` (update @ RVA `0x028CA120`)
   only sets a center-anchored root's Y; `ui::component::ControllerTutorialCursor`'s
   highlight-extent method @ RVA `0x026767D0` positions a box from the target element's **stored**
   `+0x19C` px.x (and `+0x1BC` width ×0.5 to centre). The Span-HUD widen is **register-only**
   (ADR-0006/0007) — it never persists to `+0x19C` — so this reader sees the un-widened 16:9 x.
   That is the root cause.
3. **The located lever didn't reach the reported boxes.** Instrumenting `0x026767D0` showed it
   only ever handled a single center-anchored element (id `3436450466`, 256×80), not the three
   reported corner boxes; a dev move-test offsetting its cached px.x (`[rsi+0x208]`) did not move
   the reported boxes → they use yet another path.
4. **Data tool can't read it either.** The prefab
   (`ui/layouts/tutorial/hud/prefabs/tutorial_hud01_frame01.prfb`, component
   `ControllerTutorialCursor`) fails GBFRDataTools' `b-convert` ("Unmapped/Unsupported component
   type", ADR-0016), so exact layout numbers are not recoverable statically.

No clean lever remains from either the runtime hooks or the data tool. The only routes left — a
broad hook on the *shared* layout engine (would risk every relatively-positioned UI element) or
live tracing with an attached debugger (SteamStub anti-tamper risk, on a hot per-frame site) —
are disproportionate for a cosmetic, tutorial-only artifact.

## Decision

Leave issue #7 as a documented known limitation (README "Known limitations"). Also record the
related issue #6 (16:9-authored cutscene / system-menu backgrounds whose transparent sides reveal
the live 3D scene rather than black bars) as a known limitation — adding pillarbox there is
out-of-scope cosmetic work.

The dev-only diagnostic used during the hunt (`TutorialHudDiag` plus a `FrameOffsetX` move-test)
was **removed after the investigation** to keep the source clean; the pinned sites/RVAs are
recorded here and in PATTERNS/memory for any future revisit.

## Consequences

- #6 and #7 ship as known limitations; the README explains that tutorial boxes are positioned by
  a separate system independent of the HUD layout the mod adjusts.
- If revisited, resume from `ControllerTutorialCursor` / the extent method `0x026767D0`, or from
  dynamic tools — not from the `HUDConstraints` pass.
- The register-only widen (ADR-0006/0007) is the standing root reason any *stored-rect reader*
  (this tutorial subsystem, and potentially others) lags the visibly-moved HUD — relevant if a
  similar report appears.
