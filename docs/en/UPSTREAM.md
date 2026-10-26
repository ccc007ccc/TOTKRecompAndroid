# Upstream relationships and credits

[简体中文](../UPSTREAM.md) | English

## Identity

- Current fork: [ccc007ccc/TOTKRecompAndroid](https://github.com/ccc007ccc/TOTKRecompAndroid), targeting TOTK Switch 1.4.3.
- Direct upstream: [mamcjr/BOTWRecompAndroid](https://github.com/mamcjr/BOTWRecompAndroid). This fork was renamed because its game target differs.
- Earlier foundations: [peiikai/LASRecomp](https://github.com/peiikai/LASRecomp) and [mamcjr/LASRecompAndroid](https://github.com/mamcjr/LASRecompAndroid).

The recompiler, runtime and Android/Vulkan infrastructure are inherited and adapted.
Changes and defects in this fork should not be presented as upstream releases. Upstream BOTW tests and features are not TOTK acceptance evidence.
Historical directory/JNI names are retained for compatibility; the application ID is `com.totkrecomp.android`.

## Synchronization on 2026-10-26

Merged [ef7a306 / v0.1.11-beta](https://github.com/mamcjr/BOTWRecompAndroid/commit/ef7a3062432f33457155020b40595b81c9827a41),
13 commits after the former `61ab82b` baseline. This brings font/language support, touch camera controls, compilation resource management,
audio shutdown ordering, folder checks and dynamic-resolution changes. TOTK module identity, ABI, package/data isolation and rendering fixes are preserved.
The fork's app version remains `0.1.0-totk-dev`.

The APK builds. Local checks passed: 13 TOTK input cases; 6 RomFS image and 9 inherited setup cases; 40,000 paired-exclusive updates;
local indirect branches; 8,192 shader control-flow executions and 96 GLSL/SPIR-V validations.
Local links and code fences in all 35 first-party Markdown files were also checked.
These checks do not replace device acceptance of the merged APK.

The inherited RomFS completeness table covers BOTW only. The new counter policy hides intra-frame GPU duration from the game by default;
`LARECOMP_GAME_DYNRES=1` restores the earlier behavior. Record that change when comparing later device runs.

`origin` is this fork and `upstream` is mamcjr/BOTWRecompAndroid. Save current work, fetch and review differences in `dev`,
resolve semantic conflicts, build and test, then commit and push to this fork. Do not overwrite uncommitted porting work or force-push.
Historical research remains in Git and in fixed upstream links from the legacy documentation indexes.

## References and inherited credits

- Local `TotK-Native`: module loading, service and NVN references; its execution backend is not used here.
- [Ryujinx Neo](https://github.com/Sunshinin11/Ryujinx_Neo/commit/734fe55f8f7e2d2ae69449374476e360521e4d51): Turnip gather correction.
- [Banner Turnip](https://github.com/The412Banner/Banners-Turnip/releases): the user-selected driver source; the current device baseline uses A8xx r5.
- [Revivedx/SavageWindRecomp](https://github.com/Revivedx/SavageWindRecomp), [Nycolazs](https://github.com/Nycolazs): upstream frame pacing and driver references.
- [zeldaret/botw](https://github.com/zeldaret/botw), [open-ead/sead](https://github.com/open-ead/sead): upstream game/engine research; Dusklight: launcher styling reference.
- Graphics: AMD FidelityFX FSR 1, Qualcomm Snapdragon GSR, SMAA, SDL, Dear ImGui, libadrenotools, volk, Vulkan Memory Allocator, shaderc and Mesa Turnip.
- Other components: FFmpeg, LZ4, Capstone, Unicorn, Python and Zig; actual usage is determined by the build scripts.
- Fonts/assets: Baloo 2 and DejaVu Sans; inherited Android robot artwork is based on Google's CC BY 3.0 material. Upstream also credits Claude Code assistance.

Original license and copyright notices remain in [LICENSE](../../LICENSE) and component directories.
Documentation summaries do not replace those terms. Vendor documentation and legal text are not rewritten when selecting a default documentation language.
