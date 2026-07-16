# ADR-0016: Adopt GBFRDataTools for data-layer UI identification

- Status: Accepted
- Date: 2026-07-17

## Context

The mod keys its UI work on runtime element ids (struct **+0x1C4**, PATTERNS §2.1). Until now
those ids were discovered by live probing (the dev-only `Probe` / `MoveIds` machinery, ADR-0010)
and offline disassembly — slow, and re-done from scratch after every game update that shifts the
code patterns.

While investigating issue #7 we validated that the element id is exactly `XXHash32Custom(name)`
of the element's authored name (`root` → `0xFE1DAE6D` = 4263358061, the menu-root parent seen
dominating the Span-HUD probe logs — the validation vector), and that
[Nenkai/GBFRDataTools](https://github.com/Nenkai/GBFRDataTools) can extract the game's UI data
(`.prfb` / `.viewb` / `.tbl` inside `data.i`) and hash names via `hash-string`. That turns
"which element is id N?" into a data lookup — and, because element names are stable across
patches, an update-resilient one.

## Decision

Adopt GBFRDataTools as the project's **data-layer UI-identification aid**, complementary to (not
a replacement for) the runtime ASI hooks. The extraction workflow, the hash algorithm, and the
confirmed name↔id anchors are documented in **PATTERNS §2.11**.

Scope of use: mapping element ids ↔ names, reading authored layout where the converter supports
it, and re-finding elements after a game update. It is a maintenance/dev tool — **not bundled or
shipped** with the mod.

## Consequences

- Element identification and post-update re-finding for the *data/UI* side become data-extraction
  tasks rather than live-probe grinds; the *exe hook patterns* still need the PATTERNS §1
  re-hunting flow.
- Running the tool requires the **.NET 10 runtime**.
- `b-convert` cannot read prefabs with custom `Controller*` components (many HUD/menu prefabs),
  so element names are read from the raw `.prfb` strings and hashed; `.view.viewb` files convert
  but carry only layout transforms + child `AssetPath`s (no element-name fields).
- The mod's own adjusted elements are now name-resolved (e.g. combat-prompt host `2939675107` =
  `root01` in `ui/layouts/hud/guide_sub01.prfb`), which aids maintenance and future menu work
  (issue #6). It does **not**, by itself, unlock the issue #7 tutorial-highlight fix — that is a
  separate custom-component subsystem (ADR-0017).
