# Upstream integration and frame timing

[简体中文](../UPSTREAM-INTEGRATION.md) | English

The branch targets TotK Switch 1.4.3 with 30/45/60/120 FPS settings. Requested frame rates and achieved throughput
are separate checks. See [build profiles](BUILD.md) and [phone measurements](PHONE-PERFORMANCE.md).

## Reviewed sources

- [BOTWRecompAndroid](https://github.com/mamcjr/BOTWRecompAndroid): the first 57 commits from `ef7a306` to
  `4572f067`, then 19 more through `567561819724e6f7425b595de301f2dfd4592eef` (v0.2.2-beta).
- [tib-kpl's fork](https://github.com/tib-kpl/BOTWRecompAndroid): reviewed local ports `4d2c20e` and `80bc5c4`,
  merged with `b386cce`; build outputs and unrelated main shader work remain separate.
- [UltraTears PR #27](https://github.com/kuraeow/ultratears/pull/27): open, head `4ea835e4`, base `1970c7ad`.
  Its 120 option passes a parameter; the runtime explicitly says FPS patches and real-time steps are not ported.
  It is not evidence of working uncapping.
- [MacNativeGames](https://github.com/ly05010419/MacNativeGames/tree/ff63a162b35d8b486b941191785d8e957c1f5df2)
  supplies the original MIT 1.4.3 timing and submission-capture mechanisms. The MIT license is retained in
  `runtime/hle/LICENSE-MacNativeGames.txt`. UltraTears GPL implementations are not copied into MIT code.

## Integrated behavior

| Area | Implementation and local constraints |
| --- | --- |
| GPU writebacks | Bounded small/large pools, FIFO across pools and ring wraps, no readback before completion or after timeout |
| Draw and uploads | Cached output masks and word hashes, dynamic UBO plans, correct overlapping-upload invalidation |
| Image feedback | Correct mip/layer views, allocation identities and alias boundaries; TotK row-stride, black-patch and depth fixes preserved |
| Command queries | Atomic page publication and lock-free lookup; fast/locked/ab each passed 480000 concurrent checks |
| Timeline tracing | Nonblocking-counter calls are timed separately to expose driver stalls |
| Generator | FP load/store register semantics, local-return handling, bounded jump-table fallbacks; new inputs must be regenerated |
| Build service/resume | Foreground service and bounded wake lock; compile/link flags and all four complete module IDs checked; missing-symbol managed libraries are invalidated |
| Native profiles | Runtime/hooks and every game unit use O3/ThinLTO in performance profiles; giant O0 fallbacks and optnone are prohibited |
| FPS timing | Guarded 1.4.3 producer/controller/cloth/bounce hooks, direct BL/LR retained, Android surface and ADPF budgets match all four rates |
| Submission data | Optional UBO/SSBO/CopyBuffer-source capture with inherited bindings, resource bounds, lifetime and GPU-write invalidation |
| Vertex sampling | Upstream 1/256-texel quantization is opt-in with `LARECOMP_VS_SNAP=1`; `LARECOMP_NO_VS_SNAP` takes precedence. ShaderKey separates variants, size queries clamp to valid mips, and the existing gather correction remains enabled |

The old 20 FPS mode waits three 60 Hz VI periods. Removing that wait alone speeds up the game. The new path supplies
measured time to TotK's producer while preserving its half-step and slow-motion choices. Five-frame averaging has no
1/120-second lower clamp; long pauses reset history and physics steps stop at 1/15 second. Cloth's second clock and
the >100 FPS bounce condition are patched separately. `LARECOMP_FRAMEPACE=0` retains the original path. Videos stay
at 30; TotK menu/event policies still need gameplay checks and do not borrow BOTW addresses.

`LARECOMP_VK_SUBMIT_SNAPSHOT=1` enables experimental data capture; it is off by default. Limits are 16 MiB/request
and 64 MiB in flight. Unsafe or exhausted captures wait for translation before submit returns. The frozen capture
boundary is published after CPU-visible GPU writebacks, and later replay cannot use bytes captured before an
unretired GPU write. Inline/copy/storage writes drop intersecting captures; unknown STG footprints conservatively
drop the heap. Shader/arena uploads retain request payloads only until their bytes have been copied. CPU checks
cover bounds, aliases, budgets, inherited state and 10000 producer/replay requests; device validation is separate.

Sync lag remains disabled because of terrain/water regressions. Native ARM64 experiments, BOTW 1.9 addresses, DLC
and BOTW save-layout checks are not applied to TotK without revision-specific validation. The newer 64 KiB direct
UBO experiment was subsequently disabled by upstream; the safe copy path remains. Local writeback ordering already
covers the latest FIFO correction. Controller calibration and general save-transfer features need their own TotK
input/data review.

All three APK profiles have built successfully with verified package IDs, icons, debug flags, signatures and actual
native commands. The full O3/ThinLTO 8 Elite library has linked, deployed and completed short 60-target measurements.
The O1/O3 runs used different temperatures and charging conditions, so they establish no performance gain or regression;
the completed comparison is not being repeated. See [phone measurements](PHONE-PERFORMANCE.md). All four target paths
have low-throughput device records; sustained rates above 30 and 120-FPS gameplay are not established.

Routine tests now load the selected save, measure at the 60 target without pausing, and exit automatically.
The user confirmed that intermittent missing grass also occurs in the ordinary app; it remains deferred and the
vertex-sampling option is not a proven fix. The automatic test entry can leave the loading-screen grid black;
the user reports normal manual loading and has deferred this diagnostic-only issue.

Sampling validation covers 50 GLSL/SPIR-V cases, including negative/out-of-range LODs, plus default unsnapped coordinates
and separate cache identities. Timing, generator, upload/writeback, build-profile, package-identity, submission-snapshot
and automatic-exit checks are also recorded. These checks do not establish complete gameplay correctness.
