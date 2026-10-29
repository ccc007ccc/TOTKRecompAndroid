# 上游关系、同步与来源

简体中文 | [English](en/UPSTREAM.md)

## 项目身份

- 当前仓库：[ccc007ccc/TOTKRecompAndroid](https://github.com/ccc007ccc/TOTKRecompAndroid)，目标为《王国之泪》Switch 1.4.3。
- 直接上游：[mamcjr/BOTWRecompAndroid](https://github.com/mamcjr/BOTWRecompAndroid)，本仓库由它 Fork 后更名，开发目标不同。
- 更早来源：[peiikai/LASRecomp](https://github.com/peiikai/LASRecomp) 与 [mamcjr/LASRecompAndroid](https://github.com/mamcjr/LASRecompAndroid)。

我们沿用重编译器、运行时、Android/Vulkan 基础设施并进行 TOTK 适配。不要将本分支的新功能和问题归为上游发布，
也不要把 BOTW 设备测试、60 FPS、超宽屏、手表地图或端侧编译结果视为 TOTK 已支持。
保留 `botwrecompandroid` 目录和 JNI 类名是接口兼容选择；APK 包名已经独立为 `com.totkrecomp.android`。

## 2026-10-26 同步

已合并 [ef7a306 / v0.1.11-beta](https://github.com/mamcjr/BOTWRecompAndroid/commit/ef7a3062432f33457155020b40595b81c9827a41)，
相对原基线 `61ab82b` 含 13 个提交。吸收字体与多语言、触控相机、编译资源调节、音频退出顺序、资源选择检查及动态分辨率策略。
保留 TOTK 的四模块 ABI、输入身份、包名、数据隔离、人物／着色器修复和诊断。应用版本仍为 `0.1.0-totk-dev`。

合并后 APK 构建通过；功能的实机状态见 [STATUS](STATUS.md)。上游 RomFS 完整性表仍只包含 BOTW，
并非 TOTK 检查。上游新策略默认不向游戏报告帧内 GPU 耗时，`LARECOMP_GAME_DYNRES=1` 可恢复旧策略；
必须在后续实机对照中记录这项变化。

本机回归通过：13 项 TOTK 输入检查、6 项 RomFS 构建及 9 项继承的导入检查、40,000 次成对独占并发更新、
局部间接跳转、8,192 次着色器控制流执行和 96 个 GLSL/SPIR-V 验证。项目 35 份 Markdown 的本地链接与代码围栏检查通过。
这些结果不替代合并后 APK 的实机渲染验收。

本地 `origin` 指向本 Fork，`upstream` 指向 mamcjr/BOTWRecompAndroid。后续同步应先保存正在开发的修改，
在 `dev` 内 fetch 并检查差异，合并后解决语义冲突、构建与运行相关测试，最后提交推送到本 Fork。
不强推，不覆盖尚未提交的适配成果。旧研究全文保留于 Git 历史及下列页面链接的固定上游提交。

## 参考工程与继承致谢

2026-10-29 已按问题选择性适配 IADD3、QUAD_STRIP、全局 arena 一致性，并将 MacNativeGames 清单固定到 `032a05df`。
来源、Vulkan 差异、验证与范围见 [本轮整合记录](UPSTREAM-MERGE-20261029.md)。这不表示已整体合并 BOTW v0.2.3-beta。

- 本地 `TotK-Native`：四模块装载、服务和 NVN 语义参考；未采用其指令改写执行器。
- [Ryujinx Neo gather 修正](https://github.com/Sunshinin11/Ryujinx_Neo/commit/734fe55f8f7e2d2ae69449374476e360521e4d51)：Turnip 子纹素边界补偿参考。
- [Banner Turnip](https://github.com/The412Banner/Banners-Turnip/releases)：用户指定的自定义驱动来源；当前实机使用 A8xx r5。
- [Revivedx/SavageWindRecomp](https://github.com/Revivedx/SavageWindRecomp)、[Nycolazs](https://github.com/Nycolazs)：上游帧节奏与驱动兼容参考。
- [zeldaret/botw](https://github.com/zeldaret/botw)、[open-ead/sead](https://github.com/open-ead/sead)：上游游戏与引擎研究；Dusklight 提供启动器风格参考。
- 图形组件：AMD FidelityFX FSR 1、Qualcomm Snapdragon GSR、SMAA、SDL、Dear ImGui、libadrenotools、volk、Vulkan Memory Allocator、shaderc、Mesa Turnip。
- 其他组件：FFmpeg、LZ4、Capstone、Unicorn、Python、Zig；是否用于当前构建以构建脚本为准。
- 字体与素材：Baloo 2、DejaVu Sans；上游 Android 机器人素材基于 Google 的 CC BY 3.0 作品。上游还注明使用 Claude Code 辅助开发。

许可证及原作者声明保留在 [LICENSE](../LICENSE) 与各组件目录，不用文档摘要替代原许可条件。
供应商文档、许可证和版权原文不因默认中文而改写。
