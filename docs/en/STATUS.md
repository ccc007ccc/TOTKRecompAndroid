# Status and known issues

[简体中文](../STATUS.md) | English

Updated 2026-10-27. The objective is correct, stable TOTK Switch 1.4.3 execution on Android ARM64.
The validated device is a OnePlus 13 (PJZ110, Adreno 830); this branch has no acceptance results for other devices.

## Confirmed progress

| Area | Evidence and scope |
| --- | --- |
| Inputs and loading | Four-module NSO identity, NPDM program ID, version isolation and library ABI checks |
| Startup | The normal APK/Activity/JNI route reaches underground Hyrule Castle |
| Character models | LDC address overlap and signed-offset fixes; explicitly confirmed by the user |
| Gloom checkerboard | Banner Turnip `v27.0.0-20261025-r5-A8xx` plus 8-bit gather correction; the user confirmed its disappearance |
| Rock lighting | CPU ReadTexels restores the BC4 occlusion atlas; the user confirmed the rock colors in run41 |
| Opening underground story | Compressed tail blocks can be written inside an atlas; the user confirmed the Ganon encounter no longer triggers the error in run42 |
| Error dialog | Device JNI checks verify copying without dismissal and OK without terminating the app; actual native signal crashes still terminate |
| Character and weapon sounds | Splitters, auxiliary returns, 3D reverb, limiter and initial wave-buffer slot handling implemented; the user confirmed audio recovery in run44 |
| Water reflections and lighting | Explicit fragment depth preserves arithmetic precision, restoring water material selection; the user confirmed the fix in run46 |
| Vertical strips on sky islands | Corrected row strides for linear height/mask textures; the user confirmed the fix in run48 |
| Blocky cloud coverage | Corrected TMML query component order; the user confirmed the fix in run50 |
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

- The user subsequently reported random flashes, with whole-scene and local changes not yet distinguished. A manual on-device capture now retains before/peak/after images and a preview only for abrupt-change candidates. The flashes themselves still need captured evidence; see [testing](TESTING.md#on-device-flicker-capture).
- Broader scenes, ordinary shadows, transparency and long sessions still need coverage. Current scene acceptance does not cover every game area or device.
- Some views previously encountered an unimplemented vertex `DMUL`; this change does not address it, and its relation to the fixed faults was not established.
- Other audio scenes, effect types not yet observed, input flows, save transactions/crash recovery and performance remain unverified.
  The game has produced readable autosaves, but `IFileSystem::Commit` is still a stub.
- Conditional rendering uses the existing CPU counter results and SAMPLES_PASSED fallback. It does not implement hardware occlusion queries.
- ReadTexels covers CPU texture storage; invalidation/readback after GPU writes remains incomplete.
- Dismissing a runtime error retains the failed thread without resuming its failed call. Dependent game functions may still stop.

## Build and upstream state

The application development version is `0.1.0-totk-dev`, distinct from game version 1.4.3 and upstream release numbers.
Upstream `ef7a306` / `v0.1.11-beta` has been merged and the APK builds. Its language, font, touch and lifecycle changes,
and its new policy for game GPU timestamps, still need device acceptance in this branch.

The latest cloud-fix acceptance is run50 with Banner Turnip, retaining conditional rendering, texture reads, compressed tails, error dialogs, audio, water reflections and the sky-island strip fix.
The focused comparison uses `LARECOMP_GAME_DYNRES=1` to preserve the earlier game-resolution behavior;
it is not a separate acceptance test of upstream's new default counter policy.
The game library was built with `GAMELIB_FAST_COMPILE=ON`, all functions at `-O0`.

## Inherited functionality

BOTW/LAS address hooks, 60 FPS/widescreen patches, complete on-phone compilation and the Zepp map do not automatically support TOTK.
The reproducible route currently builds all four modules on the development computer and deploys through the APK.
The upstream RomFS file-count table covers BOTW only; TOTK preparation checks module identity, RomFS bounds and deployment manifests instead.
