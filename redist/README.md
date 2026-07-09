# redist

Third-party redistributable files.

## winmm.dll — Ultimate ASI Loader (x64)

- Source: https://github.com/ThirteenAG/Ultimate-ASI-Loader
- Version: v9.7.2 (release asset `Ultimate-ASI-Loader_x64.zip`, which contains `dinput8.dll`, renamed to `winmm.dll`)
- Download URL: https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/v9.7.2/Ultimate-ASI-Loader_x64.zip
- Architecture check: PE header Machine = 0x8664 (x64)
- License: MIT; full text in `UltimateASILoader_LICENSE.md`

The installer deploys this file to the game root so the game automatically loads `scripts\GBFRUltrawide.asi` at startup.
