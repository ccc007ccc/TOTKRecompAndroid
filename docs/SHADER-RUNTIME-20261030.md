# 2026-10-30：着色器准备结果进入运行时

用户要求参考 D2R Mobile 和 Eden 的着色器编译架构，减少游玩时编译卡顿。本轮接着 [架构审查](SHADER-ARCHITECTURE-20261030.md) 落地四项机制。
代码阶段遵守“先写代码，不上手机测，我在用”；所有编译、测试在 Distrobox `dev`，该阶段没有 ADB、设备查询、安装、启动或传输。
同日用户随后恢复手机授权，已安装新测试包并补测；结果与限制见 [手机补测](PHONE-SHADER-EXPERIMENTS-20261030.md)。
一加 13 / 8 Elite、真实 2× / 60 FPS、转视角和首次落水后的持久 20 FPS 问题仍保留，下面的本地验证不代替实机成绩。

## 当前路径

完整 NVN control/code/constants、ShaderKey 和实际配置匹配后，图形与计算程序直接读取接口元数据及完整校验的 SPIR-V。
运行时保留原来的代码写入、重绑定代次、重启排空与首次 draw/dispatch 等待。缺失、损坏、新状态或源码诊断走正式译码/编译路径。
接口数据包括 cbuf、纹理/图像类型、输入/输出、深度/丢弃、全局内存、局部内存、指令数和 vertex-index 使用；没有第二份 GLSL、guest 地址或持久化 Vulkan handle。
协议有显式大小/枚举/内容摘要，SPIR-V 同时校验 entry-point 阶段。

片元的 Y 翻转、深度裁剪标志在正式译码器中不影响输出，已按阶段规范化，并用实际输入逐项比较源码。
镜像、接口类型、绑定图像格式和 VS 输出掩码继续参与键；顶点裁剪变化有独立反例，必须生成不同的精确源码键。

## 复用、预算与回收

`shader_module_cache.c` 在实际 Vulkan device 内共享不可变模块和反射。同 key 的在途工作合并；最多保留 128 项、64 MiB SPIR-V。
被借用的对象不被覆盖或回收。超出保留预算仍完成所需模块创建，但临时对象不额外保存 SPIR-V；引用结束后释放。
布局仍由程序/管线持有，兼容没有 maintenance4 的设备。重启先停止预热、排空译码/编译、等待 GPU，再回收程序及缓存。

`compile_budget.c` 统一 shaderc、接口派生、模块和驱动管线创建的入口。8 核设备加载最多 4 项，游玩最多 2 项；普通后台分别最多 3 / 1 项。
等待缓存/依赖时不占编译票，前台请求优先；已在后台构建的模块依赖和管线可以提升优先级。
后台模块被取消后，前台重新接手有效 SPIR-V，避免把取消当成程序失败。工作线程创建失败时仍有同步首次使用回退。
预算限制 API 工作项，不宣称能控制驱动内部线程。Maxwell 的全局配置和 RSC2 字符串环继续串行保护。

接口元数据由单独写入线程原子发布，最多 128 个待写身份；同一身份合并，每个 namespace 最多 32,768 个文件。
只淘汰可重建的接口元数据，不删除 SPIR-V、收据、游戏输入或玩家数据。写入失败保留旧文件；准备收据/重启之前排空已接受的发布。
收据仍证明准确 SPIR-V 集合，元数据被移除后允许译码恢复；准备任务可在没有 shaderc 时从现有 SPIR-V 修复元数据。

## 管线种子与加载阶段

标题准备同时覆盖默认计算输入，从正式源码键和 NVN control 的 workgroup 建立计算配方。真实输入产生 192 条不同的源码键/workgroup 组合。
`pipeline_seed.c` 使用显式小端字段与 SHA-256，最多 4,096 条、4 MiB；不写 C struct padding 或驱动机器码。
种子绑定本版本、译码、recipe ABI、编译 namespace 和接口 feature 配置；加载时核对格式/输入/计算 limits，在当前 Vulkan 设备重建。
设备实际成功管线记录继续保留，并更新可重建的图形/计算种子。新安装的图形状态随实际成功记录积累，未声明已经覆盖所有区域或所有格式变体。

加载阶段最多等待已排队管线 15 秒。取消、无 worker 或结束后立即停止等待，并记录 prepared/missing/failed/pending。
SPIR-V 完整、管线创建成功、运行时领用分别记账；部分或失败结果不能冒充全部完成，未知状态仍正常等待所需管线。

## 身份拆分

正式配置和键逻辑位于 `shader_config.c`、`shader_keys.c`、`shader_source_key.h`；先完成提取，再移除 `exec.c` 的整文件身份依赖。
`shader_identity.cmake` 分别计算译码/接口身份、准备/计划身份和 recipe ABI；SPIR-V 编译 namespace 继续由原有编译契约生成。
模块还按实际 device 区分，设备配方继续核对 vendor/device/driver/UUID/features。没有修改兼容白名单或给旧缓存更换文件头。
从旧编译 loader 捕获的源码键 golden 验证 VS/FS/CS、普通/优化模式与实际/回退目标 API，提取后保持原值。

## 本地验证

- ASan/UBSan/TSan：接口 codec、并发原子发布、大小/枚举/摘要损坏、同 key 合并、字节/项数上限、前台优先、依赖提升、后台取消后的前台接手。
- 实际生产函数：图形/计算 handoff、无译码器/编译器命中、不同 host 地址、控制字段变化失效、诊断源码、阶段失败与未消费 SPIR-V 回收。
- 100 次普通/诊断重启，5,000 个程序与 10,000 个管线索引的增长、重绑定/代码写入失效；真实 writer/译码/编译/预热 worker 创建失败回退。
- 自有 1.4.3 RomFS：1,113 个源码请求严格重载，逐字段比对恢复的 ShaderInfo 与真实译码结果，逐字节比较 SPIR-V；192 个计算配方逐项比对准确源码键和 control workgroup。
- 主机 AMD Radeon 780M / RADV：传统/动态渲染、扩展动态状态与兼容布局共 352 个图形/计算值正确，种子模式复用三个模块；验证层零告警。
- 同一主机另从真实准备结果重建并领用 192 条计算管线，验证层零告警。这项只创建管线，没有派发游戏计算或验证手机画面。
- 缓存/字典/完整收据、背景取消与 heap profile 重试、编译 namespace/旧缓存兼容和身份输入独立变更检查通过。

可复跑入口：`test_shader_artifacts.sh`、`test_shader_scheduling.sh`、`test_shader_warm_prepare.sh`、`test_pipeline_warm.sh`、
`test_vk_pipeline_warm.sh`、`test_vk_shader_seed.sh`、`test_shader_identity.py` 和原有程序/生命周期/缓存测试。
私有输入、SPIR-V、种子、APK 和完整日志保存在 `.work/shader-artifacts-20261030/`，不进入 Git。

同一次正式主机 CPU 工具测量，选取各阶段代码段最大的静态输入，准备后重复 5 次：

| 阶段 / record | 原译码＋严格缓存读取 ms | 完整输入哈希＋接口/SPIR-V 直读 ms |
| --- | ---: | ---: |
| VS / 27224 | 7.15 | 1.19 |
| FS / 11780 | 8.22 | 1.26 |
| CS / 23422 | 8.70 | 1.43 |

两条路径都严格校验相同的完整模块；新路径也核对源码键、阶段和 word count，恢复的 `msl` 为空。
这是热文件缓存、固定 host GLSL/FP32/32-bit/GMap/structured 配置和三个大型输入的范围，不含 Vulkan 管线创建或绘制等待，不与前轮绝对耗时作 FPS 收益比较。

## Android 产物

Release / snapdragon-8elite 的普通包与 perftest 包均编译、链接、打包通过，诊断隔离分别验证 off / on，包契约检查通过。
构建使用当前代码树，游戏库身份仍为 `466fe664cb16f8b0`；没有重新生成/编译游戏库或改写身份旁文件。

| 包 | APK SHA-256 | libmain.so SHA-256 |
| --- | --- | --- |
| `com.totkrecomp.android.perftest` | `162964f5b28f12b3af4fafa96abf4ec22fe539db515b02b1db703cd6668ead6f` | `29cfebf099859a6fe2d3a4a1aee3038ccc8ce15865fd5405af54786aef2d1fe5` |
| `com.totkrecomp.android` | `96f13e4a95cdb89c5dd7b87e8a2de45e2c226f776d1977a61881924441582654` | `f5f9e3eaf783d90cb20af08ebbac56123ce7d306ef5862f93f2350012d591c41` |

截至代码阶段只构建未安装，未运行手机画面/路线/性能验收。后续手机补测已另行记录，不能把本轮主机结果当成已经达成真实 2×60。

## 来源与边界

参考版本沿用审查固定的 D2R main `c7ff2f8`、seed-pack `9625ef3`、Eden `035f461`；采用准备接口直读、有限共享、共同预算、独立写入与可取消加载的思路。
本轮是本项目独立实现，没有复制 GPL 的 Eden 代码，也没有采用 D2R pipe-guard 的跳绘制策略。
旧 clear/worker/template 实验保持默认关闭；BC/ASTC 硬件解码和资源同步不重做，不降低分辨率、精度或目标帧率。
