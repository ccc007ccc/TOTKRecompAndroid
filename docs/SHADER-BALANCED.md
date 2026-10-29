# 着色器均衡准备方案

本页保留实现与分阶段证据。用户随后将本线范围收回到编译/准备与缓存复用，并暂停继续扩展性能调查；
当前协作边界见 [项目规范](../Agent.md) 和 [整理基线](PROJECT-BASELINE.md)。下文待验收清单不自动开启新测试任务。
随后用户新授权的链路审查与修复见 [着色器链路审查](SHADER-CHAIN-REVIEW.md)；该审查未恢复帧率优化或旧整库任务。

2026-10-29 的用户要求取代此前整库展开目标：不要求全部提前编译，重点是减少游戏中的编译卡顿，优先复用
MacNativeGames 的共享清单和现成准备逻辑。手机继续直接读取自己的游戏文件，默认 FP32，保留整数双精度选项。
准备阶段可使用全部 CPU 核心；游戏阶段的后台工作需按实际帧时间验收。

当前已有缓存优先加载和缺失编译入口，Android 默认 `shader_runtime_compile = 1`。
关闭此项或设置 `LARECOMP_SHADER_RUNTIME_COMPILE=0` 才启用原来的严格完整准备门禁。
`vk_load_prepared_shader` 仍是严格审计接口，不因运行时设置而允许缺失编译。
缓存损坏时启用回退可重新生成；缓存无法写入时仍可使用本次编译结果，不伪造 `.shaders-ready`。

标题准备已接入 Android 文件设置和正常启动，APK 自动生成并携带紧凑清单；标题之后继续低优先级后台准备，优先处理最近使用的状态。
正常启动、取消/重启和前后台生命周期代码已接入；旧构建缓存兼容读取已验收。
驱动管线记录/预热与首次使用等待已实现并通过独立 GPU 检查；固定存档场景的六轮手机启动/帧时间检查已完成，
跨区域路线、缺失缓存时的卡顿和完整画面仍需验收。
旧库可读不代表当前请求命中：此前合入的 GLSL 精简优化改变了源码，标题和共享清单的旧库命中实测见下文。
`exec.c` 现在默认等待绘制和计算所需的程序、管线完成，再提交对应操作；失败明确报告。
仅诊断开关 `LARECOMP_ASYNC_SKIP_DRAWS=1` 保留之前的跳过策略。仍不能把代码路径和独立检查当作全游戏画面或帧率证明。

## 已引入的上游实现

`third_party/macnativegames` 保留了 MIT 许可及固定版本的原版 `shader_list.py`、`prepare_shader_seed.py`。
清单压缩后约 4.72 MB，仅含标识与状态。`UPSTREAM.json` 记录来源提交和未压缩 SHA-256，导入器会校验。
使用其原有解析、去重、管线无关状态归一化、标题/其余顺序、纹理通配符处理和默认/已观察状态规划。
`third_party/d2r-digest` 复用 D2R Mobile 的便携 SHA-256 实现，用于原生准备和元数据审计。
这些 Python 工具运行于 dev，手机不需要安装 Python。

`tools/audit_shader_raw_keys.cpp` 用正式原生目录读取游戏，再按上游身份计算
`SHA256(声明长度的控制段 + 完整 NVN 代码)`。计算着色器交给翻译器的是从入口开始的代码；
读取器另存入口前的原始字节以便身份匹配，翻译代码和常量相对偏移保持原样。

`tools/import_shader_warm_list.py` 只保留当前游戏存在且阶段正确的键；完整保留顶点/片元配对、输入输出类型、
图像依赖、边框色和 tile-read 描述，不把 Metal 管线数据当成 Vulkan 管线缓存。
尤其不能直接沿用上游忽略 `attr`/`fout` 的 `deps_match`：Metal 会在创建管线时重新声明类型，Vulkan 必须适配。

## 当前 1.4.3 的匹配结果

固定上游版本 `f024ef04c707c4b4bb5b6328bfc55ae89642af40`，使用本机原有 RomFS：

| 项目 | 数量 |
| --- | ---: |
| 原生目录输入 / 原始 SHA-256 键 | 28,634 / 28,634 |
| 上游变体行 / 匹配当前游戏 | 13,802 / 136 |
| 上游预热行 / 匹配当前游戏 | 70,971 / 38,390 |
| 匹配的 S / C / R / B / T 行 | 432 / 35 / 35,949 / 1,202 / 772 |
| 当前游戏标题预热行 / 涉及原始键 | 546 / 279 |
| 上游 seed 请求：默认 + 额外状态 | 28,634 + 136 |

最后一行只是上游默认 seed 规划，**不是最终 Vulkan 变体数、已生成 SPIR-V 数或准备耗时**。
真实类型和阶段配对现已接入原生模块规划，验收见下一节。共享清单含多个游戏版本；其他版本的键不用于当前游戏预热。
未匹配清单键也不直接等于游戏文件遗漏。

8 个原有 AGL/SDK 档案与原版上游扫描器交叉核对：3,640 条有效输入全部匹配；
另外 74 个上游扫描候选的控制头魔数均为 `0x00000000`，不符合 NVN 的 `0x98761234`。
这些来自启发式扫描，不能据此给生产目录补入伪输入。该检查只覆盖这 8 个档案，尚未解释整个 137 条统计差异。

用 `tools/audit_shader_upstream_inputs.py <原生键 TSV> <本地 BNSH/SHARCB 目录> <结果 JSON>` 可复核该差异；
有效但未匹配的输入会使审计失败，非法候选单独报告。

证据：`.work/shader-balanced-20261029/imported/summary.json`、`upstream-identity-summary.json`、
`upstream-identity-check.json`。导出的清单与 seed 请求只有元数据，明确标记尚未准备 SPIR-V/管线。

## 复现

以下命令在 `distrobox enter dev` 内、项目根目录执行：

```bash
mkdir -p .work/shader-balanced
clang -O2 -Ithird_party/d2r-digest/include -c third_party/d2r-digest/digest.c -o .work/shader-balanced/digest.o
clang++ -std=c++17 -O2 tools/audit_shader_raw_keys.cpp .work/shader-balanced/digest.o -lzstd -o .work/shader-balanced/audit-raw-keys
.work/shader-balanced/audit-raw-keys .work/totk-port/game/romfs.bin > .work/shader-balanced/raw-keys.tsv
python tools/import_shader_warm_list.py .work/shader-balanced/raw-keys.tsv .work/shader-balanced/imported
bash tools/test_shader_warm_list.sh .work/shader-balanced/tests .work/totk-port/game/romfs.bin
bash tools/test_shader_runtime.sh .work/shader-balanced/runtime
```

测试覆盖选定游戏过滤、阶段错误、未知管线布局、标题顺序、压缩管线键去重、完整类型和特殊状态保留，
以及便携 C SHA-256 与 Python hashlib 的分块边界对照。原生读取器由 ASan/UBSan 验证合成边界和完整真实语料。

## 标题 SPIR-V 准备已接入（2026-10-29）

`build_shader_warm_manifest.py` 在 APK 构建时读取固定上游清单，合并只影响 Metal 管线而不改变本项目模块源码的重复项，
保留顶点/片元原始键、NVN 顶点数值类型、按 merged/lmap 还原的逻辑颜色输出类型和标题优先级。
全版本紧凑清单为 39,405 个请求、4,570,996 字节；当前游戏标题匹配 446 个请求，未发生阶段冲突。

`shader_warm_plan.hpp` 用真实配对的 VS 输出掩码生成 FS；单独 FS 使用原生目录里的已声明配对。
输入输出类型不做三种类型的排列组合。上游管线没有记录本后端的 origin/depth mode，VS 仅补四种小型 clip 变体；
FS 补两种可能影响 Qualcomm early tests 的状态，源码相同的请求由现有队列去重。
图像依赖目前在固定上游清单里为空，后续出现无法表示的依赖会明确报告，不猜类型。

`vk_shader_warm_game` 使用原有 `ShaderPrepareQueue` 和正式编译插件，默认可用全部核心。
它与旧完整准备共享互斥锁，与游戏翻译共用翻译器锁；后台过程在每次翻译、缓存快速校验和最终写收据前核对配置。
成功后记录 `cache/shader-warm/<游戏路径摘要>/title.receipt`，内容绑定构建、准备配方、翻译配置、RomFS 文件身份和清单 SHA-256，以及本次所有源码键。
再次准备时逐份验证完整缓存；命中后不扫描 RomFS、不运行翻译器、不打开编译器。缺失/损坏或身份变化会重新走准备。
这个记录不会生成 `.shaders-ready`，也不表示游戏所有可能状态已准备。

实测标题范围如下，均使用独立测试目录：

| 项目 | 结果 |
| --- | ---: |
| 源码请求 / 去重后的 SPIR-V | 875 / 734 |
| VS / FS / CS 源码请求 | 580 / 276 / 19 |
| dev 首次准备（ASan/UBSan） | 15.998 秒 |
| PJZ110 首次准备 | 6.280 秒 |
| PJZ110 校验全部缓存后的复用 | 0.180 秒，未打开编译器 |
| 缓存文件内容 / 文件分配空间 | 7,313,352 / 8,884,224 字节 |

两端均在准备结束后关闭编译入口，重新遍历 875 个请求，通过正式严格加载器读取缓存，并检查 SPIR-V 的数值接口类型及 FS 输入掩码。
另有收据身份变化、缓存缺失、非法键/计数、截断、尾随数据和原子替换测试；最新 APK 构建与内置清单字节核对通过。
手机测试是 CPU 侧原生准备/加载/接口检查，没有替换游戏 APK，也没有执行这些管线的 GPU 绘制；这些数字不代表游戏帧率。

```bash
# dev：包含真实标题准备、关闭编译器后的严格加载和接口检查
bash tools/test_shader_warm_prepare.sh .work/shader-warm-check .work/totk-port/game/romfs.bin
# dev：交叉构建同一测试，产物和 APK 中的编译组件供独立手机目录使用
bash tools/build_shader_phone_warm.sh .work/shader-phone-warm
```

最终证据在 `.work/shader-balanced-20261029/` 的 `receipt-test.log`、`phone-cold.log`、`phone-title-cold.log`、
`phone-receipt.log`、`phone-title-receipt.log`、`android-warm-receipt-build.log`。

## 正常启动、后台生命周期和最近使用记录

启动器在主页面准备标题内容。提前按 PLAY 会等待标题任务就绪再进入游戏；打开设置/文件设置会取消并回收任务，
配置修改完成返回主页面后重新检查。直接启动也走同一路径。标题完成后，剩余任务以 Linux/Android 线程 nice=10 运行，
仍可使用全部逻辑核心，给正常优先级的游戏线程先运行。游戏重启前先回收准备线程；进入后台时取消、刷新记录，
回到前台按低优先级续跑。配置变化会取消当前准备，后台最多按新配置重试两次，已完成缓存保留。

`shader_warm_job.hpp/.cpp` 管理任务状态和线程生命周期，重复帧调用启动是幂等的。
取消检查覆盖文件读取、翻译和编译队列边界；已进入 shaderc 的单次编译完成后才回收该线程。
取消和失败不产生后台完成收据，已完成的标题收据可继续使用。`LARECOMP_SHADER_WARM=0` 可关闭自动预热，供对照测量。

绘制/计算请求首次使用的程序状态写入 `cache/shader-warm/<游戏路径摘要>/observed.tsv`：原始 SHA-256 标识、阶段配对、
顶点/颜色/图像数值类型及镜像状态，没有游戏代码。每个程序状态在本轮记录一次，32 条一批刷新，切后台和重启时也刷新。
下次后台准备先处理最近记录，再处理原生计算着色器的默认状态和共享清单。记录保留最近 8192 个不同状态；截断或损坏行不影响此前有效记录。
历史记录裁剪不删除已编译缓存。后台收据还绑定本次最近记录的快照，避免把新记录误认为已准备。

准备日志、收据和最近记录都放在应用可写的缓存目录：`title.log`、`common.log`、`title.receipt`、
`common.receipt` 和 `observed.tsv`。游戏路径摘要取规范化 `romfs.bin` 绝对路径的 SHA-256；同一输入的路径别名
共用记录，不同游戏目录相互隔离。新观察文件不存在时读取旧 `.shader-warm-observed.tsv` 并写入新位置，保留旧文件。
输入目录可以只读，不需要改变游戏文件权限。日志写入失败仍回退到 stderr；只有完整准备成功才保存相应收据。

完整共享清单的当前 1.4.3 规划为 15,160 个匹配请求、61,079 个源码请求，去重后 48,006 份不同源码。
其中源码请求为 44,832 VS / 16,196 FS / 51 CS；无未配对片元。此为只读规划结果，尚未完成全部 SPIR-V 准备。
它只覆盖已收集状态，与旧 2,798,003 候选的全面枚举范围不同；游戏仍允许缺失时编译。

```bash
# dev：生命周期、最近状态、队列关闭、真实任务取消、配置变化和严格缓存加载
bash tools/test_shader_warm_background.sh .work/shader-background-check .work/totk-port/game/romfs.bin
```

以上主机 ASan/UBSan 检查通过；另在 PJZ110 独立 native 测试中取消实际后台任务，确认标题缓存保留、
未写未完成后台收据，875 个标题请求继续严格加载通过。最终 APK 构建通过，尚未安装进行完整游戏启动/画面验收。
证据：`background-final-tests.log`、`background-verified.log`、`phone-background.log`、`background-plan.json`、
`android-background-verified-build.log`，均在 `.work/shader-balanced-20261029/`。

## 缓存身份与旧库实测（2026-10-29）

Android 的存储目录改为独立的 `SHADER_CACHE_ID`，由源码键算法、编译器、精度处理、接口特殊化和 shaderc 二进制
这六项输入计算。渲染器、扫描器或准备调度变化仍更新 `SHADER_BUILD_ID` 和收据，已生成模块的存储目录保持稳定。
源码本身、阶段、Vulkan 目标版本和优化开关仍属于每个文件的内容键；不能把新的源码请求指向旧的不等价结果。

加载依次检查当前编译器目录、当前构建目录，以及 `shader_cache_compat.json` 明确审核的旧构建。
只有六项输入全部与审核哈希相同，才启用旧 `c6b57ee…` / `a322f2e…` 两个目录；不任意扫描磁盘目录。
旧、新组件差异是验证错误的名称格式，主机 296 份真实计算源码以及实际捕获的 AGL 顶点/片元源码已作输出对照；
手机也通过 16 份合成直接模块、24 份合成派生模块、5 份真实直接模块及 4 份真实派生模块的逐字节对照。
这比较的是**同一输入经过两个编译组件**的结果，不表示旧、新翻译器生成的源码相同。

字典模块从其所在目录读取基础模块并验证摘要。准备时保留同目录的内存字典优化；复用旧基础模块生成新变体时，
先将所需基础模块存到当前目录，再原子保存新变体。旧完整命中不复制文件、不改写旧缓存。
`vk_shader_cache_stats()` 是本进程累计查询次数，不是唯一文件数，不包含内部递归读取字典的次数。

PJZ110 的新加载器关闭编译器后，逐份读取旧完整缓存：

| 检查 | 结果 |
| --- | ---: |
| 完整模块 / 基础模块 | 690,127 / 28,377 |
| 合计旧目录命中 | 718,504 |
| 缺失或损坏 | 0 |
| 8 线程只读审计 | 258.171 秒 |

随后把两批旧目录以目录符号链接接入独立测试根，不复制缓存，不修改 ready。按当前原生测试配置
（FP32、Qualcomm 控制流、gather=8、copy/miss 路径）生成真实请求，标题 734 个和完整共享清单 48,006 个源码键
均为 **0 个旧库命中**。完整清单仅做翻译和查找，耗时 574.440 秒，未编译这 48,006 个模块。

已定位一个普遍差异：此前上游 `9f94aba` 整合精简了 GLSL prelude，并用内建转换替代 `as_f/as_u` 调用。
同一输入、同一配置的 14 份标题样本用旧 APK 的真实翻译器对照，源码全部不同；同一编译器处理这些样本后，SPIR-V 字节也全部不同。
因此本次不为它们伪造源码键别名，也不撤回已合入的优化。旧库保留，后续未改变源码的模块可按准确内容键复用；
当前受影响模块走标题/后台小清单准备。独立存储身份可避免今后的无关渲染器修改再次使这些新缓存换目录。

新稳定目录的手机标题准备已完成：734 个完整模块，7,313,352 字节内容，8,884,224 字节文件分配空间，0 个未完成文件。
875 个请求在关闭编译器后严格加载，数值接口和阶段配对检查通过；旧目录保留，没有创建 `.shaders-ready`。

兼容测试覆盖跨目录字典隔离、新变体依赖保存及写入失败、损坏回退、未经审核目录拒绝、当前构建回退，
以及六项输入变化、非法 schema/目录名和缺失审核项。字典和运行时加载的 ASan/UBSan 回归通过。
Android APK 与独立手机工具使用同一生成头，已核对 APK 内嵌的实际身份。该 APK 尚未由本任务安装。

另根据并行实机记录的 `cannot write shader warm log` 修正准备入口：日志文件不可写时输出到 stderr，
仍检查输入、缓存和收据；不再仅因诊断日志失败而停止准备。主机和手机原生回归覆盖日志路径被目录占用和编译器关闭后的缓存复用。
这仍需在原来失败的完整应用启动路径复验。

```bash
# 在 dev 中；第二个参数可选旧编译插件，其后可追加 GLSL 样本
bash tools/test_shader_cache_compat.sh .work/shader-cache-check
# 原生手机工具同时生成 audit-cache / audit-warm-cache / test-compiler
bash tools/build_shader_phone_warm.sh .work/shader-phone-check
```

证据在 `.work/shader-cache-compat-20261029/`：`final-tests.log`、`loader-regressions.log`、`initial-tests.log`、
`real-graphics-compiler.log`、`phone-compiler.log`、`phone-legacy-audit.log`、`phone-title-coverage.jsonl`、
`phone-common-coverage.jsonl`、`translation-comparison.json`、`translation-spirv-comparison.json`、
`warm-log-test.log`、`phone-title-prepare.log`、`phone-title-measurement.json`、`android-final-build.log` 和 `verified-state.json`。
私有 GLSL 样本不提交。

## 实际 Vulkan 管线预热与首次等待（2026-10-29）

`pipeline_state.c/.h` 从原 `exec.c` 提取了实际管线创建函数。正常绘制和预热共用图形状态、动态状态、
传统 render pass / dynamic rendering、混合、顶点格式以及计算工作组特殊化的同一实现。
共享清单继续用于 SPIR-V 准备；没有直接重放 Metal 管线字节，也没有猜测 Vulkan 状态组合。

成功创建的实际图形/计算管线记录到 `cache/vk_pipeline_recipes.bin`：SPIR-V 内容键、Vulkan 固定状态和计算工作组尺寸，
不含游戏代码、地址或 Vulkan 句柄。记录最多 8192 项；新状态追加，重复命中不搬动整个列表。
字段明确复制并清零 padding，文件绑定构建、编译器缓存身份、设备/驱动/UUID、动态状态和 UBO 约定。
不匹配、截断、尾随字节或摘要错误使文件失效；原子保存失败保留此前完整文件。

下次 Android 启动在标题准备之后，从最近新增的记录开始，用低优先级后台线程读取已有 SPIR-V 并创建管线。
线程数按可用 CPU 核心数选择，上限 64，手机实际为 8 核；不为缺失 SPIR-V 的记录启动 shaderc。
正常请求若命中已完成结果，直接取得管线；若对应任务还在排队，则交给正常创建路径，避免等在整张列表后面；
已在创建中的对应任务可以等待完成。进入后台或重启会取消并回收预热线程，保留已交给渲染器的结果。
当前管线预热在启动时调度；从后台返回后已有管线继续使用，SPIR-V 后台准备按原有生命周期恢复。

预热产物连同创建时的 pipeline/descriptor layouts 一起移交，使用期间保持布局存活，不要求 maintenance4。
独立 GPU 验证最初发现提前释放计算布局的 VUID，修正后重新通过，不能使用那轮带错误的结果。
管线记录随 driver cache 保存，并每 30 秒独立刷新，避免 driver cache 大小不再增长时丢失新增记录。

普通绘制和计算默认等待自己的程序/管线结果，完成后继续执行；不会仅因异步构建尚未完成而丢掉该操作。
等待使用条件变量，不再每 200 微秒轮询。线程创建或任务分配失败时同步执行，防止结果永久停留在“构建中”。
重启在停止渲染提交后，先排空翻译队列，再排空可能由翻译任务新增的编译队列，然后替换游戏内存。
`LARECOMP_PIPELINE_WARM=0` 关闭驱动管线预热，`LARECOMP_SHADER_WARM=0` 同时关闭自动 SPIR-V/管线预热；记录仍可保留。

已完成的独立验收：

| 范围 | 证据 |
| --- | --- |
| 实际任务队列 | 多线程首次使用、单项等待、失败唤醒、翻译到编译交接、重启排空、同步诊断；ASan/UBSan 通过 |
| 管线记录/生命周期 | 去重、设备身份、损坏/截断边界、写失败、缓存缺失/创建失败、产物所有权、排队转正常创建、取消与等待竞争；ASan/UBSan 通过 |
| RADV 780M | 跨进程读取记录、预热后实际绘制/计算，352 个数值检查；传统/动态渲染、EDS 开关、2×3×4 工作组、兼容布局绑定；验证层 0 消息 |
| Adreno 830 | 同样 352 个实际 GPU 数值检查通过；没有安装验证层 |
| Android | 当前 Debug APK 构建通过，尚未由本任务安装进行完整游戏验收 |

```bash
# 均在 dev 中运行
bash tools/test_pipeline_warm.sh .work/pipeline-warm-check
bash tools/test_vk_pipeline_warm.sh .work/pipeline-warm-gpu
# 交叉构建手机独立测试；输出程序分 --record / --render 两次运行
bash tools/test_vk_pipeline_warm.sh .work/pipeline-warm-phone --android
```

最新证据在 `.work/shader-pipelines-20261029/`：`tests.log`、`gpu-compute.log`、`phone-gpu.log`、
`cache-tests.log`、`android-final-build.log`。这些不是游戏路线或 FPS 验收，不能据此宣称卡顿已经解决。

## 完整应用启动、记录保存与固定场景实测（2026-10-29）

实测旧应用虽然能把准备日志回退到 stderr，但 `extracted/` 由 shell 拥有，收据和观察记录仍无法保存。
本次将这些元数据迁入上述应用缓存目录。主机 ASan/UBSan 测试覆盖只读游戏目录、收据复用、路径别名、
不同游戏隔离、旧观察记录迁移和原文件不变；真实 875 个标题请求继续严格加载并核对接口。

在 PJZ110 / Adreno 830 上安装隔离的 Release / snapdragon-8elite 测试包，APK SHA-256 为
`34de7f0ce716bf5501ac520d493c799e0913039048beb2098db544a854fd01ab`。
安装前保存原 APK，六轮均核对相同包、O3 游戏库和设置摘要，使用独立设置/存档副本、60 FPS 目标和 15 秒采样。
测试包以外的应用和存档未被这些测试操作；原设置 SHA-256 在结束后仍与备份一致。
当前发布编译插件与此前 Debug 插件也在手机逐字节比较，通过 21 个直接和 28 个派生模块检查。

| 运行 | 自动 SPIR-V / 管线预热 | FPS | p95 帧时间 |
| --- | --- | ---: | ---: |
| 首次保存新收据和管线记录 | 开 / 开 | 35.20 | 33.30 ms |
| 同包再次启动 | 开 / 开 | 36.84 | 34.48 ms |
| 关闭预热，起始温度较高 | 关 / 关 | 28.59 | 45.74 ms |
| 反向复测 | 开 / 开 | 36.12 | 34.40 ms |
| 冷却后关闭预热 | 关 / 关 | 34.06 | 35.91 ms |
| 冷却后仅后台 SPIR-V | 开 / 关 | 36.45 | 35.03 ms |

最后两轮均等待 skin ≤46°C、CPU 最高 ≤56°C、GPU 最高 ≤50°C、HAL 状态为 0 后启动。
较热关闭轮不能与再次启动轮直接推算收益。短测未见后台准备明显拖慢该场景；尚不能由这些差值证明长期或跨区域收益。

实际功能证据：

- 首次新收据准备 4.162 秒；随后 734 个模块在 0.306 / 0.336 秒内校验复用，未打开编译器。
- 再次启动预热 1,051 条管线、实际复用 1,046 条；反向轮预热 1,053 条、实际复用 1,052 条，缺失和失败均为 0。
- 六轮均自动进入固定存档场景并由运行时定时退出，无 `receipt_not_saved`、权限错误、FATAL 或 FORTIFY。
  周期渲染统计均为 0 skipped；没有 `[shader-runtime] compiling` 消息。
- 最终记录文件有 1,055 条管线（1,032 图形 / 23 计算），大小、摘要和稳定编译器身份核对通过。
  标题收据包含 734 个键；最近状态文件成功保存。后台被定时退出取消，没有错误写出 `common.receipt`。

这些测试使用已有 SPIR-V 和驱动缓存，**不属于清空缓存后的对照**。计时沿用现有连续 90 帧渲染条件的就绪判断；
没有进行截图或路线画面验收。全启动期间周期日志仍累计报告约 2.1–3.1 秒首次使用等待，覆盖翻译、缓存加载、
反射和管线工作，不能把它等同于 shaderc 编译时间，也不能宣称启动卡顿已消除。
接下来应围绕跨区域首次遇到状态及这些剩余等待分析，保持均衡方案，不恢复旧百万级穷举任务。

完整证据：`.work/shader-gameplay-20261029/verified-state.json`、六个运行目录、`phone-metadata/`、
`state-tests.log`、`benchmark-cli-tests.log`、`build-gameplay.log` 和 `phone-gameplay-compiler.log`。
测量使用保存的 `app-gameplay-release.apk`；其他工作流仍在修改工作树，不能将后续构建与这些结果混为一谈。
本轮收尾时另一个工作流已更新测试包，当前安装状态单独记录在 `final-device-state.json`，未将它回滚为本轮被测包。

## 单项准备续用与清单外计算状态（2026-10-29）

一次30秒加载采样得到32,645个CPU样本、0丢失；后台 `shader_warm::prepare` 占约35.90%的采样周期，
其中大量时间用于翻译和源码字符串格式化。原有完整收据仅在整张清单完成后保存，短次运行取消后会重新翻译前面的请求，
即使对应SPIR-V已经存在。该采样包含加载和早期场景，不能解释为全部游玩时间的占比。

参考已引入上游 `prepare_shader_seed.py` 的 `TranslationCache`，原生准备增加单项请求映射：
`cache/shader-warm/<游戏路径摘要>/requests/<身份摘要>.bin`。记录原始着色器摘要与完整 `ShaderKey` 对应的
准确SPIR-V源码键、阶段和顶点输出掩码；不保存第二份GLSL。身份绑定构建、准备实现和实际编译配置，
清单内容或最近状态增加不会使已有单项记录失效。使用模块前仍检查SPIR-V，缺失/损坏时走真实翻译和准备。
单项映射可先于对应异步编译完成落盘，因此它本身不代表SPIR-V已经准备成功。

记录采用明确字节顺序和逐条SHA-256校验。中断或损坏尾部保留此前有效前缀，并从正确边界续写；单文件最多
131,072个不同请求、262,144条记录。元数据不可写时本次准备仍可继续。缓存全部命中时，空准备队列不再打开编译组件。
配置身份还补入实际Vulkan API版本和 `LARECOMP_NO_BANK2`、`LARECOMP_NO_DISCARD`、`LARECOMP_SAVE_MSL`。
后者仍会改变源码键，但不会再误用另一诊断配置的请求映射；SPIR-V稳定命名空间保持不变。

主机ASan/UBSan检查已验证：删掉标题整份收据后，875个请求均从单项映射复用，翻译次数为0，编译库不可用时仍通过；
移走一份SPIR-V后，确实重新尝试翻译和准备。取消后已有请求可复用，配置变化安全停止，真实875个标题请求的接口检查继续通过。
损坏/截断恢复、输入和全部类型状态身份、历史上限、不可写元数据也已测试。

手机上的单项记录版本 `0ec972b1…` 连续两轮分别处理了12,975次翻译，以及复用12,351个请求后继续处理9,309次翻译。
第二轮预热1,047条管线、实际复用1,044条，缺失/失败为0。这证明短次启动能继续推进列表；后台仍在处理新的条目，
不能据此宣称后台CPU工作已全部消除。

随后原始 `slot_01` 场景发现两份缺失计算源码键 `a9ac9e8b91531b46`、`9fc66a9faea2df85`，
并记录约3秒首次使用停顿。新增的三个计算原始键均在已验收原生目录中，却不在固定上游共享清单里。
该轮被外部force-stop中断，没有完成计时，不是通过或本地崩溃的证据。

为覆盖这类已知原生输入，沿用上游默认seed的做法，在后台加入本版本全部 **257个计算输入的默认状态**：
最近记录优先，其次默认计算状态，然后共享清单。不展开图像类型的排列组合，不增加标题门禁范围。
无最近记录时选中请求为15,417条（原共享清单15,160加257个默认计算请求，后续仍按状态/源码去重）。
主机真实准备、取消续用和严格缓存回归通过。

最终此阶段APK为 `c991638eb44ea93b939b50fa31f78be26faaf950b550f4aa38146ee73395cf06`，
已在 `slot_01` 完成一次15秒测试：33.03 FPS、p95 39.02 ms，运行时编译消息为0，记录1,458条管线，正常自动退出。
这是已有缓存下的第二个存档场景检查；不能将其当作257个默认输入在全冷缓存下均赶在首次使用前完成的证明。

证据 `.work/shader-firstuse-20261029/`：`profile-on/`、`requests-profile/`、`requests-first/`、
`slot-1-first/`（中断）、`compute-slot-1/`、`request-tests.log`、`compute-prepare-tests.log`、`cli-tests.log`、
`build-compute.log`、`compute-state.json`。初版测试脚本遗漏SHA对象的链接已修正，`compute-tests.log`中的该错误由后续
`compute-prepare-tests.log`的通过结果覆盖。原始存档只读检查并克隆使用，未为测试修改位置。

## 接续工作

两个原始存档场景的完整应用启动和固定场景计时已验收；继续复验第二场景的管线复用、默认计算seed的全冷准备代价，
以及跨区域首次遇到状态的等待与画面。
已有固定场景短测不能代替路线验收。保持普通 APK、配置、存档及已有缓存，继续使用隔离测试根。
不能把旧完整 ready 复制为新构建的证明，也不能直接重放 Metal 管线的字节布局。
在游戏中测量首次/再次进入相同场景的编译次数、命中率、等待和帧时间，检查画面后再评价方案收益。

旧 2,798,003 项任务已于 05:50 停止，最后显示加权进度 63.625%，没有完成；产物和旧缓存保留。
旧版完整准备、源字典压缩、图像与数值测试证据见 [历史完整准备路径](SHADER-PREPARATION.md)，不能作为当前均衡方案已完成的证明。
