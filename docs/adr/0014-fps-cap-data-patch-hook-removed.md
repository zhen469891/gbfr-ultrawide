# ADR-0014: FPS cap via frame-time table data patch — the v1-style mid-hook was structurally unsafe

- Status: Accepted
- Date: 2026-07-11

## Context

GitHub issue #4: `[Raise Framerate Cap] Enabled = true` has no visible effect — the game
stays capped at 120 fps. The user's log shows `HIT: FPSCap` and no errors. The shipped
mechanism (inherited from Lyall's v1 design, relocated for v2.0.2) was a safetyhook
mid-hook at pattern+0xC (RVA 0x001B6E6F) overwriting `xmm10` with `1/240` right after the
game loads the target frame time. Notably, this hook had **no FIRED logging** — HIT ≠
FIRED all over again — and RetroGawd's port of the same feature is outright dead
(pattern 0-hit) on this build, so no community mod ever had a working v2 cap to compare
against.

Offline analysis (tools/gbfr_analyze + one-off PE scanners; read-only, exe on disk is not
SteamStub-encrypted) established the full picture of the limiter:

- The main-loop function (RVA 0x001B5FE0..0x001BC3CB, `.pdata`-confirmed) reads the target
  frame time as a double from a 3-entry table `{1/30, 1/60, 1/120}` @ RVA 0x054D6BF0,
  indexed by the fps menu byte (`[[0x07C26B70]+0x3D]`: 0→30, 1→60, ≥2→120), then
  busy-waits on `QueryPerformanceCounter` + `vucomisd xmm0, xmm10` + `pause`.
- **The hook site is a 1-byte nop (0x001B6E6F) immediately followed by the spin-loop head
  (0x001B6E70).** The loop's back-edge (`jmp` @ 0x001B6EC0) targets hookAddr+1. A mid-hook
  must steal ≥5 bytes for its jmp, so every spin iteration after the first jumps into the
  middle of the patch bytes and decodes the jmp's displacement field as instructions.
  The hook was structurally unsafe ever since the v1 port — v1's site merely happened to
  have a different layout.
- An exhaustive xref sweep (rip-relative disp32 over all executable sections, plus an
  absolute-VA pointer scan over the whole file) found the `lea` at 0x001B6E63 is the
  table's **only** static reader. `xmm10` is never spilled; the table is re-read every
  frame; no branch bypasses the limiter (with vsync the first compare simply passes).
- Downstream, the engine computes `timeScale = clamp(avgFrameSec × 60.0, 0.25, 3.0)` from
  a 4-frame ring buffer (@ 0x0703F440). `1/240` sits exactly on the 0.25 bound: **240 fps
  is the highest cap at which game logic still runs at real-time speed.** (This also
  retro-explains the old in-code comment "menus seem to speed up beyond 240fps".)

## Decision

Remove the mid-hook entirely. `FPSCap()` now only uses the pattern to *locate* the table
(via the lea's own rip-relative displacement — `Memory::GetAbsolute64(hit+3)`, never a
hardcoded RVA), verifies the third entry equals `1/120`, and `Memory::Write`s it to
`1/240`. The patched value is logged by reading the table back.

Consequences accepted:

- The in-game **120** option becomes 240; the 30/60 options keep their original meaning
  (the removed hook used to force 240 regardless of the setting — arguably a bug). README
  and ini text now tell users to select 120 in the game's graphics settings.
- The cap stays at 240 by design; raising it further would need the timescale clamp
  addressed too, which is out of scope and known to distort game speed.

## Alternatives considered

- **Fix the hook landing site** (e.g. hook before the lea and patch rcx, or inline-patch
  the vmovsd): still a code patch adjacent to a branch target, and pointless — the data
  patch achieves the identical effect with zero code-patch risk (ADR-0003 precedent:
  prefer data-layer patches over hooks when a stable data anchor exists).
- **Keep hook + add data patch as belt-and-suspenders**: rejected once the disasm showed
  the hook corrupts the spin loop's back-edge; an unsafe hook is not a safety net.

## Verification

Offline: end-to-end mechanism verified by disassembly (single reader, register-only
consumption, per-frame reload, clamp bounds).

Field (2026-07-11, local, 144 Hz monitor): with the data patch, fps exceeds 120 (monitor
refresh limits confirmation of the full 240); both `FPS Cap:` log lines present. **The
crash prediction was also confirmed**: the previous v0.2.2 build with
`[Raise Framerate Cap] Enabled = true` crashes at launch — exactly what the disassembly
predicted for a spin loop whose back-edge jumps into the mid-hook's patch bytes once the
limiter engages. (The issue #4 reporter instead saw "no effect", plausibly because their
session never spun long enough at the cap to take the corrupted back-edge, or create_mid
failed silently on their machine — either way, both field behaviors are consistent with
the unsafe hook.) Awaiting the reporter's retest for a ≥240 Hz confirmation.
