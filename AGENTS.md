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

## Releasing

### How releases work here

Pushing a git tag does **not** release anything. A release is cut by **creating and
publishing a GitHub Release** — in the web UI or with `gh release create` (which will create
the tag if it does not exist yet). Publishing fires `.github/workflows/release.yml`
(`on: release: published`), which:

1. Builds the plugin (`GBFRUltrawide.asi`, CMake + MSVC) and the installer
   (`GBFRUltrawideSetup.exe`, csc.exe) **in parallel**, both checked out at the release tag.
2. **Stamps both binaries with the version from the release tag** — `vX.Y.Z` becomes `X.Y.Z`
   (leading `v` stripped) and is passed as `-DGBFR_VERSION=` to CMake and `-Version` to
   `build_installer.ps1`. **No version number lives in source**: a plain local build is
   `0.0.0-dev`, which identifies an unofficial binary in logs and ranks below every real
   release in the installer's update check.
3. Assembles one ready-to-use `GBFRUltrawide-<tag>.zip` (installer at the root, plus a
   `payload\` tree mirroring the game layout) and **uploads it onto that same release** a few
   minutes later.

The workflow never creates or flips the release, so a **pre-release** marking is preserved —
tick "Set as a pre-release" in the UI or pass `--prerelease` to `gh release create`.

### Version numbering

Semver, tag `vX.Y.Z`:

- **Minor** for new user-facing features.
- **Patch** for bug-fix-only releases (including making a previously-broken feature work).

### Release TITLE format (current convention)

The title is **`mod semver` + `game version`**, and must **not** start with the word
`GBFRUltrawide`. The standard shape is:

```
vX.Y.Z / Game vA.B.C
```

The first is the **mod** (GBFRUltrawide) version; the second is the **Granblue Fantasy
Relink** version it targets. Examples:

```
v0.1.0 / Game v2.0.2
v0.2.2 / Game v2.0.2
```

The title is cosmetic: CI keys off the git tag (`github.event.release.tag_name`), never the
title, so this format can change freely without affecting the build.

### Release NOTES content (current convention)

User-facing, Keep-a-Changelog style. State **what changed since the previous release**,
grouped into these sections; **omit any section that is empty**:

- **Breaking changes** — anything that changes existing behaviour in a way that could
  disrupt current users (config keys renamed/removed, defaults flipped, a setting that now
  does something it did not before). List this **first** when present; it is the most
  important thing for an upgrading user to see.
- **Added** — new features/options.
- **Changed** — behaviour or defaults that changed (non-breaking).
- **Fixed** — bug fixes.
- **Known limitations** — things that still do not work / caveats.

Keep it for users: no RVAs, pattern strings, or hook internals. It is good practice to end
with a line noting the download appears a few minutes after publishing (CI attaches the zip).

Fill-in template (drop `### Breaking changes` when there are none):

```markdown
### Breaking changes
- …

### Added
- …

### Changed
- …

### Fixed
- …

### Known limitations
- …

The ready-to-use `GBFRUltrawide-<tag>.zip` is attached by CI a few minutes after publishing.
```

Example (a `v0.1.0 / Game v2.0.2` release that adds the camera-distance multiplier):

```markdown
### Added
- Camera-distance multiplier: pull the gameplay camera further back
  (`[Gameplay Camera Distance] Multiplier` in the ini / installer GUI).

### Known limitations
- At high camera-distance values the camera can clip through nearby walls in tight rooms.

The ready-to-use `GBFRUltrawide-v0.1.0.zip` is attached by CI a few minutes after publishing.
```

### Cutting a release — step by step

1. **Make sure `main` is committed and pushed** — CI checks out the tag, so the fix/feature
   must be on the commit the tag points at.

   ```powershell
   git status              # clean
   git push origin main
   ```

2. **Choose the version** (semver rule above) → tag `vX.Y.Z`, and the game version for the
   title.

3. **Create + publish the release** with the new title and notes. `gh release create` creates
   the tag on the current `main` commit if it does not exist:

   ```powershell
   gh release create v0.1.0 `
     --title "v0.1.0 / Game v2.0.2" `
     --notes-file notes.md
   ```

   Or inline notes with `--notes "..."`. Add `--prerelease` for a `0.x` pre-release (or tick
   the box in the UI). Do **not** pass `--draft` — the workflow triggers on `published`, so a
   draft builds nothing until you publish it.

4. **CI does the rest**: builds both binaries stamped `0.1.0`, bundles the zip, and uploads it
   onto the release (~a few minutes).

5. **Verify** the asset attached and the versions are right:

   ```powershell
   gh release view v0.1.0                 # GBFRUltrawide-v0.1.0.zip listed under Assets
   ```

   The plugin logs `GBFRUltrawide vX.Y.Z loaded.` on start; the installer shows `X.Y.Z` in its
   window. Both must read `0.1.0`, not `0.0.0-dev` — `0.0.0-dev` means the asset was built
   locally, not by CI from the tag.

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
