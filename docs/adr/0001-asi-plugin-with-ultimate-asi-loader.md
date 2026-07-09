# ADR-0001: ASI plugin loaded via Ultimate ASI Loader

- Status: Accepted
- Date: 2026-07-09

## Context

We need to inject resolution/aspect/HUD fixes into Granblue Fantasy Relink v2.0.2, a 64-bit
Steam game with SteamStub DRM. Options for getting our code into the process:

1. **ASI plugin + Ultimate ASI Loader** — a proxy DLL (`winmm.dll`) that the game loads on
   startup, which in turn loads any `.asi` in `scripts/`. This is what the original
   GBFRelinkFix used.
2. A standalone injector process (`CreateRemoteThread`).
3. A DXGI/D3D12 wrapper DLL.

The original project (Lyall's GBFRelinkFix) is MIT-licensed and used approach 1; reusing its
structure lets us port its hook logic with minimal restructuring.

## Decision

Ship a 64-bit `.asi` plugin loaded by **Ultimate ASI Loader** (ThirteenAG), deployed as
`winmm.dll` in the game root, with the plugin at `scripts\GBFRUltrawide.asi`.

## Consequences

- Install is additive: we add `winmm.dll` + `scripts\` and **never overwrite game files**.
  The installer still backs up any pre-existing same-named files before writing.
- `winmm.dll` is a common proxy name and is already the convention users of the original mod
  know. Steam Deck/Proton needs `WINEDLLOVERRIDES="winmm=n,b"`.
- We inherit Ultimate ASI Loader's timing: the plugin's `DllMain` thread runs early, so we
  add a configurable injection delay before installing most hooks (the game must be far
  enough along that the target code is mapped).
- Ties us to a per-build binary; see ADR-0002/0003 for how we keep that maintainable.
