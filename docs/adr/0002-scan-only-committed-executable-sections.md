# ADR-0002: Pattern-scan only committed executable sections

- Status: Accepted
- Date: 2026-07-09

## Context

The stock GBFRelinkFix `PatternScan` walks the entire module image from base to
`base + SizeOfImage`. On game v2.0.2 this **crashes the game** shortly after the injection
delay — and it is exactly the crash users reported when the old mod stopped working on v2.0
(Codeberg issue #1).

Root cause: the executable ships with SteamStub DRM, which adds a high-entropy `.bind`
section. After the game's original entry point runs, the SteamStub loader stub decommits its
own pages. A blind scan that walks into those (now uncommitted) pages triggers an access
violation. The one-second injection delay is what puts our first scan *after* the decommit,
which is why the crash looked delayed and intermittent.

## Decision

Rewrite `Memory::PatternScan` / `PatternScanAll` to iterate the PE section headers and scan
**only sections with `IMAGE_SCN_MEM_EXECUTE`**, and within each to `VirtualQuery` every
region and skip anything not `MEM_COMMIT` + readable + non-`PAGE_GUARD`.

## Consequences

- No more AV: uncommitted/guarded pages are skipped instead of dereferenced.
- Faster scans: we skip the large `.rdata`/`.data`/DRM regions entirely.
- Strictly correct for us — every pattern we hook lives in `.text`.
- If a future pattern ever needs to match in a non-executable section, the scan helper must
  be widened deliberately (and re-audited against the DRM pages).
