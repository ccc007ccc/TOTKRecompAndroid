# 手机性能测试会话

2026-10-30 着色器改造与 worker/描述符模板/清除的实际补测，包含首次/重复、预热开关和温度范围，见
[手机补测记录](PHONE-SHADER-EXPERIMENTS-20261030.md)。目标 60 与成功退出不代表达到实际 2×60。

编译档位与默认开关见 [项目规范](../Agent.md)。本页保留分阶段测量；共享映射/异步复制的后续实现状态见
[整理基线](PROJECT-BASELINE.md)，较早章节的“尚未实现”只描述当时版本。
资源分类复制计数、shared-alias/async-copy 与专项测试已在收尾阶段提交。
后续 GPU 复制与纹理发布队列默认在 TotK 1.4.3 + Turnip/Adreno 8xx 上组合开启，共享映射仍默认关闭；
见 [第四轮性能记录](PERFORMANCE-DESCRIPTORS-20261029.md)。
2026-10-29 草修复后的性能定位、上传缓存优化与对照结果见 [首轮性能优化](PERFORMANCE-20261029.md)。
随后 GPU 阶段、复制依赖与长阶段提交频率的测量见 [第二轮性能优化](PERFORMANCE-PIPELINE-20261029.md)。

常规测试使用 `benchmark`：自动加载独立存档，以 **60 FPS 目标**连续测量指定时长，然后关闭游戏。默认测量15秒；加载完成前的时间不计入结果。过程不暂停、不请求截图，也不需要逐项确认。旧的暂停/定帧数会话保留给显式诊断使用。

指定存档入口已在一加 13 / Adreno 830 上实测：不发送按键，直接读入 TotK 1.4.3 的 slot_00 空岛悬崖存档。历史 O0 库缓存启动约45–49秒，O1约30–38秒，全O3/8 Elite约31秒；这些是加载耗时，不是帧率收益。

## 独立测试包

在项目根目录构建与现有 O3/8 Elite 游戏库配套的 Release 测试包：

```sh
bash android/build_totk.sh release --profile snapdragon-8elite --perftest
```

产物在 `.work/totk-port/packages/release-snapdragon-8elite-perftest/`，包名 `com.totkrecomp.android.perftest`，可与普通应用共存。
Release 测试包仍允许 `run-as`；不能因需要调试权限改用不匹配的 Debug/dev。
独立包需要自己的游戏资料、相同驱动和图形设置，使用玩家自己的文件导入或复制一次即可。
后续测试只安装、启动、停止这个包。不要使用未指定设备的 ADB 命令。

## 指定存档启动

在 `dev` 容器中使用，先 source 主工作区的 `android/dev_env.sh`：

```sh
"$PYTHON" tools/phone_test_session.py --serial 192.168.0.12:45117 \
  --package com.totkrecomp.android.perftest \
  --output /home/deck/Code/Games/TOTKRecompAndroid/.work/totk-port/performance-phone \
  benchmark --save-slot 0 --seconds 15
```

默认 seed 为测试包的 `files/performance/seed`，也可用 `--save-seed` 指定。每次使用新的 `benchmark-*` 目录和完整存档副本。`--seconds` 接受1–60秒；所有常规测量固定为60档，不遍历30/45/120。`--game-library <手机上的绝对路径>` 可选择已经校验的配套游戏库，避免重复部署；APK与游戏库必须使用同一profile和GAMELIB_ID。

纹理描述符对照可用 `--texture-reuse on|off`（同步模式合并相同注册，默认开）和
`--texture-updates queued|sync`（捕获后排队发布）。TotK 1.4.3 + Turnip/Adreno 8xx 默认排队发布并启用异步 buffer-copy，
其他游戏/GPU/驱动保留同步默认值；本轮同步对照同时指定 `--texture-updates sync --async-buffer-copy off`。
恢复相同注册仍排空的旧行为时再加 `--texture-reuse off`；这个开关不改变队列模式。这些开关不自动开启 profiling。
`--game-dynres default|on|off` 默认 `default`，不覆盖正常运行时策略（当前默认冻结游戏计数器驱动的动态分辨率）。
`--texture-scaling settings|native` 默认 `settings`，尊重玩家内部/输出分辨率；`native` 才显式传 `LARECOMP_NO_SCALE=1`，
用于原始尺寸的诊断对照。两个开关均记录于 `launch.json`，不同负载不混算。
旧脚本隐含关闭纹理缩放，2× 设置没有真正参与测量；历史 30 多帧不是 2× 游玩成绩，见 [口径修正](RELEASE-DIAGNOSTICS-20261029.md)。

`benchmark --camera-turn` 在场景就绪后的计时区间，以半幅右摇杆连续向右转镜头；普通测试默认不转。
运行时通过状态文件确认支持该路线，旧 APK 会被拒绝，不能静默测成原地等待。
首次与重复转视角分别记录，结合长帧和编译等待；此路线仍不代表移动、战斗或全区域游玩。

`--timing-trace` 还统计 `frame-limit`、`translation`、`drain`、`queue` 与纹理/采样器更新。
`texture-drain` / `sampler-drain` 是 `drain` 的子类，不相加；`texture-queued` / `texture-unchanged`
只统计次数。GPU 阶段标签包含首个 VS/FS、输出/写入 mask 和实际区域，单行时间仍可能包含提交间空档。

运行时使用既有连续90帧场景G-buffer/绘制量条件开始计时，当前仍是启发式就绪判定，并非游戏内部“可操作”标志。计时结束时原子写入 `sample-0001.json` 和最终状态后请求进程退出；加载超过120秒、应用进入后台时也结束。渲染停止出帧时，事件线程仍可到期结束，未完成帧的等待计入总时长。脚本收集结果、日志、温度和电池状态，并在异常或进程未自行退出时关闭测试包。主机断连后运行时计时仍继续。

`launch.json` 记录包/库SHA256、启动环境及实际退出方；结果记录目标档位、实际FPS、总时长和帧耗时分位数。常规测试不启用逐帧诊断计数、抓帧或人工预热/暂停流程。30/45/60/120仍是游戏可选档位，性能测试只使用60。

`--settings-seed <手机上的绝对路径>` 会把可读设置复制到本次私有会话，使用 `LARECOMP_SETTINGS` 启动，
并记录副本路径和初始 SHA-256。应用保存驱动选项时只影响该副本。优先使用测试包私有目录中的种子；
Android 的 `run-as` 不一定能读取应用在共享存储里的设置。
`--shader-warm on|off` 控制自动 SPIR-V 和管线准备；`--pipeline-warm on|off` 单独控制管线准备。
未传参数时沿用运行时默认值。对照需固定 APK、游戏库、设置、存档和目标档位，同时记录温度。
定时退出在 UI 线程停止准备任务、刷新最近状态并输出管线复用统计，然后保存缓存并退出。

`benchmark --gpu-profile-frame 1150` 在指定绝对帧记录各 GPU 阶段的时间戳；确认该帧确实出现在日志中。
`--timing-trace` 记录 GPU 计数器配对、提交间空档、队列等待，以及有预算限制的 buffer-copy 依赖信息。
两者都会启用 profiling，并写入 `launch.json`；GPU 时间戳回读会等待队列完成，结果只用于定位，
不能混入普通 FPS 对照。这些诊断不暂停游戏、不保存截图，仍按指定时长退出。

`--render-split-us 0..20000` 显式设置长绘制阶段的拆分阈值（微秒），0 表示不从长阶段中途拆分。
它不改变分辨率、帧率目标或同步语义，也不会自动开启 profiling；参数记录在 `launch.json`，
可用于同一 APK 的普通帧率对照。不传时使用驱动对应的运行时默认值。

`benchmark --shader-stats` 让驱动输出各管线的指令、寄存器和溢出统计，同样启用 profiling 并改变管线创建标志，
只用于定位。已预热复用的管线不经过统计路径；要覆盖热点着色器，需同时传 `--pipeline-warm off`。

`--diagnostic-env NAME=VALUE`（可重复）把一个现有测量开关交给运行时：名称只能是 `LARECOMP_*`、`TU_DEBUG`
或 `TU_AUTOTUNE_ALGO` / `TU_AUTOTUNE_FLAGS`，值为不含逗号和空白的单个词；与控制器自身设置的变量同名时拒绝启动。
它不自动开启 profiling，便于同一 APK 的帧率对照，但结果属于开关诊断，`launch.json` 单独记录 `diagnostic_env`。
`TU_DEBUG` 需在驱动加载前生效，运行时只在未设置时才写入 `nolrz`；例如 `TU_DEBUG=perf` 的驱动提示写入 logcat（标签 `TU`）。

`benchmark --apk <本地确切 APK>` 会在设备锁内校验包名并安装，再记录实际安装包的 SHA-256。
同一工作区的自动启动按设备序列号持有进程锁，另一控制器会在执行ADB前退出，避免互相force-stop。
锁文件存在不表示仍有测试运行，进程结束会自动释放锁。直接执行ADB安装或手动停止不受该锁保护；
安装前仍需检查目标包进程，避免中断另一项测量。其他工作区不共享这把锁；`start` 返回后也不继续持锁。

## 显式交互诊断

确需保留场景、手动取样或定位画面问题时使用旧 `start` 模式：

```sh
PHONE_TEST=("$PYTHON" tools/phone_test_session.py --serial 192.168.0.12:45117 \
  --package com.totkrecomp.android.perftest \
  --session /data/user/0/com.totkrecomp.android.perftest/files/performance/session \
  --output /home/deck/Code/Games/TOTKRecompAndroid/.work/totk-port/performance-phone/direct-slot-00)
"${PHONE_TEST[@]}" \
  start --save-seed /data/user/0/com.totkrecomp.android.perftest/files/performance/seed \
  --save-slot 0
```

`--save-slot 0..5` 对应游戏的 `slot_00..slot_05`，不是存档菜单中按日期排序后的行号。启动前完整复制存档根目录，包括 `stores/*/metadata`，到本次会话的新 `save-*` 目录；目标已存在、槽位不存在时拒绝启动。原存档保持不变。

指定槽位启动保留原有标题、存档列表和确认界面，自动发送确认按键。`NormalTitle.140.module` 保持原样，
避免跳过加载背景等界面状态的初始化。运行时只对玩家自己的 `OnLoadGameSaveData.module` 检查长度和完整指纹，
在测试私有目录生成临时覆盖，固定进度、辅助数据、足迹与 caption 四处索引；0..5 仍是文件槽位，不是菜单行号。
APK 不携带游戏流程资源，普通启动不启用测试输入或索引修改。

首次成功读取本次存档副本中所选槽位的 `progress.sav` 后，自动确认立即停止。资源预加载、caption 读取、
其他槽位和原存档不会触发停止；不会等到场景就绪后才停止按键。脚本的 120 秒启动上限和就绪后定时退出保持不变。

2026-10-29 已复现并修复此前自动读档特有的加载格线黑底，证据见 [画面验收](VISUAL-VALIDATION.md)。
草地后续修复及用户确认见 [草地与音频核对](GRASS-AND-AUDIO.md)。草地存档与历史裸岩悬崖性能种子不同，不能混作对照。

`--auto-load` 保留不指定槽位的旧按键模式；常规测试继续使用 `--save-slot`，让索引和输入停止条件都受控。
`--session` 可以指定独立的私有控制目录，后续所有命令需使用相同目录。

示例中的 seed 是预先准备的完整存档副本。`start --game-library <手机上的绝对路径>` 可通过 `LARECOMP_GAME_LIB` 选择另一份已有游戏库，例如私有目录中的 O1 库，用于单变量比较，不必替换默认库。工具检查应用能否读取它，并将 APK 和指定游戏库的 SHA256 写入 `launch.json`。

## 保留场景并重复采样

```sh
"${PHONE_TEST[@]}" status
"${PHONE_TEST[@]}" shot
"${PHONE_TEST[@]}" sample --frames 180 --cooldown 30
"${PHONE_TEST[@]}" resume
```

测试会话在帧边界排空 GPU 后暂停，SDL 事件线程仍处理控制命令。游戏画面上提供“暂停测试／继续游戏”，暂停不是进程崩溃。一次 `sample` 先运行 30 帧预热，随后记录指定帧数并再次暂停。支持 `pause`、`resume`、`gpu`、`shot`，诊断和 FPS 采样分开执行。

这个显式诊断模式在就绪后暂停；需要定位画面问题时可读取游戏自身窗口。超过120秒未到位会暂停并报告错误；状态查询和截图不会将它改成可采样状态，未就绪会话拒绝FPS采样。常规 `benchmark` 不走这套暂停/截图流程。

输出包含固定帧数的平均/中位/p95耗时、FPS、运行日志及测量前后的温度和电池状态。暂停区间不计入帧样本；普通日志和屏幕 FPS 在刚恢复时可能仍包含长暂停，应以 `sample-*.json` 为准。

`shot` 只读取游戏自己的窗口。截图保存到私有控制目录的 `window.png`，工具通过 `run-as tar` 取回为 `view-<帧号>.png`，不依赖外部 build 目录的写权限。读取 thermalservice 时应使用 **Current temperatures from HAL**，前面的缓存通知可能是很久以前的高温记录。

## 对照要求与已知线索

对照版取自抓帧前 `d9c6d52`，需带同样的 `43716c5` 黑块修复和本测试会话实现。两版使用相同槽位、游戏库、驱动、图形设置、视角、预热和温度范围。不能比较圆盘和空岛悬崖，或把带 simpleperf 的一轮直接与未采样的一轮相比。

2026-10-28 的同存档测试使用 Banner Turnip `v27.0.0-20261025-r5-A8xx`、8-bit gather、同一全量 O0 游戏库。诊断计数开启，实际抓帧及 RenderDoc 关闭，FPS 样本不运行 simpleperf。每版先运行 240 帧预热，再各次排除 30 帧预热、测量 180 帧，采样前暂停降温至少 30 秒：

| 版本 | 各次 FPS | 全部正式样本合并 FPS |
| --- | --- | --- |
| 抓帧前 + 同一黑块修复 | 19.468、19.471 | 19.469 |
| 当前抓帧版（未优化 getenv） | 19.704、18.567、19.896 | 19.371 |

电池温度旧版 29.3–29.5°C，当前版 28.7–29.5°C，thermal status 均为 0。合并使用总帧数/总耗时；保留波动较大的第三次样本，没有择优筛选。约 -0.5% 的差异没有在这个场景复现明显回退，但样本短、只有一个视角，不能排除其他场景或实际抓帧时的损耗。

已有 run56 显示资源绑定、常量处理、顶点与纹理处理有明显成本，其中常量处理约 6.9 ms/帧。旧日志约 1.4 GiB/两秒的 `copied` 计数来自共用的 `guest_copy`，也包含顶点流和 shader constants，不能直接认定为常量缓冲；2026-10-29 的分类实测见下节。单独的 simpleperf 渲染线程样本中，`memmove` 14.41%、`upload_texture` 13.81%、`getenv` 11.71%；这些是该线程 CPU 周期采样占比，不能直接换算成 FPS 增益。采集命令必须使用 `simpleperf record --in-app`。

本分支让 `vertex_debug_capturing()` 在普通帧提前返回，省掉每次顶点常量绑定的环境查询；调试帧仍按原设置执行。改动后单独 CPU 复测有 1139 个渲染线程样本，`getenv` 降至 0.10%，`memmove` 15.56%、`upload_texture` 15.70%。不带 simpleperf 的三轮为 18.302、19.332、19.693 FPS，尚未证明稳定的 FPS 提升。不同轮的采样占比不是同等幅度的总 CPU 耗时变化。

旧游戏库的 `TOTK_FAST_COMPILE=ON` 使全部生成代码使用 O0，仅适合首次启动排错。下面的历史 O1 基线关闭了该开关，保留 heavy.txt 中超大函数 O0、其余 O1。当前 `performance` 和 `snapdragon-8elite` 档已改为全部 O3 + ThinLTO，并拒绝 O0 例外，参数见[构建说明](BUILD.md)。NDK r30 在 LICM 内存提升阶段有异常慢的 MemorySSA 更新，CMake 默认 `TOTK_DISABLE_LICM_PROMOTION=ON` 仅禁用该步骤；可设 OFF 复查原编译行为。O1 库已完成下述实机短测，不能据此推断全游戏正确性。

本次 1.4.3 生成代码另有 `main/main_516.c`（单函数 11937 个标签），虽低于原 15000 阈值，仍在 SimpleLoopUnswitch 的 MemorySSA 更新中耗尽内存。使用 `-DTOTK_EXTRA_HEAVY_SOURCES=main/main_516.c` 明确增加这个 O0 例外；该选项默认空，不修改共享的 generated/heavy.txt。随后确认 main/main_484.c（10069 标签）和 main/main_568.c（9746 标签）也发生相同问题，三者均已显式列入例外。最终 O0 名单由 CMake 缓存写入构建记录，其他单元仍按 O1 编译。最终共 16 个 O0 大单元。资源紧张的主机应降低构建并发；本次以 `-j2` 完成编译，链接另以单任务执行。这些例外和编译参数必须随测量结果一起记录。

## O0 / O1 实机结果与 20 FPS 档位

同一 APK（SHA256 前缀 `a8e5a347d6f7d0f4`）、存档、视角、驱动和设置，电池温度 27.3–27.5°C，先运行 240 帧预热，再测三次 180 帧；每次仍排除 30 帧预热、暂停降温至少 30 秒：

| 游戏库 | 三次 FPS | 合并 FPS | 各次 p95 帧耗时 |
| --- | --- | --- | --- |
| 原全量 O0 | 19.929、18.198、19.326 | 19.124 | 60.19、67.44、63.68 ms |
| O1，16 个大单元保持 O0 | 20.000、20.000、19.999 | 20.000 | 51.27、51.20、51.15 ms |

本场景平均 FPS 约提高 4.6%，长帧明显减少。部署库从 525059280 字节降至 276045768 字节。O0 SHA256 前缀为 `c5265ce36cbecb55`，O1 为 `507fe278e9dc73de`；O1 保留的完整符号库 Build ID 为 `844486e05d5e9e10c004b580e7aa635d7d8f3ea5`。

该组历史采样的约 20 FPS 有明确的节奏限制：运行日志中游戏把 present interval 设为 3，`nvnQueuePresentTexture` 等待三个 60 Hz 周期。当时 `framepace_on()` 的可变时钟适配仅支持 BOTW 1.8.2，TotK 1.4.3 尚未启用。本分支随后加入了 TotK 30/45/60/120 档及实测步长，机制与验证范围见[上游整合](UPSTREAM-INTEGRATION.md)。旧 O1 路径的单独 CPU 采样中，游戏库占全进程周期样本的 51.55%；渲染线程仍以 `memmove`（16.45%）、`upload_texture`（15.36%）和绑定/绘制为主要热点。这些占比不能直接当作各环节的关键路径耗时。

`start --diagnostic-present-interval 2` 是保留的旧间隔诊断，开启 `LARECOMP_FORCE_INTERVAL=2` 和节奏日志。输出标为 `present-interval-diagnostic`；它可能改变游戏时间步长，不能作为可正常游玩的FPS提升或默认设置。当时采样前进程被从任务列表移除，因此没有可用的该项诊断FPS结论。当前四档使用后续适配的TotK实测时钟，见[上游整合](UPSTREAM-INTEGRATION.md)。

## 四档适配后的O1与全O3实测记录

2026-10-28，同一slot_00悬崖、60档，已有如下三轮180帧样本：

| 构建 | 各次FPS | 条件 |
| --- | --- | --- |
| 新计时O1（2个巨大单元O0） | 27.936、25.973、26.069 | 电池约30°C，未充电 |
| 全O3/ThinLTO、8 Elite | 22.216、23.536、22.873 | 电池约35.6°C，充电 |

两组温度与充电条件不同，不能据此判定O3带来提升或回退。按用户最新要求不再重跑这组构建对照，保留原始结果与条件差异。O1的30/45/120目标短测约27.78/27.61/27.46 FPS，只证明低吞吐场景下目标与计时路径可运行，没有证明稳定超过30帧或120帧玩法。

全O3库来自 `aaf348b`，1755个生成单元均为 `-O3 -flto=thin -fno-fast-math`，没有O0回退；8 Elite使用 `-march=armv8.7-a+crypto+fp16 -mtune=oryon-1`。游戏库264946296字节，SHA256 `5f3156b208076afa8b07d831d538977e643083ed6014bfd8711760d5137f087c`，GAMELIB_ID `993c0a5c82dc5099`；当时APK SHA256 `ac037f1031d1e42a171182d4b434784d3750351f6f5fbda6f34973b9ffb9c457`。后续只改运行时的修正可复用配套游戏库，不需要重编整库。

## 2026-10-29：统一内存与当前瓶颈

当前优先检查可消除的资源复制和同步等待。最新结果显示 CPU 渲染线程与 GPU 都有显著成本，
还不能把问题归为游戏 CPU 逻辑慢或 GPU 算力不足中的某一种；GPU 提交时长也包含其内部同步与内存访问成本。

PJZ110 / Turnip 的启动日志先出现 `no heap import`，随后实际通过 `VK_EXT_map_memory_placed` 将 **2120 MiB**
游戏堆映射为 GPU 内存，CPU/GPU 访问同一组页面。实际内存类型为 `0xf`（device-local、host-visible、coherent、cached）。
因此启动早期的日志不能作为整场仍在 CPU 复制模式的证据。超过 2047 MiB storage descriptor 范围的部分使用窗口访问。

一次 60 档自动诊断在同一进程每 60 帧交替普通 UBO 复制与已有的直接读取路径，30 秒后自行退出：

| 稳定日志行均值 | 复制 UBO | 共享 UBO 直读 |
| --- | ---: | ---: |
| 整帧周期 | 39.84 ms | 39.39 ms |
| 命令执行（含内部等待） | 24.45 ms | 23.95 ms |
| 渲染线程处理（含等待） | 25.64 ms | 25.17 ms |
| GPU 提交执行时间 | 26.00 ms | 25.77 ms |

去掉加载阶段，并去掉每次切换后前五条 pace 日志，保留 360 / 330 条稳定记录。
这是短时诊断，不构成约 0.5 ms 收益可重复的证明，也没有完成直接 UBO 路径的画面/同步验收，默认不启用。
普通 `benchmark` 仍不带这些计数，避免把诊断开销混进常规 FPS 样本。

随后将 `guest_copy` 的实际复制字节拆为普通 uniform、shader constants 和 vertex 三类。
新 APK 在独立 15 秒测试内的六个完整统计区间（289 帧、约 12.2 秒）得到：

| 复制类型 | 每帧实际复制 | 所在范围 |
| --- | ---: | --- |
| 顶点流 | **19.09 MiB** | 全部在共享游戏堆范围之外 |
| 普通常量缓冲 | 2.58 MiB | 全部在堆内 |
| shader constants | 0.54 MiB | 主要在堆外 |

顶点流占上述三类实际复制字节的 **85.94%**。这是共用 guest-copy 的细分，不包含所有纹理转换、索引展开或 GPU 内部复制。
同一区间渲染命令执行约 26.09 ms/帧、GPU 提交约 26.85 ms/帧；其中 `buffer -> buffer copy` 的强制 GPU 等待约 **4.01 ms/帧**。
CPU/GPU 时间有重叠，不能相加；渲染命令执行是墙钟时间，不能冒充纯 CPU 核心执行时间。

因此优先方向是把**堆外顶点资源**纳入 GPU 可见的共享分配和绑定路径。目前 `guest_direct` 只覆盖已有的 `gmem_buf`，
堆外资源会逐次进入 staging ring。需继续确认实际资源地址、分配入口及生命周期；`physical_memory` 的 alias 区分配是待核对入口，
尚未证明全部顶点流来自该入口。不能简单把指针范围检查删除，因为普通 CPU 虚拟地址并不自动对应一个已注册的 Vulkan buffer。

第二个方向是把 buffer-copy 的立即 `renderer_flush` 改为受资源依赖约束的处理，尽量留在 GPU 队列中，
真正发生 CPU 读取时再等待。不同地址间的游戏语义复制仍需要保留；同一物理内存也不能取消写入顺序和在途读取保护。
纹理另有 Tegra 布局转换，不能直接套用线性顶点 buffer 的同一方案。

复现命令仍自动读档、60 档、定时退出，无暂停/截图：

```bash
# source android/dev_env.sh 后，使用前文相同 serial/package/game-library 参数
"$PYTHON" tools/phone_test_session.py --serial 192.168.0.12:45117 \
  --package com.totkrecomp.android.perftest --output .work/totk-port/memory-profile \
  benchmark --seconds 15 --profile
# 比较已有 UBO 直读时，将 --profile 换为 --compare-ubo 60；该项仅作诊断。
```

`launch.json` 保存诊断设置和 APK/library 身份。`LARECOMP_LOG_CBUF_BYTES` 现在额外输出 `[copy-bytes]` 的三类实际复制及堆外字节。
测试脚本六项模拟设备检查、上传缓存 helper 检查和新 APK 构建通过；两次手机会话都由运行时按时退出。
证据目录 `.work/totk-port/memory-bottleneck-20261029/` 的 `summary.json`、`classified-blocks.json` 和两份 phone/classified 日志。
两次 APK 身份不同，不跨两次测试比较 FPS；新版本只是补齐诊断，还未实现堆外资源零拷贝。

## 离线验证

```sh
clang -O1 -g -fsanitize=address,undefined -Wall -Wextra \
  tools/test_test_boot.c runtime/test_boot.c -o "$TEST_OUT/test-boot"
"$TEST_OUT/test-boot" "$SEQUENCES/NormalTitle.140.module.ainb" \
  "$SEQUENCES/OnLoadGameSaveData.module.ainb"

clang -O1 -g -fsanitize=address,undefined -Wall -Wextra \
  tools/test_test_session.c runtime/test_session.c -lpthread -lm -o "$TEST_OUT/test-session"
"$TEST_OUT/test-session" "$TEST_OUT/new-session"
"$TEST_OUT/test-session" "$TEST_OUT/new-timeout-session" --startup-timeout
for mode in timed timed-stall timed-timeout timed-background; do
  "$TEST_OUT/test-session" "$TEST_OUT/new-$mode" "--$mode"
done
clang -O1 -g -fsanitize=address,undefined -Wall -Wextra \
  tools/test_test_autoload.c runtime/test_session.c -lpthread -lm -o "$TEST_OUT/test-autoload"
mkdir -p "$TEST_OUT/new-autoload"
"$TEST_OUT/test-autoload" "$TEST_OUT/new-autoload"
"$PYTHON" tools/test_phone_benchmark.py
```

定时检查覆盖就绪后连续采样、无暂停、停帧到期、加载超时及后台退出；脚本使用mock ADB检查单次启动、结果收集、旧APK拒绝与退出兜底，不连接手机。已有C/脚本检查不等同于完整手机玩法验收。

`TEST_OUT` 为项目内新的测试输出目录，`SEQUENCES` 为从自己的 RomFS 提取的原始资源目录。另用独立的 [AINB 解析器](https://github.com/dt-12345/AINB) 核对六个槽位的完整结构，确认标题资源不变、读档资源只有四处索引连接变化。游戏资源与测试输出不得提交到仓库。
