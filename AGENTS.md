# AGENTS.md

Guidance for AI agents (and humans) working in this repository.

## What this is

**GBFRUltrawide** is an ASI plugin that adds ultrawide (21:9 / 32:9) support to
**Granblue Fantasy Relink v2.0.2** ("Endless Ragnarok"). It is a from-scratch port of
[Lyall's GBFRelinkFix](https://codeberg.org/Lyall/GBFRelinkFix) (MIT), whose byte patterns
all broke when game v2.0 shipped with a recompiled executable. The plugin is a 64-bit DLL
(`.asi`) loaded by Ultimate ASI Loader (masquerading as `winmm.dll`).

See `README.md` for user-facing install/usage and `docs/PATTERNS.md` for how every memory
pattern was (re)located on v2.0.2.

## Layout

| Path | What |
| --- | --- |
| `src/dllmain.cpp` | The whole plugin: config, pattern scans, hooks. One file, mirrors upstream's structure. |
| `src/helper.hpp` | `Memory::PatternScan` / `PatternScanAll` / `PatchBytes` / `Write`. |
| `vendor/` | Vendored deps: `safetyhook` (+ amalgamated Zydis), `inipp`. Tracked on purpose. |
| `ini/GBFRUltrawide.ini` | Default config shipped to users. |
| `installer/` | .NET Framework 4.8 WinForms install/config GUI (`GBFRUltrawideSetup.exe`). |
| `redist/` | Ultimate ASI Loader (`winmm.dll`) + its license. |
| `docs/` | ADRs, agents config, pattern-hunting notes. |

## Build

```powershell
.\build.ps1                    # CMake + MSVC, outputs dist\GBFRUltrawide.asi
.\installer\build_installer.ps1  # csc.exe, outputs installer\out\GBFRUltrawideSetup.exe + payload\
```

Requires VS2022 Build Tools (MSVC v14.4x, C++23, static CRT, x64). No Python needed.

## Working on the hooks — read this first

This plugin patches a **specific game build (v2.0.2, module timestamp 1782470458)**. Byte
patterns and struct offsets are build-specific and _will_ break on future game updates.

Hard-won facts that a naive edit will get wrong:

1. **Never scan the whole module image.** The SteamStub `.bind` section decommits its own
   pages after the entry point runs, so a blind `SizeOfImage` scan AVs (this is what crashes
   stock GBFRelinkFix on v2.0). `Memory::PatternScan` only walks committed, executable
   sections — keep it that way. (ADR-0002)

2. **UI object struct shifted -0x38 vs v1.** width `0x1F4→0x1BC`, height `0x1F8→0x1C0`,
   object ID `0x1FC→0x1C4`, offsX `0x1CC→0x194`, offsY `0x1D0→0x198`, marker `0x200→0x1C8`.

3. **A pattern hitting is not enough — verify the hook offset lands on an instruction
   boundary with the expected semantics.** v2's compiler changed register allocation and
   instruction ordering; several patterns still hit but the old hook offset now points
   mid-instruction or at a register that gets clobbered. Use the offline Zydis tool
   (`scratchpad/gbfr_analyze.exe`: `disasm` / `scan` / `xref`) to check.

4. **Prefer data-layer patches over hooks where a shared table exists.** The resolution
   preset table and quality table are read by multiple inlined copies of the same logic;
   patching the table covers all of them, whereas hooking one copy misses the others. (ADR-0003)

5. **The zoom/crop fix is subtle.** v2 maps a 3840×2160 canvas to the screen fill-width
   (lerp `t` hardcoded 0) and applies a shader-side scene crop factor. See ADR-0004 and
   `docs/PATTERNS.md`. The old ScreenEffects constant is now that crop factor.

When a pattern needs relocating, mirror the workflow in `docs/PATTERNS.md`: anchor on
unchanged constants/struct offsets, `xref`, disassemble, verify semantics, and record
confidence + the RVA it was verified at.

## Conventions

- Every pattern scan logs `HIT`/`MISS`; hooks log a one-shot `FIRED` diagnostic. Keep this —
  it is the only way to debug on a machine without a debugger attached.
- Never dereference a failed scan (`nullptr`). Check before adding an offset.
- Config section/key names are kept identical to upstream GBFRelinkFix so old configs carry over.
- Communicate with the maintainer in Traditional Chinese (Taiwan).

## Agent skills

### Issue tracker

Issues are tracked as GitHub issues via the `gh` CLI; external PRs are not a triage surface. See `docs/agents/issue-tracker.md`.

### Triage labels

Five canonical roles, default string vocabulary (`needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`). See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: one `CONTEXT.md` + `docs/adr/` at the repo root. See `docs/agents/domain.md`.
