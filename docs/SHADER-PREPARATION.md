# 手机端 SPIR-V 完整准备路径与历史验收

**2026-10-29：用户已选择[均衡准备方案](SHADER-BALANCED.md)，不再要求所有候选提前展开。**
当前默认缓存优先、缺失可编译；只有关闭运行时编译才使用本文的完整准备门禁。
下文保留旧完整路径及实测证据，数百万候选不再是默认启动要求。05:50 已停止旧扩展任务，产物保留。

后续[链路审查](SHADER-CHAIN-REVIEW.md)已确认：本路径的 `vk_shader_game_ready` 只核对完成标记身份，
缓存目录已被删除时也可能返回 true；它不等于逐项完整性检查。旧日志/标记仍写入游戏目录，
只读输入下存在准备失败。以下历史“门禁通过”不能扩大为缓存可用或全游戏完全静态运行的证明。

此路径仍在开发，**尚未达到全量游戏可用**。2026-10-28 实测及后续容器审计发现，原先 8,697 份输入仅来自 `Shader/`，
遗漏了库、UI 和特效文件。当前原生读取共 **28,634** 份输入（14,760 顶点、13,617 片元、257 计算），
FP32 优先模式为 **28,634 翻译成功、0 失败**。多重采样、图像读写及随后暴露的整数图像原子加法均已接通。
文件提取与程序关联已通过真实语料及 ASan/UBSan 检查；扩展范围的完整 SPIR-V 库尚未准备完成。
根据原游戏粒子顶点状态初始化代码约束已确认的格式后，通用审计配置计划为 **2,666,099** 条目；
PJZ110 系统 Qualcomm 驱动的实际准备计划为 **2,798,003** 条目。两者不能混用统计口径。

此前原范围的 800,432 个候选条目已在 PJZ110 系统驱动下完成准备，生成 690,091 份唯一游戏 SPIR-V，
内容 1.02 GB、实际占盘 3.58 GB，文件全部通过只读加载检查。这些测量仍有效，但**不能代表游戏全部着色器已覆盖**。
启动器和直接启动入口在严格模式下保留准备门禁；默认模式的缺失编译和剩余工作见均衡方案。

## 首次游戏启动发现的遗漏（2026-10-28）

使用与原缓存构建身份一致的 APK、系统驱动、FP32 优先及独立数据根启动，完成标记检查通过。
首次实际绘制缺少顶点模块 `9d4a40786749a100`，游戏按设计停止。该文件在原缓存中不存在，非复制损坏。
保存诊断源码后去掉 `LARECOMP_SAVE_MSL` 添加的指令注释，与原始文件重新翻译结果逐字节对比成功：

`Lib/agl/agl_resource.Nin_NX_NVN.release.sarc.zs:agl_common.sharcb`

该顶点程序被 `texture`、`xlu_snap`、`image_filter_blur` 等多个程序组复用，扩展目录中编号为 11032。
因此本次缺失首先是提取范围错误，并非已经证实的动态生成着色器。

读取器现在按支持的文件类型发现整个 RomFS 中的 `.bnsh`、`.bfsha`、`.sharcb`、`.sarc`、`.blarc`（含 `.zs`），
递归检查有界 SARC 成员，保留原 `Shader/` 条目顺序，并在错误中报告具体成员路径。
新增读取覆盖 SDK BNSH 2.1.2 / v9 `0x7d1` 控制头、库程序附加控制数据，以及 UI BNSH 的程序配对。
现有 FSHA 仍为 23 个模型，命名/独立 BNSH 程序组从 66 增至 332；全部 12,574 份输入有程序关联。
这仍是已支持格式的扫描范围，不是对其他容器或运行时状态的完整证明。

新暴露的 29 个首失败为：18 份 `SULD` 图像读取、8 份 `SUST` 图像写入、3 份 `TLDS info 6` 多重采样读取。
其中多重采样读取先修复，该阶段曾为 **12,548 成功 / 26 失败**。随后图像指令及 Vulkan 资源绑定已补齐，验收见下文。
图像功能仍需实际游戏验收；当前允许缺失编译，不得用虚构结果替代尚未支持的指令语义。

### 多重采样读取验收（2026-10-28）

`TLDS info 6` 现在以 Ra/Ra+1 作为坐标、Rb 作为采样索引，生成 `sampler2DMS` 和 `texelFetch`。
纹理尺寸查询在纹理类型确定后生成；多重采样纹理只有一个 mip，不能调用普通纹理的 `textureQueryLevels`。
Vulkan 采样视图现在保留实际多重采样图像；未绑定时使用同类型的多重采样占位图，避免绑定普通单采样图像。

- 3 份真实片元程序均通过正式准备组件的 Vulkan 1.1 SPIR-V 编译与验证。
- 96 组合成程序覆盖先查询后读取、通道掩码、双目标寄存器、FP16 打包及 R254/RZ 边界，ASan/UBSan 与 SPIR-V 验证通过。
- RADV Radeon 780M 和手机 Adreno 830 均实际执行通过 128 个数值检查：4 个采样点、不同坐标/通道、2 倍坐标缩放和两种结果编码。
  宿主启用验证层，消息为 0；手机没有验证层，不能将其消息数作为验证层验收。
- 与修复前的翻译器比较，原先成功的 12,545 份程序在相同审计配置下源码逐字节一致；新增成功 3 份，无翻译回归。APK 构建通过。

运行 `distrobox enter dev -- bash tools/test_vk_multisample.sh` 可复现无游戏数据的合成和 GPU 检查。
这些检查验证指令及 Vulkan 多重采样行为，尚不等于正式游戏的视图绑定、完整缓存命中或画面验收。
私有证据在 `.work/handoff-20261028/`：`new-inputs.log`、`full-fp32.json`、`source-compatibility.log`、
`ms-gpu-raster.log`、`ms-gpu-phone-raster.log`、`android-ms-build.log`。

原先失败的 26 份程序包含 35 条原始 B32 图像读取（17 条 1D、18 条 2D）与 41 条 2D RGBA 格式化写入。
76 条指令的句柄均可追到 `c[2][0x120]`、`c[2][0x128]`、`c[2][0x130]`，即 NVN 图像绑定表的前三个槽。
当时后端仅保存 `CMD_BIND_IMAGES`，缺少存储图像描述符、格式匹配和写后资源状态维护。
这些路径现已连同准备阶段的图像格式变体和运行时管线选择实现，详见下方验收。

### 特效文件的第二次扩展（2026-10-28）

在 15,080 个 `.pack.zs` 和 1,592 个 `.esetb.byml.zs` 解压内容中检查已知着色器签名，
发现 1,578 个特效文件含有 2,282 个 BNSH。逐份验证 BNSH 结构后得到 42,909 个输入记录，
相对库/UI 扩展目录新增 **16,060** 份唯一代码/控制输入。其余 14 个特效文件没有内嵌 BNSH。
本次签名检查没有在解压后的 Pack 内容中发现同类签名，但这不证明任意进一步嵌套压缩的内容均已覆盖。

正式读取沿文件结构定位：`BYML v5 → 对齐的 PtclBin → VFXB → GRSN → GRSR/GRSC → BNSH`。
不依赖在任意字节中搜索魔数；越界、错误对齐、不支持的块及不完整 BNSH 会使准备失败。
旧目录的 12,574 条记录保持原顺序，再追加特效记录。全部 28,634 份输入都保留了阶段关联，
命名/独立 BNSH 程序组从 332 增至 2,614；这里的配对仍不能代替动态绘制状态覆盖证明。

首次纳入特效时，28,634 份输入的 FP32 翻译审计仍为相同的 26 个失败；新增特效输入没有增加翻译失败。
提取器与目录在 ASan/UBSan 下通过，flat inventory 与目录的代码及控制块逐字节匹配。
这轮检查尚未编译新增特效程序的全部 SPIR-V 或状态变体。
证据在 `.work/handoff-20261028/{containers.log,effects-inspection.log,effects-translation.json,effects-catalog/catalog.json,
effect-catalog-test.log,effect-archive-test.log}`。

### 图像资源与扩展计划验收（2026-10-28）

已实现 `SULD.D.B B32` 原始位读取、`SUST.B` RGBA float/sint/uint 写入，以及 32 位整数 `SUATOM.B`。
图像句柄沿 NVN `c[2][0x120+8*i]` 绑定槽解析，Vulkan 使用存储图像描述符、兼容的原始 R32 视图和 GPU 写入所有权。
已有采样图像按需增加存储用途，保留旧内容，并将缩放目标恢复到 guest 尺寸。未支持的句柄、维度或格式明确失败。
图像数字格式独立编译为基础模块；计算管线按实际使用的图像格式选择，不能套用顶点／片元接口改写。

- 全部 28,634 份输入翻译成功。与多重采样和图像修复前的对象比较，28,605 份原先成功程序源码完全一致，29 份新增成功，0 回归。
- 26 个真实图像程序的 296 个格式／内存路径／int64 配置组合通过正式编译器、SPIR-V 验证和存储图像反射。
- RADV 780M 与 Adreno 830 各通过 20,946 个数值检查，包括原始位读取、原子加法返回值与写后值、格式化写入，以及 0.5×／1×／2× 多 mip、多层图像升级。
  RADV 验证层零消息；手机无验证层。这些测试尚不能代替实际游戏资源绑定与画面验收。
- 原游戏 1.4.3 顶点状态初始化确认 13 个粒子系统属性及插件属性 0..5 使用 float4；BNSH 反射提供名称到 location 的关联。
  网格属性及未知同位置别名仍保留全部三种类型；共享的非特效未知用途仍保持开放。
- 压缩决策图保留布局集合、去重和顺序；准备器每批至多 2048 个接口变体。旧的 112 亿候选是加入特效格式约束前的中间结果。

通用审计配置包括 VS 1,902,016、FS 762,611、CS 1,472 条目，顶点类型组合 266,120；
14,154 份顶点程序有文件约束，606 份仍使用通用布局，其中 10,580 份有特效用途。
数量是候选条目，源码键还会去重，不能当作最终文件数或空间测量。
证据在 `.work/shader-images/{translation.json,real.log,gpu-final.log,gpu-phone-final.log,effect-plan-final/summary.json,build-identity.json}`。
最终 APK 构建通过；其准备插件及 shaderc 与旧 `a322...` 缓存所用二进制逐字节相同。
旧内容只在独立目录中复用，仍由正式加载器验证，准备完成标记必须由完整准备流程重新生成。

### 扩展库手机准备与补充审计（2026-10-28 晚）

已在 PJZ110 系统 Qualcomm 驱动上启动扩展库准备，直接读取手机原 RomFS，默认 FP32，使用 8 个准备工作线程。
实际预检为 28,634 份输入、0 翻译失败；计划为 **2,798,003** 条目：
VS 1,902,016、FS 894,515、CS 1,472。主机按相同 Qualcomm 翻译配置复现了该数量。
通用审计与系统驱动预检的差异集中在片元状态，直接内存路径的写入元数据仍有冗余枚举待修正；
当前准备保留这些候选，由源码键去重。尚未得到本次完成标记、最终空间或游戏验收结果。

旧缓存 718,504 个文件通过逐文件符号链接复用；正式只读加载器已检查 690,127 个完整模块（含 36 个内置模块）
和 28,377 个基础模块，0 失败，编译器未打开。旧内容为 1,020,844,285 字节，新增目录分配为 66,358,272 字节。
测量工具现将复用内容计入总文件／内容，并分别报告新文件分配、复用目标分配和链接／目录开销，避免漏算或重复计算新增占盘。
本次没有覆盖可玩 APK、存档或原配置，原缓存仍在原目录。

补充 Pack 审计按结构检查了 15,080 个 `.pack` 容器、15,081 个 SARC、336,624 个成员，
展开已知 Zstd 帧并检查声明类型和完整着色器签名；最大成员嵌套深度为 2，0 着色器命中，ASan/UBSan 通过。
该结果补足了先前未递归检查 Pack 成员的缺口，仍不等于证明任意未知格式或运行时动态状态均已覆盖。

分批集成测试也已通过：2,187 个顶点接口变体跨越 2,048 批边界，另有 296 个独立计算图像程序，
共 2,483 份经正式准备、关闭编译器后的加载及独立编译比较。
最新证据在 `.work/shader-images/{cache-reuse-audit.log,cache-reuse-measurement.json,batch-test.log,pack-members-audit.log,qualcomm-plan/summary.json}`；
运行位置及继续观察方式见同目录 `PROGRESS.md`，不要仅凭历史 PID 判断任务仍在运行。

私有证据在 `.work/session-handoff/`：`gameplay-first.log`、`first-miss-match.log`、`expanded-translation.json`、
`expanded-catalog/catalog.json`、`archive-test.log`、`catalog-test.log`。手机已恢复 20:48:29 的原 APK；
原存档和配置未用于测试写入，失败测试根和原缓存均保留。前述 3.58 GB 不包含接手时复制的独立测试缓存。

## 原 Shader 目录的手机整库测量（2026-10-27）

测试直接读取手机已有的 1.4.3 RomFS，使用 APK 内的正式准备组件、Adreno 830 系统驱动和 FP32 优先模式。
测试输出留在独立目录，未在这轮测量中替换应用。完整结果如下：

| 项目 | 结果 |
| --- | ---: |
| 原始输入 / 翻译失败 | 8,697 / 0 |
| 候选准备条目 | 800,432 |
| 唯一游戏 SPIR-V | 690,091 |
| 内置 SPIR-V / 共享基础模块 | 36 / 28,377 |
| 缓存文件总数 / 未完成文件 | 718,504 / 0 |
| 文件内容字节数 | 1,020,844,285（1.02 GB） |
| 文件及目录实际占盘 | 3,577,945,088（3.58 GB，3.33 GiB） |
| 八线程续跑进程耗时 | 12,982.409 秒（约 3 小时 36 分） |
| 该进程峰值常驻内存 | 418,440 KiB（约 409 MiB） |

此前先以四线程准备，期间曾暂停降温，随后按用户要求取消额外的两核限制与温度挂起。
八线程进程保留并校验复用旧缓存，因此上述 3 小时 36 分既不是全新冷缓存耗时，也不包含此前进程的时间。
Android 系统温控保持默认；今后的准备仍按逻辑 CPU 核心数创建工作线程。

800,432 比通用配置多出的 105,660 条来自直接内存路径预检未报告全局写入用途，产生了额外状态枚举；
完整源码键去重已去除重复输出。这不是原始游戏程序数量增加，也不是所有候选状态都已证明实际需要。
文件内容与占盘差额主要来自几十万个小文件的块分配开销，后续需要打包存储并进一步约束有证据的状态组合。

`tools/audit_shader_cache.c` 随后用正式游戏加载代码读取全部 718,504 个文件，8 线程用时 118.419 秒，
失败数为 0，未打开编译器；完整游戏及内置模块解压后累计 83,370,045,376 字节（逐项释放，并非峰值内存）。
该检查验证解压、依赖、校验和与模块边界，不等同于 GPU 管线执行、所有运行时状态命中或画面正确性验证。
准备完成标记也已写入。证据位于本地 `.work/totk-port/shader-phone-full/verified-result.json`、
`measurement.json`、`shader-prepare.log`、`cache-audit.log`，游戏数据与生成着色器不提交 Git。

本轮构建身份为 `a322f2e0d8de9128093af41f17d49f62ccfe6872ef0b82c68879f0ff146f22b8`，
准备配置摘要为 `71144b7ac5e9c74a`。此库属于高通系统驱动配置，不能把它的完成标记当作 Turnip 配置已准备的证明。

## 执行路径

游戏文件导入后的准备步骤直接读取手机上的 `extracted/romfs.bin`，解压 Zstandard 字典及
支持档案中的 FSHA/BNSH、SARC/SHARCB（包括库与 UI）。不会复制整份 RomFS、要求 Python/Zig，或要求电脑提供着色器包。
电脑仍用于构建 Android 应用本身及当前的 CPU 游戏库；这与 GPU 着色器准备是两件事。

- `shader_archive.hpp`：带边界检查的原生提取器；复核自本地 TotK-Native 的
  `tools/shader_archive.py`、`shader_library.py`。
- `shader_catalog.hpp`：保留程序选择、阶段配对和命名程序归属；手机准备流程已使用此目录，
  并记录关联覆盖率。`shader_material.hpp` 进一步读取模型/材质/顶点布局，详见[文件关联审计](SHADER-ASSET-LINKS.md)。
- `shader_variant_plan.hpp`：结合实际读取属性、模型格式、缺省输入和程序配对构建准备清单。
  无模型布局的程序及无法确定的属性保留通用类型组合，片元输出类型仍保守枚举。
- `shader_prepare.cpp`：全库预检、原生清单生成、进度和完整性门禁。
  预检中的任何失败都会阻止开始昂贵的变体编译。
- `libshaderprepare.so`：显式准备或已启用的游戏缺失编译通过 `dlopen` 加载，依赖已随 APK 提供的 shaderc。
  输出经过深度精度修正和同一 shaderc 发行版内的 SPIRV-Tools 验证，再原子保存。
  默认按逻辑 CPU 核心数创建有界工作线程，各自复用编译器实例。同一源码及状态的全浮点模块只编译一次，接口类型变体由
  `shader_prepare_derive` 派生并验证；等价完整源码键只提交一次。全部工作完成且实例关闭后才写完成标记。
  POSIX 上保留动态库映射，避免 shaderc 的进程级初始化锁在反复卸载时泄漏；编译器实例和调用入口仍在准备结束时关闭。
  严格加载接口没有编译回退；普通游戏加载入口遵从运行时编译设置。
- `shaders.c`：游戏阶段由 GLSL 内容、阶段、Vulkan 目标和优化策略确定准确的预编译条目，读取后创建模块。
  **仍有运行时 Maxwell → GLSL 翻译来确定内容及资源元数据**；缓存未命中且允许回退时才进行 GLSL → SPIR-V 编译。
  要进一步移除 Maxwell 翻译，需要另行保存并加载 `ShaderInfo` 与完整变体索引。

Vulkan 管线创建仍由驱动处理。预生成 SPIR-V 不等于预生成所有驱动机器码或消除所有管线卡顿。

## FP32 优先试验

Android 默认开启 **设置 → 启动前 → 着色器精度 → 优先使用 FP32（实验性）**。
对应应用配置 `shader_fp32 = 1`；关闭或设为 `0` 使用保留的 GPU 整数双精度模拟。
开关仅在启动前提供，准备期间禁用；更改后返回主界面会重新检查准备身份。
从“设置游戏文件”重新准备图形时会复用现有 RomFS 和游戏代码，不需要重新导入。
整库测量目前仅覆盖系统驱动下的 FP32 优先模式；切回整数双精度仍需完成该配置的准备与游戏验收。

开启时，舍入到最近偶数的 DADD/DMUL/DFMA 将操作数和每条指令的结果降为 FP32，
使用原生浮点加、乘及融合乘加。DADD/DMUL 保持禁止自动融合；有向舍入指令仍使用整数双精度。
寄存器对、常量及内存继续保留 binary64 编码，因此只降低算术精度，不改变其他指令的数据布局。
常见正常值的 FP32/FP64 格式转换使用直接位运算；特殊值和范围边界保留通用转换。

这是有损性能试验：会减少有效位数和指数范围，可能改变抵消误差、极小值及最终顶点位置。
PJZ110 的系统 Adreno 830 Vulkan 驱动未提供 `shaderFloat64`，但原生 FP32 和整数双精度模拟都在 GPU 执行。
尚未测量此改动的游戏帧率或验证画面；整数双精度算术本身也尚未完成 GPU 数值一致性验证。

## 存储和失效

完整条目位于数据根目录 `cache/shaders/<构建身份>/<源码键>.bin`；共享基础模块位于同目录的 `<基础源码键>.base.bin`。
v3 封装包含身份、字数、完整性校验、压缩长度、编码方式及字典的基础键/内容校验值。
基础模块使用独立 Zstandard 帧，类型变体使用基础 SPIR-V 作为原始字典压缩完整结果；内置着色器仍保存为独立帧。
加载时只做有界解压、SPIR-V 指令边界及内容校验，不运行编译器或接口转换器。
拒绝超出 16 MiB 输出上限、截断、尾随数据、字典缺失/不匹配及校验不符的条目；基础文件不能再依赖其他字典，因而没有递归依赖链。
这是带封装的条目，不能直接把整个 `.bin` 交给 `spirv-val`。不复用旧 `cache/spirv` 中无对应身份的缓存。
未完成的条目用 `.part` 原子保存；中断后的重新准备先复用或修复基础模块，再复用已经通过校验的完整变体，不重复编译或派生。
旧构建缓存目前不会自动删除，更新后可在确认不再需要时清理旧身份目录。

`extracted/.shaders-ready` 只在全部变体成功后写入，约束缓存版本、构建身份、设备/翻译配置和游戏文件身份。
标记身份现在以 `v3:` 开头；旧标记和 v2 缓存均不作为本版准备结果，需重新准备。
配置身份包含 FP32 开关；两种算术模式的源码不同，不会混用受影响着色器的缓存。无双精度指令的相同源码可复用。
`shader-prepare.log` 的 `double-arithmetic` 字段标明 `fp32-preferred` 或 `integer-binary64`。
`extracted/shader-prepare.log` 记录档案、失败条目、预计组合数及进度。缺失/损坏的条目在游戏阶段明确失败，
不会静默少画，也不会启动编译器。主菜单、清屏、SMAA、FSR、SGSR 共 36 个内置着色器在窗口初始化时准备。

## 实际发现的缺口

最初仅扫描 `Shader/` 时，手机（PJZ110）与 dev 的输入计数均为：3,675 个顶点、5,015 个片元、7 个计算。
参考项目的记录也为 8,697，但它的“全量离线编译”只覆盖指定状态，运行时仍存在未命中编译路径。
不能把那份统计作为本项目完整静态覆盖的证明。

首轮的 91 个失败中，现已补齐 LEA 普通形式、R2P.CC、TXD 显式导数采样及 TLD 完整形式，
其中 61 份真实程序全部通过正式准备组件的 Vulkan 1.1 SPIR-V 编译、深度精度处理和验证。
原先剩余 30 份首先停在 DMUL，程序内还使用 DADD、DFMA 和双精度格式转换；现已补齐，
两种模式下这 30 份真实程序均通过正式准备组件的 Vulkan 1.1 SPIR-V 编译和验证。首次失败及当前进展：

| 指令路径 | 首次失败数 | 当前进展 |
| --- | ---: | --- |
| DMUL（双精度乘法） | 30 | 已补齐 DADD/DMUL/DFMA/F2F；两模式 SPIR-V 验证通过 |
| TXD（显式导数采样） | 26 | 已修复 |
| LEA 的立即数形式 | 21 | 已修复 |
| R2P.CC | 12 | 已修复 |
| TLD 完整形式 | 2 | 已修复 |

这只是原 `Shader/` 范围中每个着色器的第一个失败分类，修复后该范围翻译失败归零；该范围的系统驱动 FP32 准备已完成。
扩展范围中的剩余失败和游戏验收进度以本文开头为准。
FP32 模式是显式精度取舍，不是等价双精度；两模式在 int32 配置下均不要求 SPIR-V Float64/Int64 能力。

原枚举器对 11 个属性逐个尝试三种类型，仅类型就有 `3^11 = 177147` 组。
新清单保留模型中声明的浮点/整数差异；整数布局另外保留未绑定时的浮点默认路径。
属性缺失或格式未识别时仍尝试三种类型。相互包含的布局合并、等价组合去重，无模型布局的 101 个顶点程序仍走通用组合。
Vulkan 绘制路径没有设置 `flip_x`，因此不再生成这类条目；没有全局内存访问的程序不重复枚举内存路径。
缺省属性的类型判断也与实际管线绑定统一，修复了无效流索引下“着色器为整数、默认缓冲却为浮点”的不一致。

| 清单项目 | 原枚举 | 当前 |
| --- | ---: | ---: |
| 顶点输入类型组合 | 9,579,589 | 29,791 |
| 全部准备条目（源码去重前） | 318,627,294 | 694,772 |
| 顶点 / 片元 / 计算条目 | — | 146,964 / 547,771 / 37 |

以上是通用测试配置的清单；系统驱动实测为 800,432 条。片元输出类型仍占主要数量，已完成准备，但耗时与占盘仍需优化。
不能把全部输出擅自改成浮点；均衡模式允许缺失编译，但必须使用真实绑定类型。

目前已完成[文件关联审计](SHADER-ASSET-LINKS.md)：全部 8,697 份代码都能追到所属程序，
54,541 份材质均匹配到文件中实际存在的候选程序。清单已使用这些接口和配对；动态构造的、超出文件声明的接口
仍可能未命中，因此此清单不是对所有运行时绘制状态的完整证明。未命中仍会明确失败，不自动编译或少画。

## 验证

所有主机构建和测试在 `distrobox enter dev` 中运行。

```bash
mkdir -p .work/totk-port/shader-preparation
clang -O2 -c runtime/nvn/maxwell/translate.c -o .work/totk-port/shader-preparation/translate.o
clang++ -std=c++17 -O2 tools/audit_shader_library.cpp \
  .work/totk-port/shader-preparation/translate.o -lzstd -ldl -o .work/totk-port/shader-preparation/audit
.work/totk-port/shader-preparation/audit .work/totk-port/game/romfs.bin \
  > .work/totk-port/shader-preparation/audit.json
```

发现不支持的着色器时审计退出码为 1。默认只检查翻译，输出明确标注 `translation_only` 和 `spirv_compiled: false`，
只用于覆盖率检查，不会生成可玩标记。

`tools/test_shader_archive.cpp` 检查畸形控制块、截断、阶段不符和解压拒绝，可选接收真实 RomFS 核对计数。
`tools/test_shader_preparation.c` 覆盖内置着色器、准备结束后关闭编译器、损坏条目拒绝和重新准备修复。
`tools/test_shader_depth_precision.c` 检查准备组件的顶点/片元深度精度及游戏阶段只读加载。
APK 构建、主机 ASan/UBSan、输出 `spirv-val` 检查和手机上的原生组件测试已通过。
手机组件测试覆盖 Vulkan 1.1/1.2/1.3 的全部内置名称及计算工作组特化；主机 RADV 的
10 组图形管线和 2 个计算管线通过，另有 12 个随机样本因原有翻译限制跳过，验证层消息为 0。
手机临时组件测试文件已清理，审计只读取原有 RomFS。
以上为早期组件验收记录；后续系统驱动配置的整库准备与加载检查见本文开头。正常游戏启动及画面验收不包含在这些检查中。

## 指令覆盖回归

`tools/test_shader_library_ops.c` 从正式发射器生成地址计算和条件码传递测试，
1,048,576 组数值通过；327 个合成顶点、片元及计算着色器覆盖新指令、纹理维度、输出掩码、
整数偏移及 R254/RZ 边界，全部通过 Vulkan 1.1 编译和 SPIR-V 验证。
原有 253,952 组 32 位算术/地址测试及 186 个着色器回归也通过。
这组指令回归不覆盖双精度数值，也不代表全游戏画面已通过验收。

```bash
mkdir -p .work/totk-port/shader-coverage/ops
clang -O1 -g -fsanitize=address,undefined tools/test_shader_library_ops.c -lm \
  -o .work/totk-port/shader-coverage/test-ops
.work/totk-port/shader-coverage/test-ops .work/totk-port/shader-coverage/ops
c++ -O2 -fsanitize=undefined .work/totk-port/shader-coverage/ops/arithmetic.cpp \
  -o .work/totk-port/shader-coverage/check-ops
.work/totk-port/shader-coverage/check-ops
for s in .work/totk-port/shader-coverage/ops/library-*; do
  case "$s" in *.vert|*.frag|*.comp)
    glslc --target-env=vulkan1.1 "$s" -o "$s.spv"
    spirv-val --target-env vulkan1.1 "$s.spv" ;;
  esac
done
```

审计工具可额外传 `--compile /绝对路径/libshaderprepare.so`，通过正式准备组件逐份编译并验证。
这仍只检查指定属性/状态配置的全库程序，不代表所有绘制状态变体均已覆盖；
报告分别列出 `translation_failed` 与 `spirv_validated`，失败原因用合法 JSON 转义保存。
默认审计整数双精度模式；添加 `--fp32-double` 审计 FP32 优先模式，报告包含 `double_arithmetic`。

## FP32 回归

以下命令在 dev 内运行。`--compiler` 可传本机版本的正式准备组件，省略则使用 glslc：

```bash
python3 tools/test_shader_fp32.py --out .work/totk-port/shader-fp32/tests \
  --compiler .work/totk-port/static-shaders/libshaderprepare.so
```

92 个合成着色器覆盖三种算术的寄存器/常量/立即数形式、四种舍入模式、开关切换、转换、修饰符、
条件执行及寄存器对边界，全部通过 Vulkan 1.1 SPIR-V 验证。检查确认原生 FP32/Fma、禁止融合修饰及
无 Float64/Int64 类型或能力。无双精度程序的源码在两模式间保持相同。
直接提取正式 GLSL 中的正常值转换代码，在 CPU 上与硬件 IEEE 转换比较，2,600,960 个样本通过，
覆盖正负号、指数范围、舍入中点和四种舍入模式；这不是 GPU 双精度算术数值测试。

30 份真实双精度程序中，FP32 模式共 360 处原生算术、0 处整数模拟算术；关闭后为 0/360。
两模式共 60 份 SPIR-V 通过正式组件验证及 64 位能力/类型扫描，APK 构建通过。
PJZ110 上原生审计也分别得到 8,697/8,697 翻译成功；只读取已有 RomFS，临时审计文件已清理。
报告和私有着色器样本保存在 `.work/totk-port/shader-fp32/`；没有安装覆盖手机上的可玩 APK。

## 清单与并发准备回归

`tools/audit_shader_variants.cpp` 使用与手机准备阶段相同的原生规划器，输出清单统计、具体类型组合及可选 GLSL 样本；
不会生成 `.shaders-ready`。主机快索引版本与逐程序扫描版本的完整清单逐字节一致。
`tools/test_shader_variant_plan.cpp` 覆盖类型保留、未确定字段、缺省路径、布局包含关系和状态枚举；
`tools/test_shader_prepare_queue.cpp` 覆盖有界提交、源码去重、错误传播、销毁和编译器会话关闭。

```bash
clang -O2 -c runtime/nvn/maxwell/translate.c -o .work/totk-port/shader-variants/translate.o
clang++ -std=c++17 -O2 tools/audit_shader_variants.cpp \
  .work/totk-port/shader-variants/translate.o -lzstd -o .work/totk-port/shader-variants/audit
.work/totk-port/shader-variants/audit .work/totk-port/game/romfs.bin \
  .work/totk-port/shader-variants/plan --samples
```

以下为接入共享基础模块之前的 v2 独立压缩缓存回归。
736 个真实组合样本通过 Vulkan 1.1 SPIR-V 验证；四线程正式准备组件生成并加载的对应 Vulkan 1.3 模块也通过外部验证。
样本去重后为 712 个源码键，原始 SPIR-V 合计 128,362,992 字节，含封装的压缩缓存为 32,003,741 字节，减少约 75%。
这是样本结果，不能当成整库大小或游戏帧率测量。
36 个内置着色器、压缩损坏/超大输出/尾随数据拒绝、重新准备修复和深度精度回归通过主机 ASan/UBSan。
手机 PJZ110 直接读取原有 RomFS，生成了与主机完全一致的 694,772 条目清单；同样的 736 个组合在手机上生成源码、
通过四个原生编译器会话准备并加载成功。此次冷缓存样本准备与加载耗时约 38.95 秒，输出缓存大小与主机一致。
没有从电脑搬运 SPIR-V；临时测试文件已全部清理，未覆盖已安装 APK。这是准备组件验证，尚未完成整库准备或游戏画面验收。

## 接口转换组件

`shader_interface.hpp/.cpp` 已加入 `libshaderprepare.so`，提供独立的准备阶段接口转换入口
`shader_prepare_derive`。它将浮点顶点输入或片元输出改为指定的 32 位整数类型，并在接口读写处
加入位转换；整数 RT0 的 Tint 读取归零并移除其资源绑定。算术指令和原有精度标记保留。
不支持的指针用法会失败，所有成功结果须通过 SPIRV-Tools 验证。

正式准备队列现已调用此入口，共享基础模块和 v3 字典缓存的集成验证见下一节。
以下为集成前的独立组件验证，不能把这些计数当成游戏缓存已准备完成。

dev 中完成以下独立验证，报告保存在 `.work/totk-port/shader-interfaces/`：

- `test_shader_interfaces.py`：72 组直接编译与接口转换结果在消除死代码、规范化 ID 和纯指令排列后相同，
  覆盖 Vulkan 1.1/1.3、开启/关闭优化、浮点/有符号/无符号及混合接口、Tint 保留/移除。
- `test_shader_interface_parser.cpp`：ASan/UBSan 下完成 1,517 个截断、字值修改、阶段和类型异常用例；
  其中 1,361 个被解析器或最终验证器拒绝，没有报告内存错误。部分字值修改仍是合法模块。
- `audit_shader_interfaces.py`：352 份真实基础源码、1,056 个类型变体通过 Vulkan 1.1 验证；
  接口与描述符集合和独立编译结果一致，算术指令、函数调用和 NoContraction 标记保持原样。
  该检查不等于实际 GPU 执行或游戏画面验证。
- Android debug APK 构建通过；未安装覆盖手机上的可玩版本。

主机样本中，转换并验证累计工作线程用时约 204.65 秒，同一批变体独立编译并验证约 372.05 秒。
这不是手机测速，也不是全库速度预测。转换结果合计 193,392,984 字节，独立编译结果合计
192,278,432 字节；接口转换本身没有减少存储体积，现由下面的字典缓存减少落盘体积。

```bash
# 在 dev 中运行；PLUGIN 指向包含 shader_prepare_derive 的主机准备组件。
python3 tools/test_shader_interfaces.py "$PLUGIN" .work/totk-port/shader-interfaces/synthetic
python3 tools/audit_shader_interfaces.py "$PLUGIN" .work/totk-port/shader-variants/plan \
  .work/totk-port/shader-interfaces/real
```

## 共享基础模块与字典缓存集成

`shader_prepare.cpp` 按内存路径、varying mask 和非类型状态分组，额外生成全浮点基础源码。
每个完整类型变体仍由正式 Maxwell 翻译器生成源码键，与游戏阶段的查询入口保持一致；队列只保留基础源码及变体键/类型表。
工作线程持有一个基础模块及其 Zstandard 编码字典，逐个派生、验证和原子保存完整 SPIR-V，不把全部派生模块留在内存。
重复基础键的编译和保存互斥，其他基础模块仍可并行；完整变体继续按源码键去重。
计算阶段及全浮点变体直接保存已验证基础模块的完整内容，不调用图形接口转换器。
任何编译、转换、验证或存储错误都会停止队列，不回退到游戏阶段编译，也不写完成标记。
日志分别记录 `prepared-source-keys` 和 `shared-base-keys`，派生失败包含完整变体键。

新增可复现回归入口（在 dev 内运行；第二个参数可省略以只运行合成用例）：

```bash
bash tools/test_shader_dictionaries.sh .work/totk-port/shader-dictionaries \
  .work/totk-port/game/romfs.bin
```

- `test_shader_dictionary.c`：完整字典解压、基础与变体损坏、字典身份错误、依赖链拒绝、旧缓存拒绝、写入失败、
  重新准备修复、跨线程/会话拒绝，以及四个线程遇到同一基础键时只编译一次。
- `test_shader_prepare_queue.cpp`：基础分组、完整变体去重和进度计数、编译/派生失败传播、取消和所有会话关闭。
- `test_shader_prepare_ready.cpp`：v3 完成身份、旧标记拒绝、配置/游戏文件变化及准备失败时清除标记。
- `test_shader_dictionaries.py`：通过正式四线程队列和游戏加载入口，对比共享字典与独立编译的输出。
  36 组合成变体覆盖两种优化模式、混合接口及 Tint 保留/移除，规范化 SPIR-V 完全一致。

全库清单仍为 694,772 条，重新分组后的 736 份真实样本源码集合与旧清单完全相同。
两条路径生成并加载的全部 736 份模块通过外部 Vulkan 1.3 `spirv-val`；共享路径的接口/描述符与独立编译一致，
算术、函数调用及 NoContraction 标记与基础模块一致。736 个条目去重为 712 个完整源码键，共享 371 个基础模块。
独立压缩缓存为 32,020,829 字节；字典缓存连同基础文件共 16,457,342 字节，减少约 48.6%。
其中基础文件占 16,210,251 字节，完整变体压缩条目占 247,091 字节。这是样本存储测量，不是全库预测。

缓存、队列、完成标记、36 个内置着色器及深度精度回归通过主机 ASan/UBSan；Android debug APK 构建通过。
产物检查确认 `libmain.so` 不依赖 shaderc，`shader_prepare_derive` 由准备插件导出。
报告位于 `.work/totk-port/shader-dictionaries/`，真实样本对照见 `integration/real/comparison.json`；
报告中的耗时来自主机检测构建，不能作为手机耗时推算。
这些检查仅验证准备与加载组件；后续手机整库时间/空间与加载验收见本文开头，正常游戏画面仍待验收。
