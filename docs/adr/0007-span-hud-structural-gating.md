# ADR-0007: Span HUD via structural gating, not object-ID whitelists

- Status: Accepted
- Date: 2026-07-10

## Context

The v1-inherited Span HUD gated its widening on three hard-coded object IDs (gameplay
HUD, guard & lock-on, dodge). The ADR-0006 appendix already suspected these were inert on
v2 — none of the three IDs appear in v2's object stream — and an instrumented field test
confirmed it: the ID-gated path never fires, so "Span HUD" silently degraded to
"nothing". Cross-validation against community ultrawide research showed the same layout
site (UI Constraints, pattern base RVA 0x0261C638, hook +0x1C; rcx = child element,
rax = parent, xmm2/xmm0 = parent W/H) can drive edge-to-edge HUD without per-feature IDs.

The alternatives were: (a) re-hunt the three v2 object IDs and keep the whitelist —
precise, but dead again on every content update and it only ever covered three elements;
(b) gate structurally and exclude what must stay 16:9.

## Decision

Gate structurally, exclude by tracking, and write registers only:

1. **Structural gate** — widen only children whose parent is a *full-canvas* container
   (exactly 3840×2160). Wide screens: `xmm2 = 2160·fHUDAspectRatio`; narrow:
   `xmm0 = 3840/fHUDAspectRatio`. Register-only: the layout recomputes every frame, so
   the override is self-healing and never pollutes other readers of the struct.
2. **menuTree** — a transitively-marked id set seeded with the known menu/story roots
   {1465589452, 141651223, 584127281}: whenever a parent in the set flows through, its
   child id is inserted. The set is **never cleared**: element ids are stable content
   hashes, and a subtree member observed once stays a menu element for the session;
   clearing would forget membership between relayouts and let menu children widen.
3. **Blocklists** — 8 parent ids + 3 child ids that must stay 16:9. Both menuTree and
   blocklist skips apply **only when the child's X anchors are equal**
   (anchorA.x == anchorB.x at +0x1A4/+0x1AC); stretch children (unequal anchors) are
   meant to fill their parent and still widen.
4. **Combat prompts** (parent id 2939675107, children anchored 0.5/0.5 with
   |px.x| ≥ 1600) are *moved*, not widened: first sight captures the authored px.x
   ([child+0x19C]), each pass rewrites it as `base · fAspectMultiplier`, pushing them
   from the 16:9 edge to the true screen edge.

The v1 gameplay-HUD root id (1719602056) is kept behind a one-shot diagnostic log purely
to detect if it ever comes back to life; the field test confirms it does not fire.

## Consequences

- Verified at 3440×1440: combat HUD spans to the true screen edges; menus and story
  dialogs stay 16:9-centered; `SpanAllHUD` ships default-on.
- The structural gate can over-widen elements we have not seen yet; the mitigation is
  the span diagnostic (first 20 unique widened child ids logged as `FIRED: … span #N`)
  plus growable blocklists — triage is "read the log, add the id", not a re-hunt.
- Menus kept at 16:9 now end abruptly at the pillarbox boundary. Pushing individual
  menu elements (e.g. corner button prompts) to the true edge is a separate, deferred
  problem — see ADR-0010 for the discovery tooling and why it is dev-build-only.
