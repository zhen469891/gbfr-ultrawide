# ADR-0003: Prefer data-layer patches over code hooks for resolution

- Status: Accepted
- Date: 2026-07-09

## Context

The custom-resolution feature originally worked by hooking the single "preset index →
width/height" conversion site and overwriting the output registers. On v2.0.2 this was
insufficient:

- The compiler inlined the conversion into **four** copies (two match the old pattern, two
  have a different prologue `8B 41 3C` and are invisible to it). The swapchain was sized
  through a copy we did not hook, so the screen stayed 16:9 even though our hooks fired.
- A separate boot-time "apply saved settings" path does not use the preset table at all — it
  switches the active row of a **quality table** (6 rows, stride 0x4C, static defaults all
  1920×1080) and two `xchg`-quad sites publish that row's dimensions to the resolution
  globals. This reverted our resolution on the second boot flicker.

Diagnostics confirmed every data layer (four resolution globals, quality-table row,
swapchain) needs to read our value — hooking any one code path leaves the others stale.

## Decision

Patch the **shared data** rather than (only) the code:

1. Overwrite all 5 entries of the resolution preset table (RVA 0x054BEA78 width / 0x054BEA8C
   height) at startup.
2. Overwrite all 6 quality-table rows' render + window dimensions (0x06B84210 + r*0x4C +
   0x24/0x28/0x2C/0x30).
3. Pin the two `xchg`-quad publish sites (0x006C126B, 0x001C8F33) with mid-hooks so whatever
   later gets written into the row, the value published to the globals is forced to ours.

Code hooks are kept only as belt-and-braces (e.g. the S3/S4 inlined conversion copies) or
where no shared datum exists.

Table addresses are derived at runtime from the hooked site's rip-relative `lea`s and
sanity-checked (expected 3840/1920/2160/1080 contents) before writing, so a bad match is a
skipped patch, not a corrupt write.

## Consequences

- One write covers every consumer of a table, present and future-inlined.
- Resilient to the game re-applying settings — the publish-site hooks are the backstop.
- Side effect: the in-game resolution menu may show every option as the forced resolution.
- Related: ADR-0004 (the remaining zoom/crop was a separate compositing issue, not a data one).
