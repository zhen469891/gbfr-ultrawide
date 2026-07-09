# GBFRUltrawide

> Custom-resolution & ultrawide (21:9 / 32:9) fix for **Granblue Fantasy: Relink v2.0.2** — a v2.0.2 rebuild of Lyall's GBFRelinkFix.

Ultrawide / non-16:9 fix for **Granblue Fantasy: Relink v2.0.2**, shipped as an x64 ASI
plugin. It restores custom resolutions, correct aspect ratio, and a proper HUD on
21:9 / 32:9 (and narrower) displays.

## What this is (and why it exists)

This project is a **v2.0.2 rewrite** of [Lyall's GBFRelinkFix](https://github.com/Lyall/GBFRelinkFix)
(MIT License). GBFRelinkFix was written for game v1.x; when Relink updated to v2.0, the
compiler regenerated the code around every hook site and **all of the original memory
patterns stopped matching**. The upstream repository is now archived and no longer
maintained.

GBFRUltrawide keeps the upstream hook *design* and ini layout, but every pattern has been
re-hunted, disassembled, and instruction-level-verified against game **v2.0.2**. It also
drops the spdlog dependency in favour of a minimal built-in logger, renames the module to
`GBFRUltrawide`, and logs an explicit `HIT` / `MISS` / `FIRED` line for every scan so a
future game update makes it obvious *which* single feature broke.

For the full engineering story — how each pattern was relocated and how to redo it when
the game updates again — see [`docs/PATTERNS.md`](docs/PATTERNS.md).

## Features

| Feature | ini section | Notes |
| --- | --- | --- |
| Custom resolution / ultrawide | `[Custom Resolution]` | Any WxH, or `0` to use the desktop resolution. Patches the preset & quality tables plus every apply path. |
| Aspect ratio fix | `[Fix Aspect Ratio]` | Correct 3D projection aspect at any ratio (fixes the stretched/pillarboxed view). |
| HUD locked & centred to 16:9 | `[Fix HUD]` | Keeps the HUD readable and centred instead of stretched across the full width. |
| Span HUD | `[Span HUD]` | Optionally spans the gameplay HUD to a chosen aspect (`SpanAllHUD`, `SpanAllBackgrounds`). |
| Span backgrounds | `[Fix HUD]` / `[Span HUD]` | Fades, menus, and full-screen backgrounds fill the whole screen. |
| Graphical-corruption fix | (auto) | Rounds up internal render-scale values at odd widths (e.g. 3440) to stop corruption. |
| Shadow quality | `[Shadow Quality]` | Override the shadow-map resolution (e.g. 4096/8192). |
| Level of detail | `[Level of Detail]` | Multiplier to push out object LOD pop-in distance. **Medium confidence** — see below. |
| Disable TAA | `[Disable TAA]` | Turns off temporal anti-aliasing. |
| Raise FPS cap to 240 | `[Raise Framerate Cap]` | Experimental; physics can misbehave above 30 fps. |
| Camera distance multiplier | `[Gameplay Camera Distance]` | Pull the gameplay camera back/in. |

### Known limitations on v2.0.2

- **Gameplay FOV multiplier is NOT supported.** On v2.0.2 the gameplay camera message no
  longer carries a FOV value — the slot that used to hold FOV now holds a pitch angle.
  Hooking it would tilt the camera instead of zooming, so the FOV hook is intentionally
  not installed. Any `[Gameplay FOV]` value (and the `<16:9` FOV compensation of
  `[Fix FOV]`) is logged as *NOT SUPPORTED* and ignored. Aspect-ratio correction is
  unaffected and still works at every resolution.
- **LOD distance** and **UI markers** are **medium-confidence** relocations. They are
  verified at the instruction level but benefit from in-game confirmation; if they
  misbehave, fallback candidate sites are documented in `docs/PATTERNS.md`.

## Installing

The game must be at version **v2.0.2** (see [Compatibility](#compatibility)).

### Option A — installer (recommended)

Run **`GBFRUltrawideSetup.exe`**. It:

- auto-detects the Steam install of Relink (reads Steam's `libraryfolders.vdf`, Steam
  App ID `881020`, plus common library locations), or lets you browse to it manually;
- backs up any files it will overwrite into a `GBFRUltrawide_backup` folder, and offers
  to back up & remove leftover files from an old GBFRelinkFix install;
- deploys `winmm.dll` (Ultimate ASI Loader) to the game root and `GBFRUltrawide.asi` +
  `GBFRUltrawide.ini` into `scripts\`;
- provides a graphical settings editor for the ini (resolution, HUD, tweaks).

### Option B — manual install

1. Copy **`winmm.dll`** (the bundled Ultimate ASI Loader, x64) into the game root next to
   `granblue_fantasy_relink.exe`.
2. Copy **`GBFRUltrawide.asi`** and **`GBFRUltrawide.ini`** into the game's `scripts\`
   folder (create it if it doesn't exist).
3. Launch the game. A log is written to `scripts\GBFRUltrawide.log`.

> **Steam Deck / Linux:** add `WINEDLLOVERRIDES="winmm=n,b" %command%` to the game's Steam
> launch options so the loader is picked up.

## Configuration

Settings live in **`scripts\GBFRUltrawide.ini`**. The section and key names are kept
identical to upstream GBFRelinkFix so an existing config carries over. Key options:

- `[Custom Resolution] Enabled / Width / Height` — set your resolution, or leave
  `Width`/`Height` at `0` to inherit the desktop resolution.
- `[Fix HUD] Enabled` — lock the HUD to 16:9 and span backgrounds.
- `[Span HUD] Enabled / AspectRatio / SpanAllHUD / SpanAllBackgrounds` — `AspectRatio = 0`
  matches your screen; `1.7778` = 16:9, `2.3888` = 21:9, `3.5537` = 32:9.
- `[Fix Aspect Ratio] Enabled` — 3D aspect correction.
- `[Shadow Quality] Enabled / Value` — e.g. `2048`/`4096`/`8192`.
- `[Level of Detail] Multiplier` — `>1` pushes out LOD pop-in.
- `[Disable TAA] Enabled`, `[Raise Framerate Cap] Enabled` (240 fps, experimental).
- `[GBFRelinkFix Parameters] InjectionDelay` — ms to wait before applying non-resolution
  hooks (default `1000`).

## Compatibility

Built and verified **exclusively for game v2.0.2** (module timestamp `1782470458`). Every
pattern is anchored to code the v2.0.2 compiler emitted, so a **future game update will
very likely break some or all patterns** — exactly as v2.0 broke the original mod. When
that happens, the log flips the affected features to `MISS` and disables *only* those
features; the rest keep working. Re-hunting instructions are in
[`docs/PATTERNS.md`](docs/PATTERNS.md).

## Building from source

Requirements: **Visual Studio 2022 Build Tools** with the MSVC **v14.4x** toolset,
**C++23**, **x64**, and CMake 3.21+.

```powershell
.\build.ps1
```

This configures with the `Visual Studio 17 2022` generator (`-A x64`), builds Release, and
collects the artifacts into `dist\` (`GBFRUltrawide.asi` + `GBFRUltrawide.ini`).

To (re)build the installer after `build.ps1` has produced `dist\`:

```powershell
powershell -ExecutionPolicy Bypass -File installer\build_installer.ps1
```

It compiles `GBFRUltrawideSetup.exe` with the .NET Framework 4.x `csc.exe` and assembles
the release layout (exe + `payload\` containing `winmm.dll`, `.asi`, `.ini`) under
`installer\out\`.

## Troubleshooting

The log is at **`scripts\GBFRUltrawide.log`**. For every pattern it records one of:

- **`HIT`** — the pattern was found; the line includes the module-relative offset
  (`granblue_fantasy_relink.exe+0x…`) where the hook/patch was installed.
- **`MISS`** — the pattern was *not* found. Only that single feature is disabled; the mod
  keeps running. A `MISS` after a game update means that pattern needs re-hunting.
- **`FIRED`** — the installed hook actually executed at runtime (logged once per hook).
  A `HIT` with no matching `FIRED` means the hook installed but the game never ran that
  code path — a useful signal that the pattern found the wrong (dead) copy.

If ultrawide isn't applying: confirm the game is v2.0.2, that `winmm.dll` is in the game
root, that `.asi`/`.ini` are in `scripts\`, and check the log for `MISS` lines.

## License & credits

MIT — see upstream [GBFRelinkFix](https://github.com/Lyall/GBFRelinkFix) for the original
work by **Lyall**.

- [Lyall / GBFRelinkFix](https://github.com/Lyall/GBFRelinkFix) — original mod, hook design,
  and ini layout (MIT).
- [ThirteenAG / Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) —
  ASI loading via `winmm.dll` (MIT).
- [safetyhook](https://github.com/cursey/safetyhook) — inline/mid-function hooking, amalgamated
  with Zydis (MIT).
- [inipp](https://github.com/mcmtroffaes/inipp) — ini parsing (MIT).
