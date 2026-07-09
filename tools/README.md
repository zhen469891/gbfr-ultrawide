# tools\ — offline disassembly analysis tool

`gbfr_analyze` is a development-time offline CLI for statically analysing
`granblue_fantasy_relink.exe`: disassembly, byte-pattern scanning, and rip-relative
cross-reference search. Its purpose is to **relocate patterns after a game update** — see
[`docs/PATTERNS.md`](../docs/PATTERNS.md) and
[`docs/adr/0005-per-build-pattern-porting.md`](../docs/adr/0005-per-build-pattern-porting.md):
every game update lets the compiler reorder instructions, reallocate registers, and move
struct layouts, so old byte signatures flip from `HIT` to `MISS`. This tool helps you re-find
the injection points using stable data anchors (data tables, constants, struct offsets)
instead of relying on fragile instruction-byte shapes.

The tool is fully offline and read-only: it only reads the exe file's bytes; it never loads
or runs the game.

## Section / RVA mapping

`gbfr_analyze` operates on the `.text` section and displays addresses as **RVA**. For the
PE layout of `granblue_fantasy_relink.exe` v2.0.2 (hardcoded as source constants):

```
.text  RawPtr = 0x400,  VirtAddr = 0x1000   =>   RVA = fileOffset + 0xC00
.text  file offset range: [0x400, 0x049AFE00)
```

That is, `RVA = fileOffset + 0xC00` (constant `kRvaDelta = 0xC00`). Scanning covers `.text`
only, deliberately avoiding the SteamStub-obfuscated `.bind` section to avoid false positives.
If a future game version changes the PE layout, update the `kTextRawPtr` / `kTextRawEnd` /
`kRvaDelta` constants at the top of `gbfr_analyze.cpp` accordingly.

## Build

Requires Visual Studio (with C++ tools; Build Tools is fine). From the repo root:

```powershell
powershell -ExecutionPolicy Bypass -File tools\build_tools.ps1
```

The script locates MSVC via `vswhere`, loads `vcvars64`, then compiles
`tools\gbfr_analyze.exe` with `cl.exe`. It compiles the repo's vendored amalgamated Zydis
(`vendor\safetyhook\Zydis.h` / `Zydis.c`) in as well, so no separate Zydis download is needed.

## Usage

Pass the exe path with `-f` (strongly recommended; if omitted it falls back to a hardcoded
default Steam install path, for local developer convenience only).

```
gbfr_analyze [-f exePath] disasm <fileOffsetHex> <numBytes>
gbfr_analyze [-f exePath] scan   <pattern with ?? wildcards>
gbfr_analyze [-f exePath] xref   <targetRVAHex>
```

> The game exe path usually contains spaces — wrap it in double quotes.

### `scan` — byte pattern scan

Scans `.text` for a byte signature; `??` is a wildcard byte (a single `?` also works).
Prints each hit's fileOffset and corresponding RVA. Use it after an update to confirm whether
an old pattern still hits uniquely, or to find new injection points.

```powershell
tools\gbfr_analyze.exe -f "D:\Steam\steamapps\common\Granblue Fantasy Relink\granblue_fantasy_relink.exe" `
  scan "48 8D 05 ?? ?? ?? ?? C5 7B 10 14 C8"
```

Output (FPSCap pattern, should hit uniquely at RVA 0x001B6E63 on v2.0.2):

```
HIT 1: fileOffset=0x001B6263  RVA=0x001B6E63
Total hits: 1
```

### `disasm` — disassembly

Disassembles `numBytes` bytes from a given **file offset** (hex); addresses are shown as RVA,
one instruction per line with RVA, raw bytes, and Intel-syntax mnemonic. Use it to confirm the
instruction shape at an address, a hook's landing site, or to read the table address a `lea`
points to.

```powershell
tools\gbfr_analyze.exe -f "D:\...\granblue_fantasy_relink.exe" disasm 1B6263 32
```

Output (near the FPSCap hit; the first `lea` points at the frame-time table RVA 0x054D6BF0):

```
0x001B6E63  48 8D 05 86 FD 31 05            lea rax, [0x00000000054D6BF0]
0x001B6E6A  C5 7B 10 14 C8                  vmovsd xmm10, qword ptr [rax+rcx*8]
0x001B6E6F  90                              nop
...
```

> Note: the first `disasm` argument is a **file offset** (not an RVA). If you have an RVA,
> convert: `fileOffset = RVA - 0xC00`.

### `xref` — rip-relative cross-reference

Brute-force scans all of `.text` for every rip-relative disp32 field whose computed target
equals the given **target RVA**. Use it when you know a data table's / function's RVA and want
to find who references it. Confirm each candidate with `disasm` to verify it is a real
instruction (the tool only matches the disp32 value; it does not parse instruction boundaries).

```powershell
tools\gbfr_analyze.exe -f "D:\...\granblue_fantasy_relink.exe" xref 54D6BF0
```

Output:

```
XREF 1: disp32 at fileOffset=0x001B6266 RVA=0x001B6E66 (instruction ends at RVA+4; verify with disasm)
Total xref candidates: 1
```

(That is the disp32 field of the `lea rax, [0x54D6BF0]` instruction above.)

## Typical workflow (relocating patterns after a game update)

1. Run the old `scan` pattern to check whether it still hits uniquely; if it MISSes or hits
   multiple times, switch to a stable anchor.
2. Starting from a stable data-table RVA (see PATTERNS.md), use `xref` to find the
   instructions that reference it.
3. Run `disasm` on the candidate addresses to confirm the instruction shape and decide the new
   hook landing site and offset.
4. Update the new pattern / RVA / offset back into `docs/PATTERNS.md` and the code.

## Files

- `gbfr_analyze.cpp` — tool source (single file).
- `build_tools.ps1` — build script (vcvars64 + cl.exe, compiles vendored Zydis in).
- `gbfr_analyze.exe` — build output (not version-controlled).

## Zydis dependency

Uses the repo's vendored amalgamated Zydis directly (`vendor\safetyhook\Zydis.h` / `Zydis.c`,
Zydis 4.x). The source only `#include`s `"Zydis.h"`; the build script supplies the path via
`/I..\vendor\safetyhook` and compiles `Zydis.c` in, so no second copy of Zydis lives under
`tools\`. That header already defines `ZYDIS_STATIC_BUILD`.
