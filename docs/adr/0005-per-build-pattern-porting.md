# ADR-0005: Treat the plugin as pinned to one game build

- Status: Accepted
- Date: 2026-07-09

## Context

Granblue Fantasy Relink v2.0 shipped a recompiled executable. Every byte pattern in the
original GBFRelinkFix (written for v1.x) broke: 8 of 15 patterns missed outright, and several
that still matched had hook offsets that now landed mid-instruction or on a
since-reallocated register. The modding community (Nenkai) documented that all code-injection
mods broke on v2.0 due to "compiler changes."

This will happen again on the next major game update.

## Decision

Accept that the plugin is **pinned to a specific build** (v2.0.2, module timestamp
1782470458) and optimise for *re-porting* rather than for a build-independent abstraction:

- Keep all patterns/offsets in one file (`src/dllmain.cpp`) with a `PATTERN STATUS` header
  documenting each pattern's state, verified RVA, and v1→v2 change.
- Maintain an offline analysis tool (Zydis-based `gbfr_analyze.exe`: `disasm`/`scan`/`xref`)
  and a written re-location playbook in `docs/PATTERNS.md`.
- Every scan logs `HIT`/`MISS`; every hook logs a one-shot `FIRED`. This log is the primary
  field-debugging instrument (users have no debugger).
- Anchor relocation on things that survive recompiles: data-table contents, struct offsets,
  UI object-ID hashes, floating-point constants — not raw instruction bytes.

## Consequences

- A game update is expected to break the plugin; the response is a documented re-port, not a
  redesign.
- The `FIRED`/`HIT`/`MISS` logging and the pinned module timestamp let us (and users) confirm
  quickly whether a given game build is supported.
- New contributors have a repeatable method instead of having to rediscover the toolchain.
