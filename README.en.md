# TOTKRecompAndroid

[简体中文](README.md) | English

An experimental static recompilation port of **The Legend of Zelda: Tears of the Kingdom, Nintendo Switch 1.4.3**, for Android ARM64.

This repository is a **fork of [mamcjr/BOTWRecompAndroid](https://github.com/mamcjr/BOTWRecompAndroid)**, renamed to
[ccc007ccc/TOTKRecompAndroid](https://github.com/ccc007ccc/TOTKRecompAndroid). It builds on the upstream recompiler,
Android application and Vulkan runtime, with a different game target. Upstream BOTW compatibility and performance results
are not validation of this TOTK branch. Chinese is the default documentation language; this English edition covers the current project.

## Current status

The game reaches the underground Hyrule Castle scene on a OnePlus 13 with Adreno 830, displaying characters, materials and Chinese game UI.
The user confirmed that character deformation was fixed. With Banner Turnip 27 A8xx and the gather correction enabled,
the user also confirmed that the gloom checkerboard disappeared.

**The port is unfinished.** The upper-left quarter of the game image still clips the torch halo, and large parts of the scene
occasionally become dark or disappear. Audio, save transactions, complete input flows, long sessions and performance need further validation.
The current diagnostic game library uses `-O0`; its frame rate is not an optimized performance baseline.
See [status and known issues](docs/en/STATUS.md) and the [detailed investigation record, in Chinese](docs/TOTK-PORT.md).

## Build and run

You need your own base game, 1.4.3 update and keys. Neither the repository nor the APK includes game code, resources or keys.
All four NSO build IDs and the NPDM program ID are checked. Other game revisions are not currently supported.

On SteamOS, use the Distrobox container named `dev` for builds, dependency installation, generators and tests:

```bash
bash android/build_totk.sh
```

This builds the runtime APK. Initial setup also requires preparing your inputs, generating and compiling `totkgame.so`,
and deploying it to the phone. Follow the [build guide](docs/en/BUILD.md). Runtime-only changes usually require only an APK rebuild.

| Item | Current value |
| --- | --- |
| Game | TOTK Switch 1.4.3, program ID `0100F2C0115B6000` |
| Application | `com.totkrecomp.android`, Android 11+, ARM64 |
| Game library | `totkgame.so`, four-module ABI 2 |
| APK | `android/botwrecompandroid/app/build/outputs/apk/debug/app-debug.apk` |
| Local working data | `.work/totk-port/`, excluded from Git |
| Phone data | `Android/data/com.totkrecomp.android/files/` |

The `botwrecompandroid` directory, Java/JNI names and `LARECOMP_*` variables retain their upstream technical names.
Some inherited UI and tools still need TOTK adaptation; their presence does not imply support.

## Documentation and contributions

- [English documentation](docs/en/README.md)
- [Contribution guide](CONTRIBUTING.en.md)
- [Issue tracker](https://github.com/ccc007ccc/TOTKRecompAndroid/issues): include device, driver, reproduction steps and logs.

## Credits and license

Thanks to [mamcjr/BOTWRecompAndroid](https://github.com/mamcjr/BOTWRecompAndroid),
[peiikai/LASRecomp](https://github.com/peiikai/LASRecomp) and [mamcjr/LASRecompAndroid](https://github.com/mamcjr/LASRecompAndroid).
This port also consults the local `TotK-Native` project and
[Ryujinx Neo's Turnip gather correction](https://github.com/Sunshinin11/Ryujinx_Neo/commit/734fe55f8f7e2d2ae69449374476e360521e4d51).
See [upstream relationships and credits](docs/en/UPSTREAM.md) for details.

The original [MIT license](LICENSE) and component-specific licenses and copyright notices are retained.
They do not authorize distribution of Nintendo game data, fonts or other assets. This is not an official Nintendo project.
