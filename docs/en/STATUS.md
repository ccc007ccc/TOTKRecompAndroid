# Status and known issues

2026-10-30 shader implementation: after the D2R Mobile / Eden review, prepared interfaces/SPIR-V now enter the runtime
directly, with bounded module reuse, shared compilation budgets, a metadata writer, actual pipeline seeds and separate identities.
Host concurrency/real-input/GPU checks and both Android Release builds passed; see the [implementation (Chinese)](../SHADER-RUNTIME-20261030.md).

The user subsequently authorized phone tests. The new independent perftest package is installed on OnePlus 13.
At actual 2× / 60 target with camera rotation, the old reference reached 23.95 FPS and the new first/repeated runs 18.30/22.40.
Worker, descriptor-template and clear experiments reached about 20.7–21.4 without a demonstrated stable gain.
Repeated interface loading on/off reached 20.46/19.95; the much slower first off run is insufficient to claim a stable percentage gain.
Compilation hitches before sampling remain. The user reports water-related slowdowns across locations, more obvious underground.
The opening underground scene reached 27.54 FPS over 60 seconds, with later windows at 22–25 while interval remained 1 and thermal status rose 2→3.
A separate warm-off profiling run measured about 13.44 ms/frame of GPU synchronization waits and negligible frame-limit waits;
its FPS is not a comparable ordinary result, and the water-related cause remains unidentified.
See the [phone record (Chinese)](../PHONE-SHADER-EXPERIMENTS-20261030.md). Historical statements below about no phone access
describe the earlier code stages. Actual 2×60 and the water route remain unverified.

2026-10-30 clear experiment: none of 13,617 static fragment inputs uses the identity-depth fallback.
The first full clear at a new pass can now use loadOp CLEAR. Production CPU regressions, 120 host GPU comparisons
and both Release builds/isolation checks passed. Diagnostic-only, disabled by default; no phone was used.
General deferred batching and actual 2×60 remain unfinished; see the [record (Chinese)](../PERFORMANCE-PASS-CLEAR-20261030.md).

2026-10-30 Git performance review: DXVK, Turnip, Eden, Ryujinx, MacNativeGames and a TOTK Windows branch were checked
against fixed commits. Identity-depth declarations and deferred full clears are the first GPU candidates; descriptor
rings, validation indexes and address lookup follow. The latest list adds 191 matching 1.4.3 pipeline metadata rows,
with no new variants. Runtime and phone state were untouched; see the [review (Chinese)](../UPSTREAM-PERFORMANCE-20261030.md).

2026-10-30 follow-up: descriptor update templates and category comparison of resolved values passed actual `stage_set`,
failure/restart and host GPU legacy/template/template+worker regressions, plus both Release builds and isolation checks.
The experiment remains disabled by default and diagnostic-only; no phone access or FPS gain was measured.
See the [descriptor record (Chinese)](../PERFORMANCE-DESCRIPTOR-UPDATES-20261030.md). Skipping input resolution from NVN dirty bits,
persistent cross-submission reuse and actual 2×60 remain unachieved.

2026-10-30 code work: an opt-in Vulkan recording worker passed snapshot/thread/request-queue regressions, host Vulkan
validation and diagnostic/ordinary Release builds. It remains off by default and excluded from ordinary Release.
The phone was not accessed at the user's request; Android gameplay and FPS gains remain unverified.
See the [worker record (Chinese)](../PERFORMANCE-VULKAN-WORKER-20261030.md). Descriptor work subsequently progressed as described above; actual 2×60 remains unfinished.

[简体中文](../STATUS.md) | English

Updated 2026-10-28. The objective is correct, stable TOTK Switch 1.4.3 execution on Android ARM64.
The validated device is a OnePlus 13 (PJZ110, Adreno 830); this branch has no acceptance results for other devices.
The current performance target is sustained 60 FPS at the actual 2× setting, including camera rotation.
BC/ASTC already use hardware sampling decode. GPU upload-layout conversion and write-notification comparisons are
tracked in the [texture investigation (Chinese)](../PERFORMANCE-TEXTURES-20261030.md); the full target remains unachieved.

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
| Black rectangles on the blue spawn platform | Corrected the default Tegra tile-height rounding boundary; same-process/view comparison removed the rectangles, and the user confirmed the fix |
| Shader control flow | Qualcomm proprietary-driver switch fallthrough failure reproduced and fixed; remaining image faults persisted |
| Synchronization | Nonzero NVN barriers now issue GPU dependencies; independent synchronization checks passed |
| Service initialization | Physical mapping, audio workspace, cache and temporary-directory startup blockers cleared |
| Frame-rate targets | 30/45/60/120 choices, guarded 1.4.3 timing/cloth/bounce hooks and Android refresh requests run in low-throughput scenes; sustained rates above 30 and 120-FPS gameplay remain unproven |
| Optimized builds | Runtime and all 1755 generated units built and deployed with O3/ThinLTO and 8 Elite tuning, without O0 fallbacks; package variants and library identities are checked |
| Automatic testing | Complete isolated save copies, direct slot selection, continuous timed measurement at the 60 target and automatic exit; paused diagnostics remain explicitly available |

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

- On 2026-10-29 the missing vertex driver point-size limit was filled, restoring grass interaction-map updates; the user reported that grass had recovered. Earlier captures of field-wide disappearance with an empty terrain-normal input are retained; this short run is not long-session acceptance. See [grass and audio follow-up (Chinese)](../GRASS-AND-AUDIO.md). Vertex sampling quantization stays opt-in.
- The automatic-loading black grid was fixed on 2026-10-29 by preserving the normal menu confirmation flow and stopping test input when the selected progress file is read. See [visual validation](../VISUAL-VALIDATION.md); the historical intermittent UI fault below remains separate.
- On 2026-10-29 the user reported that UI flicker appears to have recovered alongside other changes. Its fixing commit and mechanism are unknown, so it is no longer listed as a currently reproduced fault. Historical single-frame sky-island blackouts were not separately reconfirmed. Capture controls provide lightweight stage/state diagnostics by default, with full replay as an advanced option; see [testing](TESTING.md#on-device-flicker-capture).
- Broader scenes, ordinary shadows, transparency and long sessions still need coverage. Current scene acceptance does not cover every game area or device.
- Some views previously encountered an unimplemented vertex `DMUL`; this change does not address it, and its relation to the fixed faults was not established.
- Other audio scenes, effect types not yet observed, input flows, save transactions/crash recovery and performance remain unverified.
  The game has produced readable autosaves, but `IFileSystem::Commit` is still a stub.
- Conditional rendering uses the existing CPU counter results and SAMPLES_PASSED fallback. It does not implement hardware occlusion queries.
- ReadTexels covers CPU texture storage; invalidation/readback after GPU writes remains incomplete.
- Dismissing a runtime error retains the failed thread without resuming its failed call. Dependent game functions may still stop.

## Build and upstream state

The application development version is `0.1.0-totk-dev`, distinct from game version 1.4.3 and upstream release numbers.
The base includes upstream `ef7a306` / `v0.1.11-beta`. Further changes were reviewed through `5675618197` / `v0.2.2-beta`
and selectively adapted, including two tib-kpl runtime ports. See [integration](UPSTREAM-INTEGRATION.md) for sources,
implemented behavior and validation limits. The performance/frame-timing branch is merged into `main`; subsequent testing and fixes use `main`.

The latest cloud-fix acceptance is run50 with Banner Turnip, retaining conditional rendering, texture reads, compressed tails, error dialogs, audio, water reflections and the sky-island strip fix.
The focused comparison uses `LARECOMP_GAME_DYNRES=1` to preserve the earlier game-resolution behavior;
it is not a separate acceptance test of upstream's new default counter policy.
Those early rendering records used the all-O0 game library. O1 and full O3/ThinLTO 8 Elite libraries have since completed
short device measurements. Different temperatures and charging conditions prevent an O1/O3 performance conclusion;
the user declined a repeat comparison. Original results remain in [phone measurements](PHONE-PERFORMANCE.md).

## Inherited functionality

BOTW/LAS addresses, menu/event and widescreen policies, complete on-phone compilation and the Zepp map do not automatically
support TOTK. This branch's four frame targets use separately verified 1.4.3 timing. BuildService and native compiler flags
are adapted, but full on-phone compilation remains unverified; host-built four-module libraries are the validated path.
The reproducible route currently builds all four modules on the development computer and deploys through the APK.
The upstream RomFS file-count table covers BOTW only; TOTK preparation checks module identity, RomFS bounds and deployment manifests instead.
