# ADR-0006: World-anchored UI fix via the canvas-manager invariant

- Status: Accepted
- Date: 2026-07-10

## Context

After ADR-0004 shipped, enemy health bars, damage numbers and the lock-on marker sat
uniformly ~440 px to the right of their targets at 3440×1440 (hit detection unaffected).
440 px is exactly the pillarbox width (3440 − 2560)/2 — a constant offset, independent of
where the enemy is on screen.

Diagnosis (two offline analysis rounds + three instrumented in-game sessions):

1. The v1 "UIMarkers" per-widget hook had been relocated to RVA 0x026812CD. The pattern
   HITs, but the site never FIREs even in combat: it is a mode-gated, corner-anchored
   one-off widget (unique −95/−30 px constants, one xref in the whole exe), not the
   general marker path. The relocation hunt had been methodologically blind: it scanned
   for *adjacent vmovss load pairs*, but the live code loads W and H with a single packed
   `vmovsd` and consumes them via `vmulss reg, 0.5, [mem]` — invisible to that pattern.
2. The real world→screen widget positioning is inlined ~51× (behind-camera guard const
   0x054A5BD0 enumerates the copies) and computes `canvasX = screenX/scale − canvasW/2`
   from the global canvas manager `[0x07C02358]` (scale +0x17C/+0x180/+0x184, source W/H
   +0x1B4/+0x1B8, current W/H +0x1BC/+0x1C0). Rendering maps back through
   `windowW/2 + canvasX·scale`, so positions are correct only while
   **canvasW × scale == windowW**. Vanilla fill-width satisfies this by construction;
   ADR-0004's fit-height scale (H/2160) broke it.
3. The first fix attempt wrote manager W = 2160·aspect (5160) once, at the
   scale-recompute site — no visual change, because **+0x1BC is a derived field**: the
   dirty-layout recalc (RVA 0x0261C5D0, invoked for every dirty canvas from 0x02524B80 /
   0x0214E641) copies source W/H (+0x1B4/+0x1B8) over current W/H every dirty frame,
   washing the write back to 3840 long before combat.

## Decision

Extend the CanvasFitHeight site with a second mid-hook at pattern +0x40 (RVA 0x0015FB08,
immediately after the three scale stores; rax = canvas manager) that writes **both** the
source and current fields: W (+0x1B4, +0x1BC) = 2160·aspect at >16:9, H (+0x1B8, +0x1C0)
= 3840/aspect at <16:9. Writing the source field turns the game's own layout recalc into
the maintainer of our value instead of its destroyer.

This is a data-layer fix in the ADR-0003 sense: all ~51 inlined positioning copies read
the manager, so one write covers them all — hooking projection copies individually is
hopeless.

## Consequences

- Verified in-game at 3440×1440: HP bars / damage numbers / lock-on align at screen
  center and edges; an instrumented watchdog build observed **zero clobbers** across a
  full combat session (the recalc faithfully propagates the source value); fixed HUD
  unaffected (the 41 named per-canvas nodes keep their own 3840×2160).
- The v1 "UIMarkers" per-widget hook is removed (dead path in v2). Porting lesson
  recorded in `docs/PATTERNS.md` §3.10 and §4: a unique pattern hit with plausible
  semantics can still be a never-executed path — only `FIRED` logging exposes it.
- Runtime-verified breadcrumbs for the next port: positioner fns 0x02648970 (r14=mgr)
  and 0x02652B90 (rbx=mgr); shared world→screen helper RVA 0x00962FD0.
- Appendix finding: none of the three v1 HUDConstraints object IDs (gameplay HUD,
  guard & lock-on, dodge) appear in v2's object stream, and +0x194/+0x198 read as
  normalized anchors (0.5), not pixel offsets — the ID-gated Span-HUD offset writes are
  almost certainly inert on v2 and need a re-hunt if Span HUD ever misbehaves.
- Known residual risk: off-screen culling reads the widget's own canvas node
  (`[widget+0x10]`, 0x026D4FF0 family), not the manager. If world-anchored widgets
  vanish near the ultrawide side regions, the hosting named canvas (41-node table @
  0x05A3CA70) needs the same width treatment. Not observed in testing; left unpatched.
