# 手机性能测试会话

常规测试使用 `benchmark`：自动加载独立存档，以 **60 FPS 目标**连续测量指定时长，然后关闭游戏。默认测量15秒；加载完成前的时间不计入结果。过程不暂停、不请求截图，也不需要逐项确认。旧的暂停/定帧数会话保留给显式诊断使用。

指定存档入口已在一加 13 / Adreno 830 上实测：不发送按键，直接读入 TotK 1.4.3 的 slot_00 空岛悬崖存档。历史 O0 库缓存启动约45–49秒，O1约30–38秒，全O3/8 Elite约31秒；这些是加载耗时，不是帧率收益。

## 独立测试包

在 `dev` 中 source 主工作区的 `android/dev_env.sh`，进入本分支 `android/botwrecompandroid` 后运行：

```sh
bash gradlew --offline -PperformanceTest=true assembleDebug
```

安装产物为 `com.totkrecomp.android.perftest`（TOTK Performance Test），可与普通应用共存。不传属性则构建普通包。独立包需要自己的游戏资料、相同驱动和图形设置，使用玩家自己的文件导入或复制一次即可。后续测试只安装、启动、停止这个包，避免另一项任务更新普通包而中断测试。不要使用未指定设备的 ADB 命令。

## 指定存档启动

在 `dev` 容器中使用，先 source 主工作区的 `android/dev_env.sh`：

```sh
"$PYTHON" tools/phone_test_session.py --serial 192.168.0.12:45117 \
  --package com.totkrecomp.android.perftest \
  --output /home/deck/Code/Games/TOTKRecompAndroid/.work/totk-port/performance-phone \
  benchmark --save-slot 0 --seconds 15
```

默认 seed 为测试包的 `files/performance/seed`，也可用 `--save-seed` 指定。每次使用新的 `benchmark-*` 目录和完整存档副本。`--seconds` 接受1–60秒；所有常规测量固定为60档，不遍历30/45/120。`--game-library <手机上的绝对路径>` 可选择已经校验的配套游戏库，避免重复部署；APK与游戏库必须使用同一profile和GAMELIB_ID。

运行时使用既有连续90帧场景G-buffer/绘制量条件开始计时，当前仍是启发式就绪判定，并非游戏内部“可操作”标志。计时结束时原子写入 `sample-0001.json` 和最终状态后请求进程退出；加载超过120秒、应用进入后台时也结束。渲染停止出帧时，事件线程仍可到期结束，未完成帧的等待计入总时长。脚本收集结果、日志、温度和电池状态，并在异常或进程未自行退出时关闭测试包。主机断连后运行时计时仍继续。

`launch.json` 记录包/库SHA256、启动环境及实际退出方；结果记录目标档位、实际FPS、总时长和帧耗时分位数。常规测试不启用逐帧诊断计数、抓帧或人工预热/暂停流程。30/45/60/120仍是游戏可选档位，性能测试只使用60。

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

运行时从玩家自己的 RomFS 读取两份原始 AINB 资源，检查长度和完整内容指纹后，在私有测试目录生成临时覆盖文件：

- `NormalTitle.140.module`：从标题页等待按钮的入口转入已有的确认读档分支。
- `OnLoadGameSaveData.module`：固定进度、辅助数据、足迹和 caption 的四处存档索引，保留原有重置、加载与读档后处理。

目前仅接受已校验的 TotK 1.4.3 原始资源；修改过的输入会被拒绝。APK 不携带游戏流程资源。普通启动不启用补丁。此入口跳过人工操作，不会跳过游戏本身的必要资源初始化；标题背景初始化尚保留。

`--auto-load` 是此前的模拟按键方案，仅为兼容保留，不能作为直接读档的验收证据。`--session` 可以指定独立的私有控制目录，后续所有命令需使用相同目录。

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

已有 run56 显示资源绑定、常量复制、顶点与纹理处理有明显成本，其中常量复制约 6.9 ms/帧；约 1.4 GiB/两秒的复制量主要是常量缓冲，不是纹理上传。最新单独的 simpleperf 渲染线程样本中，`memmove` 14.41%、`upload_texture` 13.81%、`getenv` 11.71%；这些是该线程 CPU 周期采样占比，不能直接换算成 FPS 增益。采集命令必须使用 `simpleperf record --in-app`。

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
"$PYTHON" tools/test_phone_benchmark.py
```

定时检查覆盖就绪后连续采样、无暂停、停帧到期、加载超时及后台退出；脚本使用mock ADB检查单次启动、结果收集、旧APK拒绝与退出兜底，不连接手机。已有C/脚本检查不等同于完整手机玩法验收。

`TEST_OUT` 为项目内新的测试输出目录，`SEQUENCES` 为从自己的 RomFS 提取的原始资源目录。另用独立的 [AINB 解析器](https://github.com/dt-12345/AINB) 核对六个槽位的完整结构，确认除了目标入口与索引连接之外没有变化。游戏资源与测试输出不得提交到仓库。
