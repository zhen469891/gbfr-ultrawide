# ADR-0015: The modal blur backdrop tiling (issue #3) — and three wrong fixes before the right one

- Status: Accepted
- Date: 2026-07-11

## Context

GitHub issue #3: on a 32:9 desktop at 3840×1080, the title screen looked fine until a
modal dialog/submenu opened, at which point the whole screen *behind* the dialog became
several horizontally-squeezed duplicate copies with black vertical gaps; closing the modal
restored it. The reporter's workaround was to switch the desktop to 5120×1440 via NVIDIA
DSR.

This ADR exists because the bug took **three falsified fixes** to solve, each of which
produced a `HIT` (and in two cases a *confirmed byte/state change*) while leaving the
symptom untouched — the sharpest repeat of the §4 rule-8/9 lesson (a `HIT`, even a
verified data change, is not a fixed symptom) we have hit so far. Future render-path work
should assume this failure mode.

## The actual root cause

The pause/modal blur backdrop's downsampled source, the Cygames "cyan" renderer's
`GaussScaledTarget`, is a **hardcoded 1920×1080** render target (RVA 0x007CE785/791). It is
drawn back to the full backbuffer by replaying a projection matrix built each frame by a
viewport reader at **RVA 0x0330EF00**, which branches on render height at **RVA 0x0330EF2B**
(`cmp dword[rcx+0x284],0x438 ; jnz 0x0330EF66`):

- **renderH == 1080** (fall-through): builds the projection from the *actual* (wide) width
  while the source stays 1920-wide → a 3840/1920 = 2.0× horizontal mismatch → the blit wraps
  into duplicate copies + black gaps.
- **renderH != 1080** (jnz taken): normalizes to a canonical 1920×1080 basis — the path that
  1440-tall resolutions already take, which is why they were always clean.

The trigger is therefore **purely renderH == 1080**, independent of width/aspect. The fix
(feature `GaussBackdropTiling`, PATTERNS.md §3.29) patches that compare's immediate
`0x438 → 0` so renderH (never 0) always takes the normalize arm. Gated to
`iCustomResX > 1920 && iCustomResY <= 1080`, so native 16:9 1080p (correct as-is) is
untouched. Field-verified fixed at 3840×1080.

## Decision

Ship the reader-branch imm patch. Do **not** ship the two earlier candidates (both removed;
kept only as PATTERNS.md hunt records §3.27/§3.28 so they are not re-hunted).

## What made this hard — the diagnostic lessons

1. **A confirmed data change is not a fixed symptom.** The first real attempt
   (`GaussBufferSize`, §3.28) resized the downsampled buffer from 960×540 to 1920×1080; the
   `DIAG` line *confirmed* `gauss_buffer=1920x1080`, matching the working resolution — and
   the tiling was unchanged. Byte demonstrably changed, symptom demonstrably not. Only
   eyes-on told us. Always field-verify the *pixels*, never infer a visual fix from a state
   readback.

2. **Beware change-gated writes.** The second attempt (the `+0x28A` draw-path flag producer
   @ 0x0330F55C) looked like the perfect lever and its `setnz` was even the "obviously
   correct" branch — but it sits behind a *resolution-change* early-out (`cmp [rcx+0x284],r8d
   ; jz`), so on a stable session it **never executes**. A patch on an instruction that only
   runs on a state *transition* is inert during steady state. When a patched site shows a
   `HIT` but no effect, check whether an earlier branch skips it in the common case; find the
   **per-frame authority**, not a one-shot/edge-triggered sibling.

3. **A trigger matrix beats a single repro.** The break between "buffer size" and "height
   gate" theories was only settled by collecting four data points — 3840×1080 (bad),
   5120×1080 (bad), 5120×1440 (good), 3440×1440 (good). Width 5120 appeared in both a good
   and a bad case; height 1080 was bad in both widths. That isolated the axis to renderH
   with zero further disassembly. Ask the tester for the *orthogonal* resolution, not just a
   second confirmation of the same one.

4. **Symptom precision narrows the search enormously.** "Only while a modal is open, gone
   when it closes" moved the entire hunt off the UI-widget layer (backgrounds whitelist,
   Span HUD) and onto the engine's blur-backdrop compositor before any disassembly. An
   isolation pass (`Span HUD = false`, all UI hooks off) that *doesn't* change the symptom is
   as informative as one that does.

5. **Off-by-one on an imm patch crashes, not no-ops.** An early build wrote the buffer-size
   imm at signature +11 instead of +10, clipping the next instruction into a garbage
   memory-write and crashing at launch. Every imm patch now (a) counts opcode+ModRM(+disp)
   bytes explicitly to place the write, and (b) verifies the target reads the expected
   immediate before writing, so a future offset/pattern drift logs a `MISS` instead of
   corrupting code. (Same discipline the FPSCap rework adopted in ADR-0014.)

## Scope / non-goals

This is an engine bug reachable only because the mod enables wide-and-1080-tall render
resolutions; it is not introduced by any mod hook (proven by isolation). Users who prefer
not to run the patch can avoid the bug entirely by using any 1440-tall resolution (the
reporter's DSR 5120×1440 workaround). The fix is gated so it changes nothing for
factory-reachable setups.

## Verification

Offline: reader is the per-frame projection authority; branch, source-RT dims, and the two
dead-end levers all decoded and mutually consistent with the renderH==1080 trigger table.
Field (2026-07-11, local, 3840×1080): tiling behind the quit-confirm modal is gone with the
patch; `HIT: GaussBackdropTiling` present, no crash. See PATTERNS.md §3.29 (and §3.27/§3.28
for the falsified attempts).
