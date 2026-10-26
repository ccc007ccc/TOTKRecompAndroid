# Status and known issues

[简体中文](../STATUS.md) | English

Updated 2026-10-26. The objective is correct, stable TOTK Switch 1.4.3 execution on Android ARM64.
The validated device is a OnePlus 13 (PJZ110, Adreno 830); this branch has no acceptance results for other devices.

## Confirmed progress

| Area | Evidence and scope |
| --- | --- |
| Inputs and loading | Four-module NSO identity, NPDM program ID, version isolation and library ABI checks |
| Startup | The normal APK/Activity/JNI route reaches underground Hyrule Castle |
| Character models | LDC address overlap and signed-offset fixes; explicitly confirmed by the user |
| Gloom checkerboard | Banner Turnip `v27.0.0-20261025-r5-A8xx` plus 8-bit gather correction; the user confirmed its disappearance |
| Shader control flow | Qualcomm proprietary-driver switch fallthrough failure reproduced and fixed; remaining image faults persisted |
| Synchronization | Nonzero NVN barriers now issue GPU dependencies; independent synchronization checks passed |
| Service initialization | Physical mapping, audio workspace, cache and temporary-directory startup blockers cleared |

The driver and gather correction have not been isolated by separate controls in the current scene. Do not attribute the improvement to only one of them.
Earlier negative results remain in the [investigation record](../TOTK-PORT.md).

## Unresolved

- **Torch halo clipping:** the upper-left quarter of the 16:9 image cuts off the halo, including with runtime scaling disabled.
  Run39 located the first fault in bloom upsampling: the actual mip 1 has a zeroed upper-left quarter.
  Unimplemented NVN render-enable and conditional-render state are being checked for incorrectly executed clears.
- **Large-area flicker:** the user captured a frame with characters, flame and UI still visible while much of the background became dark or absent.
  Missing draws, lighting and compositing have not yet been distinguished.
- **Other transparency and shadows:** gloom, fog, particles and flame previously disappeared together; ordinary shadows could flicker independently.
  They have not all been accepted after the driver change.
- Some views encounter an unimplemented vertex `DMUL` instruction; its relation to these faults is unproven.
- Complete audio, input flows, save transactions/crash recovery, long sessions and performance remain unverified.
  The game has produced readable autosaves, but `IFileSystem::Commit` is still a stub.

## Build and upstream state

The application development version is `0.1.0-totk-dev`, distinct from game version 1.4.3 and upstream release numbers.
Upstream `ef7a306` / `v0.1.11-beta` has been merged and the APK builds. Its language, font, touch and lifecycle changes,
and its new policy for game GPU timestamps, still need device acceptance in this branch.

The phone's run39 baseline was installed before that merge and uses Banner Turnip. It is not evidence for the merged APK.
The game library was built with `GAMELIB_FAST_COMPILE=ON`, all functions at `-O0`.

## Inherited functionality

BOTW/LAS address hooks, 60 FPS/widescreen patches, complete on-phone compilation and the Zepp map do not automatically support TOTK.
The reproducible route currently builds all four modules on the development computer and deploys through the APK.
The upstream RomFS file-count table covers BOTW only; TOTK preparation checks module identity, RomFS bounds and deployment manifests instead.
