# 2026-10-30：相关项目的优化方案审查

本轮用户要求用 Git 查相关项目、寻找优化方案。基于本仓库 `2ce0d3a`，没有改运行时代码、构建/安装 APK 或操作手机。
目标保持一加 13 / Snapdragon 8 Elite、真实 2× / 60 FPS，包含转视角。旧实测 GPU 约 33 ms、渲染 CPU 约 30 ms
只是当时负载证据；最新 worker/描述符实验还没有手机性能对照，不能当作已经降低这些时间。

## 固定审查版本

| 项目 | Git 版本 | 本轮核查重点 |
| --- | --- | --- |
| [mamcjr/TOTKRecompAndroid](https://github.com/mamcjr/TOTKRecompAndroid/commit/cc4f1346773b97ab799d49e2c0c3e1ebbb8318c8) | `cc4f1346773b97ab799d49e2c0c3e1ebbb8318c8` | 远端 HEAD 与前轮相同，没有新的优化可据此合并；帧率候选误判记录继续保留 |
| [MacNativeGames](https://github.com/ly05010419/MacNativeGames/commit/c02d28f6cd784295540edd9808db0e312d60d534) | `c02d28f6cd784295540edd9808db0e312d60d534` | 打包时预建管线、管线预测、合并回读与最新共享清单 |
| [TOTK Windows 分支](https://github.com/itzzslimechisel/totkrecomp/commit/b1b3d2daf21b0e9f725333e0ec14a527d0c0f5f5) | `b1b3d2daf21b0e9f725333e0ec14a527d0c0f5f5` | Dynarmic 独占锁、写保护块跟踪、预热索引；CPU 后端与本项目不同 |
| [Eden](https://git.eden-emu.dev/eden-emu/eden/commit/035f4611cb8f369d11e97651aff1988e60e8a120) | `035f4611cb8f369d11e97651aff1988e60e8a120` | 最新回退、descriptor buffer、缓存与调度 |
| [Ryujinx](https://git.ryujinx.app/ryubing/ryujinx/commit/475615f0431b4995b03c930c5089d20f6f3570e0) | `475615f0431b4995b03c930c5089d20f6f3570e0` | 分类脏位、镜像缓冲失效和地址范围索引 |
| [DXVK](https://github.com/doitsujin/dxvk/commit/78cdf8f1e62954eda30c382dfb13e65d6d43ba4b) | `78cdf8f1e62954eda30c382dfb13e65d6d43ba4b` | 延迟清除、附件读写跟踪、按资源范围形成屏障 |
| [Mesa / Turnip](https://gitlab.freedesktop.org/mesa/mesa/-/commit/15c66721a523be25a08a95859353e9a06be59dc9) | 最新 `15c66721a523be25a08a95859353e9a06be59dc9`；另核对此前驱动日志的基点 [`ab1502fd1606633e910212da185e92fa6bd4cde1`](https://gitlab.freedesktop.org/mesa/mesa/-/commit/ab1502fd1606633e910212da185e92fa6bd4cde1) | LRZ 禁用规则、conservative depth、descriptor buffer / BDA 声明 |
| [Strato](https://github.com/strato-emu/strato/commit/ae1566a48285816a87e81d4aeb40bd2f4e56e60b) | `ae1566a48285816a87e81d4aeb40bd2f4e56e60b` | 分区 trap 管理、重叠区间与锁；不是新版本性能修复 |

通过 Git 远端、提交正文/差异及具体源码核查，原始证据与只读参考 checkout 保存在忽略的
`.work/upstream-performance-20261030/`。Eden/Strato GPL 源码只作机制参考；本轮没有导入其代码。
DXVK 为 zlib/libpng 许可，MIT 工程和 Mesa 文件的实际来源/许可仍须在后续实现中逐项保留。

## 优先推进的候选

### 1. 对原样深度输出声明 `depth_unchanged`

后续统计：自有 1.4.3 的 13,617 个片元输入中，73 个输出深度，原样回退路径为 **0**。
这个窄候选暂不加开关；已转向新 pass 首次完整清除合并，见 [代码与验收记录](PERFORMANCE-PASS-CLEAR-20261030.md)。
下文保留 Git 审查时的候选依据，不能当作后续已实现或实测收益。

**最适合先做小范围代码实验。** 当前 `translate.c` 在 SPH 声明深度输出、对应寄存器没有使用时生成
`gl_FragDepth = gl_FragCoord.z`，但没有 conservative-depth 声明。
Turnip 基点与最新版本的 [`tu_lrz.cc`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/ab1502fd1606633e910212da185e92fa6bd4cde1/src/freedreno/vulkan/tu_lrz.cc#L1352)
都明确：`FRAG_DEPTH_LAYOUT_NONE/ANY` 会阻碍 LRZ，`FRAG_DEPTH_LAYOUT_UNCHANGED` 可保留它，另受 FS 副作用与 drirc 覆盖影响。
不是只有新驱动才有这条路径，但这次没有查询设备当前二进制能力/配置。

候选只对已证明最终输出恒等于 `gl_FragCoord.z` 的生成路径加入 `layout(depth_unchanged) out float gl_FragDepth`。
真实计算深度、原深度精度/NoContraction 修复和 discard/stencil 语义保留；不使用强制 early-fragment-tests 改变程序行为。
先统计实际活跃/缓存 FS 中这种路径的数量，做译码正反例和 GPU 深度/颜色对照，再量 LRZ 及真实 2× 帧时间。
当前没有命中比例或 FPS 收益，不能说它解决了全部 LRZ 失效。

### 2. 延迟并合并清除，优先减少深度 `vkCmdClearAttachments`

DXVK 的 [`batchClear` / `deferClear` / `hoistInlineClear`](https://github.com/doitsujin/dxvk/blob/78cdf8f1e62954eda30c382dfb13e65d6d43ba4b/src/dxvk/dxvk_context.cpp#L2037)
把尚未使用附件的清除合入 loadOp，在读取或覆盖冲突时才落实。Turnip 在深度 `vkCmdClearAttachments` 后禁用 LRZ，
见同基点 `tu_lrz.cc` 与 `tu_clear_blit.cc`。本项目 `clear()` 先开始 pass，再调用该 API；已有“新颜色附件首次写入 CLEAR”
不是 NVN 显式清除的延迟合并，深度也没有对应状态。

先把**可证明完整覆盖、全通道/全 stencil mask、且在第一次使用前发生**的清除折入 loadOp CLEAR，保留最终清零值和 STORE。
在采样、复制、CPU 回读、别名切换、提交或目标变化前落实待清除内容；局部/掩码清除继续原路径。
完整覆盖必须按实际缩放、mip/layer、最小 render area 与深度图像范围判断，不能只看 guest scissor。
后续再基于附件访问与寿命减少不必要的 pass 切换，评估是否让部分通道适合 GMEM。

`DiscardColor` 的零值行为曾修复掉落地板后的黑区，**不能直接换成 DONT_CARE/no-op**。
不要全局改 STORE，也不重跑已无收益的“删 pass 屏障”或“强制 GMEM”。
DXVK 最近修复了 [`ac400e40`](https://github.com/doitsujin/dxvk/commit/ac400e40d96880fa67b543fe4bb8308617888ddb)
的 depth/stencil aspect 交集判断和 [`14260d1d`](https://github.com/doitsujin/dxvk/commit/14260d1da135995beba53f1b2b9909d150eea950)
的多层 clear 视图；这些是本项目新增测试应覆盖的具体边界。

### 3. Descriptor buffer / 带代次的描述符复用

Eden [`vk_graphics_pipeline.cpp`](https://git.eden-emu.dev/eden-emu/eden/src/commit/035f4611cb8f369d11e97651aff1988e60e8a120/src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp#L568)
在 payload 不变且 descriptor ring generation 一致时复用，并调用 `TouchFrame` 延长在途寿命。
`pipeline_helper.h` 检查扩展、布局尺寸、combined-image-sampler 格式和 push-descriptor 条件，失败回退。
Turnip 的已测日志基点 `tu_device.cc` 声明 descriptor buffer、single-array combined sampler 与 BDA 支持；仍需使用前读取实际 PFN/feature/property。

本项目刚实现的模板只避免写结构/双哈希，仍由原 pool 分配新 set，跨提交不能复用。
下一层候选是独立 descriptor ring、按 generation 和资源代次复用，消除频繁分配与池复用开销。
**不能仅去掉 seq 判断**；必须保护 descriptor 字节、buffer/view 代次、GPU 最后使用序号，并处理提交与重启。
动态 UBO 也需重新表示，不能把当前 dynamic-descriptor 布局原样换成 descriptor buffer；worker 包装需新增 API 指针参数的深复制规则。
先做三种路径的实际 GPU 等价/复用/退役测试，再单独测 CPU、GPU 和长帧。原模板与 worker 的手机对照也仍待执行。

### 4. 启动预热索引与有界验证

Windows 分支 [`8754041d`](https://github.com/itzzslimechisel/totkrecomp/commit/8754041db3ad1d9f25b8ced26052ae5cea8c106a)
把约两万个小 variants 文件换成一次读取 `index.tsv`，作者报告无新增工作时 54 s → 1.5 s。
本项目的耗时来源更深：`shader_receipt::check` 每个 key 调 `vk_shader_cached`，后者实际完整解压、校验模块和字典，检查后又释放。
因此不能把上游这 14 行当作我们同样的加速修复。

候选是统一已验证模块索引、去重/缓存字典和验证结果，增量检查变化，保留使用时的内容校验与损坏回退。
runtime compile 关闭时完整准备门禁必须继续证明全部模块可用；不能退回只认身份/文件存在的旧 `.shaders-ready`。
这主要减少启动和后台 CPU/功耗，不据旧预热开关无差别的短测声称稳态 60 FPS。
MacNativeGames [`94b844a5`](https://github.com/ly05010419/MacNativeGames/commit/94b844a58c7a8c9b1e22850c69d8857f1511332b)
的打包 Metal archive 是平台特定管线，不可当作 Adreno Vulkan 管线导入；本项目已有设备 driver cache 与实际预热，不重复搭一套。

### 5. GWin 常用地址范围的直接寻址；BDA 作为后续选项

当前 `gwin_mode` 覆盖大于单个 storage descriptor 的堆，FS/VS 全局访问经过 GMap 的 last-hit/binary lookup。
它不是新发现的“所有纹理 CPU 解码”；BC/ASTC 已硬件解码。可先给已映射、落在 GMem descriptor 前缀内的完整访问增加直接路径，
超出前缀继续窗口回退，减少热循环的寻址工作；必须限定 **placed GWin**，不能让 GPU 绕过 copy-mode 快照去读 guest 内存。
更进一步的 BDA 可参考 Turnip 的设备地址实现，但需要完整范围、向量跨界、别名、GPU 写入/回读与缓存身份测试。
目前没有证明 GMap 占 GPU 时间的比例，不能因堆约 2120 MiB 就认为其全部负载都需要 BDA。

### 6. 地址区间索引，以及当前恒为零的 Tint 专用变体

Ryujinx `BufferCache.cs` / Strato `trap_manager.cpp` 都按范围管理资源。
本仓库写通知仍持 `g_live_lock` 遍历 `g_all_tex`，纹理大小已缓存，下一步可只查相交区间，保留多描述符别名和回收顺序。
它对应已有游戏线程热点，优先级低于 GPU 主线；这不要求恢复已崩溃的主堆页保护。

另一个本地候选：普通 draw 的 `tint[8]` 始终为零，但大量 FS 仍声明 binding 19 和 tint 分支。
可把未启用的效果作为编译变体，去掉该 uniform/分支；需同时调整准备与着色器身份并验证颜色/精度。
这是本轮对照源码发现的具体裁剪候选，还没有测试或收益数据，不以“降精度/跳绘制”替代它。

## 最新共享清单的实际增量

用本项目正式 importer/原始 parser，按自有 1.4.3 的 28,634 个 raw keys 过滤新清单；新旧 parser 字节相同。
只导出审查元数据，没有更换 vendored 文件、生成 SPIR-V、预建管线或修改玩家缓存。

| 项目 | 当前固定 `032a05df` | 上游 `c02d28f6` |
| --- | ---: | ---: |
| 全清单 warm 行 | 73,397 | 74,572 |
| 匹配本地 1.4.3 的 warm 行 | 39,757 | 39,948 |
| 匹配的 variants | 136 | 136 |
| seed 请求 | 28,770 | 28,770 |

新增匹配 **191 行**：R 94、T 95、B 2；无删除、无新增 variant。上游有 1.2.1/mod 行，不能把总新增数当作本版本覆盖。
这些仍是 Metal 管线/边框/tile 元数据，需要正式 Vulkan 适配；不等于 191 个现成可导入管线，也不能称修复首次入水卡顿。
原始清单 SHA-256 `a99d173f1676c275a99e078be64aabf0e5b34c5135a2f99bb73c4b3176dc3f88`，完整审查结果在私有 `shader-list-delta.json`。

## 暂不直接移植的结果

- Windows [`809bf8bf`](https://github.com/itzzslimechisel/totkrecomp/commit/809bf8bfd885c91687678dfde163b2b5424847ff)
  的约 38→61 FPS 来自 Dynarmic 全局独占锁；本项目 `cpu.h` 已是 scalar/128-bit CAS，无这把 monitor 锁。
  上游数据为 1.0.0 / RTX 3070，不能当作 Android 1.4.3 的可复用收益。
- Windows [`393962d2`](https://github.com/itzzslimechisel/totkrecomp/commit/393962d21cbfd67b1e141df7612ce6a711d4895c)
  的 99% chunks 跳检查来自页保护与 Windows vectored handler；本项目主堆保护曾 SIGSEGV，需先厘清信号链、GPU 映射与复用。
- Eden 今天 [`b4d57600`](https://git.eden-emu.dev/eden-emu/eden/commit/b4d5760014176bfc42b60b7dc5661240c97c8e3b)
  撤回 #4490/#4531 的 NCE invalidation 回读，正文说明成本、设备性能回退和缺少根因修复。
  不照搬该回读；它与本项目页保护崩溃是不同机制。
- Mac [`e13f0308`](https://github.com/ly05010419/MacNativeGames/commit/e13f03088e666138a8942500ebfcef407f2e3f08)
  合并 mip/layer 回读在 BOTW 的特定 cubemap 有收益，但其 TOTK 对照 11.69/11.75 ms 未显示收益；我们的回读/提交已有批次保护，
  需先确认重复等待的实际命中，不能重做已存在路径。
- 不推广过场误判、不强制 GMEM/sysmem、不删同步，不把金属管线 archive、JIT CPU 快路或其他游戏成绩直接标为可合并。

## 建议的落地次序

先做原样深度输出的统计与 conservative-depth 等价实验，再做完整清除合并与附件访问统计。
两项直接对应已有 LRZ/多附件 GPU 证据，代码阶段可以先在 dev 完成，不需要手机。
descriptor buffer/代次复用、启动验证索引随后独立推进；寻址与区间索引按实际命中决定投入。
所有性能候选最终仍需在用户允许手机后，用真实 2×、60 目标、相同路线和缓存做 FPS/长帧与画面对照。
