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

## Latest visual fix confirmed

Run40 implements `SetRenderEnable` and `SetRenderEnableConditional`. At command execution it compares the two 64-bit
NVNcounterData counter values, then gates draws and color/depth attachment clears. Timestamps do not participate;
state changes and counter reports still execute.

Before the fix, clearing a 40×22 rectangle in the 80×45 bloom mip reproduced the captured corruption pixel for pixel.
In frames 1000/1001, the fix skipped 117 color clears and 28 depth clears; the final bloom mip's upper-left region regained valid content.
The user confirmed that this device build fixed the reported halo clipping and scene flicker.

## Remaining validation

- A subsequent user comparison shows overly dark standalone rocks and the base material of luminous rocks. This material/lighting issue is now being investigated.
- Broader scenes, ordinary shadows, transparency and long sessions still need coverage. Current scene acceptance does not cover every game area or device.
- Some views previously encountered an unimplemented vertex `DMUL`; this change does not address it, and its relation to the fixed faults was not established.
- Complete audio, input flows, save transactions/crash recovery and performance remain unverified.
  The game has produced readable autosaves, but `IFileSystem::Commit` is still a stub.
- Conditional rendering uses the existing CPU counter results and SAMPLES_PASSED fallback. It does not implement hardware occlusion queries.

## Build and upstream state

The application development version is `0.1.0-totk-dev`, distinct from game version 1.4.3 and upstream release numbers.
Upstream `ef7a306` / `v0.1.11-beta` has been merged and the APK builds. Its language, font, touch and lifecycle changes,
and its new policy for game GPU timestamps, still need device acceptance in this branch.

The phone now runs run40, which includes the upstream merge and conditional-render fix, with Banner Turnip.
The focused comparison uses `LARECOMP_GAME_DYNRES=1` to preserve the earlier game-resolution behavior;
it is not a separate acceptance test of upstream's new default counter policy.
The game library was built with `GAMELIB_FAST_COMPILE=ON`, all functions at `-O0`.

## Inherited functionality

BOTW/LAS address hooks, 60 FPS/widescreen patches, complete on-phone compilation and the Zepp map do not automatically support TOTK.
The reproducible route currently builds all four modules on the development computer and deploys through the APK.
The upstream RomFS file-count table covers BOTW only; TOTK preparation checks module identity, RomFS bounds and deployment manifests instead.
