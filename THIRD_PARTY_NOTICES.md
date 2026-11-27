# Third-party notices

The project requires a locally supplied Stunt Race FX ROM. The source and portable launcher ship without the ROM or extracted game assets. Stunt Race FX and its original content belong to their respective rights holders.

## Distributed dependencies

- The portable toolchain includes LLVM-MinGW, CMake, and Ninja. The distribution retains the notices supplied with each tool.
- The self-contained launcher includes .NET runtime notices under `licenses/`.
- The launcher includes Barlow Condensed Bold Italic by the Barlow Project Authors. The font is licensed under the SIL Open Font License 1.1, included in `licenses/BarlowCondensed-OFL.txt`.
- The native game uses SDL3 3.4.16 under the zlib license and the Khronos OpenXR loader 1.1.53, including JsonCpp. Their notices are included under `source/.tools/`.

## Research references

Super FX format research consulted [sfxObjectDump](https://github.com/Hordeking/sfxObjectDump) (GPL-3.0, `e491badb`) and [stuntrace](https://github.com/sp00nznet/stuntrace) (MIT, `2870eb3e`). [Snes9x](https://github.com/snes9xgit/snes9x) (`fae2fea`) served as an offline reference. The native game and launcher do not include these projects.

A distribution license for the application source has yet to be selected.
