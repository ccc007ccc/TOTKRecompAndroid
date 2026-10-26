# Runtime architecture

[简体中文](../ARCHITECTURE.md) | English

The project inherits the AArch64-to-C recompiler and Android/Vulkan runtime from
[BOTWRecompAndroid](https://github.com/mamcjr/BOTWRecompAndroid).
Your Switch 1.4.3 executable modules become an NDK-built library; resources are read from the merged RomFS.

```text
Own base NSP + 1.4.3 update + keys
  → streaming extraction, BKTR merge, identity checks
  → ExeFS + romfs.bin + loose movie files
  → AArch64-to-C generation → NDK builds totkgame.so
  → Activity/JNI → four-module loading → game
```

| Module | Generation address | Role |
| --- | --- | --- |
| main | `0x7000000000` | Game code |
| sdk | `0x7004000000` | SDK and HLE boundary |
| subsdk0 | `0x7006000000` | Multimedia, required |
| rtld | `0x7007000000` | Dynamic linker module |

The runtime selects an available guest address window and relocates modules and dispatch tables.
Library ABI 2 checks all four full build IDs against ExeFS. Keep `runtime/game_profile.h` and `tools/totk.py` consistent.

| Location | Responsibility |
| --- | --- |
| tools/gen.py, lifter.py, nso.py | Instruction translation, control flow, NSO input and generation |
| tools/pull_totk.py, prepare_totk.py, totk_romfs.py | Extraction, update merge, input validation and movies |
| runtime/loader.c, gamelib.c, dispatch.c | Loading, library contract and dispatch |
| runtime/kernel.c, services/ | Switch kernel and service replacements |
| runtime/hle/ | SDK replacements and version-specific hooks |
| runtime/nvn/cmdbuf.c, objects.c | NVN recording, objects and texture layout |
| runtime/nvn/maxwell/translate.c | Maxwell shader translation |
| runtime/nvn/vulkan/ | Vulkan resources, synchronization, drawing and diagnostics |
| runtime/ui/ | Launcher, settings, touch controls and overlays |
| android/ | APK, dependencies and NDK library builds |

NVN 55.15 control blocks define shader code and constant ranges. Maxwell shaders become GLSL/SPIR-V.
Turnip can map the guest heap as GPU memory; the proprietary Qualcomm path uses copies and address tables.
Heaps beyond a storage-buffer range use windows.

Texture handling covers Tegra layout, mips/layers, formats, CPU uploads and GPU ownership.
Color, depth, sampled copies and differently sized targets must be checked separately; reused guest addresses can represent normal temporary storage reuse.
NVN support is incomplete; see [status](STATUS.md).

`com.totkrecomp.android` isolates application data from BOTW. Some directories, Java/JNI names and `LARECOMP_*` variables retain their upstream names.
Private inputs, generated code, raw GPU captures and caches live in `.work/totk-port/`.
The local `TotK-Native` project is a semantic reference; this branch does not use its instruction-rewriting executor.
