# TOTKRecompAndroid 项目规范

本文件是项目操作和代码维护的统一规范，适用于开发者与自动化助手。命令默认从仓库根目录执行。
规范与实现一起维护；遇到不一致先核对源码和实际产物，修正过期说明，不绕过校验。
阶段性调查、性能数字和待验收项放在专题文档，避免把某次实验写成长期默认行为。

## 1. 项目边界与工作方式

- 当前目标是 **Android ARM64 / Nintendo Switch《王国之泪》1.4.3**，程序 ID `0100F2C0115B6000`，游戏库 ABI 2。
  `main`、`sdk`、`subsdk0`、`rtld` 四个模块及各自完整 32-byte Build ID 都必须核对。
- 本项目继承 BOTWRecompAndroid/LASRecomp 的生成器和运行时。保留的 `botwrecompandroid` 目录、Java/JNI 名字、
  `LARECOMP_*`、部分 `LAS_*` 参数是技术接口；不要为统一名称批量改动它们。
- 当前按已有约定在 `main` 工作。不要为了整理自动切分、重置或搬移已有的混合改动；开始工作先看
  `git status --short`、`git diff --stat` 和相关文件差异。需要隔离工作时先明确范围和共有文件归属。
- 着色器工作负责准备正确性、编译速度和缓存复用；共享内存、资源同步、帧率优化是另一类任务。
  修改 `exec.c`、`device.c`、`vk.h`、Gradle/CMake 和手机控制器等共有文件前核对现状，禁止用旧副本整文件覆盖。
- 规范整理不自动续跑历史优化、整库编译或实机测量任务。当前要求不包含额外代理并行工作。
- 两个 2026-10-29 对话的范围、结果和未完成项见 [项目整理基线](docs/PROJECT-BASELINE.md)。
  着色器链路和项目规范已提交；后续收尾接入堆外共享映射、异步 buffer-copy 及专项测试，
  当时两个开关默认关闭；后续 GPU 复制与纹理发布的组合默认值见第 7 节及第四轮记录。
  收尾范围与验收见 [收尾记录](docs/PROJECT-CLOSEOUT.md)。

## 2. 环境与工具链

SteamOS 宿主不可变。编译、依赖安装、生成器、测试与 `gh` 全部使用现有 **Distrobox `dev`**；
宿主负责编辑、Git 检查和进入容器，不使用宿主包管理器搭建编译环境。

```bash
distrobox enter dev
cd /home/deck/Code/Games/TOTKRecompAndroid
source android/dev_env.sh
```

自动化可用 `distrobox enter dev -- bash -lc 'source android/dev_env.sh; ...'`。
`android/build_totk.sh`、`build_gamelib.sh` 等可从宿主调用；在其他容器里不要依赖它们的容器探测来选择 `dev`。

| 工具/平台 | 约定与来源 |
| --- | --- |
| Android SDK | 默认 `/home/deck/Code/Toolchains/android-sdk`；`ANDROID_HOME` 可覆盖；compile/target SDK 36，min SDK/API 30 |
| Android NDK | r30，Gradle 固定 `30.0.16248370`；默认 `/home/deck/Code/Toolchains/android-ndk-r30`；Clang 21 + LLD |
| 构建系统 | 仓库 Gradle Wrapper 8.14.3、AGP 8.13.0、SDK CMake 4.1.2、Ninja；不用系统 Gradle 替换 Wrapper |
| Java | 用 Java 21 运行 Gradle；Java 源码/字节码兼容级别 17 |
| Python | `android/dev_env.sh` 的 `$PYTHON`，默认 `.work/totk-port/venv/bin/python`；首次 APK 构建创建 venv |
| 生成器依赖 | 当前固定 Capstone 5.0.7、Unicorn 2.1.4、LZ4 4.4.5，版本维护在 `android/build_totk.sh` |
| 桌面合成测试 | `dev` 的 Clang/Clang++、shaderc、SPIRV-Tools、glslang、zstd；需要 GPU 的测试还需 Vulkan ICD/验证层 |
| 其他容器 | `dev-ubuntu-22-04`（glibc 2.35）、`dev-debian-12`（2.36）、`dev-fedora-43`（2.42）留作对应 Linux 包兼容验收；本项目当前 Android 交付不把它们列为常规构建目标 |

`ANDROID_NDK` 与 Gradle 的 `ndkVersion` 必须指向同一版工具链。升级 SDK/NDK、生成器或编译参数时，
连同依赖、原生库身份和端侧参数一起核验，不能仅更改本机环境变量。
`android/deps/build_deps.sh` 固定下载版本和 SHA-256；FFmpeg、RenderDoc、端侧生成器分别有专用脚本。
这些依赖按各自脚本编译，不宣称全部随应用 profile 变成 O3/ThinLTO。

## 3. 目录与职责

| 路径 | 放什么 / 在哪里修改 |
| --- | --- |
| `runtime/` | 装载、内核、服务、HLE、设置和平台实现 |
| `runtime/nvn/` | NVN 命令/对象、Tegra 布局、通用渲染契约 |
| `runtime/nvn/maxwell/` | 正式 Maxwell → GLSL 翻译器及数值实现 |
| `runtime/nvn/vulkan/` | Vulkan 执行、资源/同步、着色器缓存/准备、管线及诊断 |
| `runtime/ui/` | 启动器、设置、触控、翻译；`.mm` 在 Android 作为 C++ 编译 |
| `android/botwrecompandroid/` | APK、Activity/JNI、资源和 Gradle；不是另一个 BOTW 产品 |
| `android/gamelib/` | 生成游戏库的 NDK CMake 工程 |
| `android/build_profile.sh` / `build_profiles.cmake` / `gamelib_id.cmake` | CLI 档位选择 / 原生编译参数 / 游戏库身份的正式来源 |
| `tools/` | 输入准备、生成器、打包部署、审计、合成测试；优先扩展现有入口 |
| `third_party/` | 带来源和许可证的上游代码/元数据；适配层写在本项目目录 |
| `docs/` | 当前中文指南和专题记录；`docs/en/` 为英文指南，`docs/las/` 等为历史材料 |
| `companion/`、`android/probe/` | 保留的伴侣应用/独立探针；不构成 TOTK 主应用已经支持的功能 |
| `.work/totk-port/` | 本机游戏输入、生成代码、构建包、日志与私有证据，全部忽略 |
| `build/`、`android/deps/{src,work,out}/`、Gradle `build/`/`.cxx/` | 已有构建/测试/依赖产物，不能当源码维护 |

新临时任务使用 `.work/<任务名>-<日期>/`，例行构建沿用 `.work/totk-port/`。
不要批量搬动已有产物目录；构建缓存、手机路径和历史证据可能引用它们。
生成的 C、GLSL、SPIR-V、头文件或打包资源应修改其生成源，不能只修改输出。

## 4. Debug、Release 与编译档位

**variant 决定应用包装/调试属性；profile 决定原生代码优化和游戏库身份；`--perftest` 决定独立测试包。**
普通 Release 必须设置原生 `TOTK_DIAGNOSTICS=0` 和 Java `BuildConfig.DIAGNOSTICS=false`；
Debug 与独立测试包设置为 1 / true。诊断开关与 O1/O3 档位分开，不能靠 `NDEBUG`、签名或图标判断。
普通 Release 不打包 RenderDoc，不编入抓帧/测试会话后端，关闭 F8/F9/F10 抓帧入口、脚本输入、内存探针和详细 profiling，
也不接受 Intent 的 `env` 测试参数。诊断环境读取使用 `diagnostics_build.h` 的 helper；新增探针必须受同一构建策略控制。
保留正常触控/按键绑定、FPS 显示、故障报告、着色器准备及渲染优化。特别是 `fnprof_hit` 的调度抢占位仍属正式功能。
普通包保留 Android `profileable android:shell="true"` 的外部 CPU 采样能力；没有自动采样，且不因此开启内置诊断或 debuggable。
功能默认值另见第 7 节。

| 用途 | 推荐参数 | 原生运行时、hooks、编译插件 | 生成游戏单元 |
| --- | --- | --- | --- |
| 日常 Debug | `debug --profile dev` | NDK Clang，O2 | O1；允许已有冷函数/巨型函数 O0 策略 |
| 通用 Release | `release --profile performance` | NDK Clang，O3 + ThinLTO | 全部 O3 + ThinLTO，禁止 O0/optnone 例外 |
| 已知 8 Elite 设备 | `release --profile snapdragon-8elite` | O3 + ThinLTO，`-march=armv8.7-a+crypto+fp16 -mtune=oryon-1` | 同一组参数，巨大函数也优化 |
| 一加 13 实机测试 | 上一行再加 `--perftest` | 同一 Release 原生档；Java 应用保留 debuggable/run-as | 复用匹配的 8 Elite O3 游戏库 |

Debug 与 Release 都使用 **NDK Clang** 编译 Android 代码；`dev` 的系统编译器只用于桌面工具/测试。
`snapdragon-8elite` 是设备专用档，不作为所有 ARM64 手机的通用发布包；禁止用 `-march=native`。
性能档保留 `-fno-fast-math`。`shader_fp32` 是另一个运行时选择，不等于允许编译器任意改变浮点语义。

```bash
# 宿主或 dev：只构建 APK，不安装、不生成游戏库
bash android/build_totk.sh debug --profile dev
bash android/build_totk.sh release --profile performance
bash android/build_totk.sh release --profile snapdragon-8elite --perftest
```

无参数默认 Debug/dev；只传 `release` 默认 performance；只传性能 profile 默认 Release。
脚本也读取 `TOTK_BUILD_VARIANT`、`TOTK_BUILD_PROFILE`、`TOTK_PERFORMANCE_TEST`；自动化中明确传参并检查这些继承变量，
不要在 shell 中遗留 `TOTK_PERFORMANCE_TEST=true` 后误建普通包。
`debug --profile performance` 等组合用于定向调试，不能仅根据文件名“Debug”判断优化级别。
游戏库 CMake 固定使用 `CMAKE_BUILD_TYPE=Release`，其实际 O1/O3 策略由 **profile** 控制。

资源不足时在 `dev` 内设置 `export JOBS=2 TOTK_LTO_JOBS=1`；默认脚本 JOBS=6、LTO jobs=2。
Ninja 编译池和 ThinLTO 后端分别限制，链接单任务执行。不要为了节省编译时间给性能档加 O0 回退。
`GAMELIB_FAST_COMPILE`、`GAMELIB_EXTRA_HEAVY_SOURCES` 只用于 dev 排错；
`GAMELIB_DISABLE_LICM_PROMOTION` 默认 ON，性能档前置编译和 ThinLTO 后端都保留该已知编译器规避措施。

### 4.1 产物、包名与签名

最终交付目录是 `.work/totk-port/packages/<variant>-<profile>[-perftest]/`：

- `TOTKRecompAndroid-<variant>-<profile>-arm64-v8a.apk`：以实际 manifest 核对包名。
- `libmain.so`：从这个 APK 提取的链接用运行时；不要拿任意旧构建里的库链接游戏。
- `build.json`：variant/profile、实际 `GAMELIB_ID`、包名、APK/运行时 SHA-256 等打包记录。
- 游戏库构建后另有 `totkgame.so`、`.id`、`.profile`、`.game`；`.game` 当前为 `143`。

| 应用 | 包名 | 属性 |
| --- | --- | --- |
| Debug | `com.totkrecomp.android.debug` | 独立数据，名称带 Debug，启动图标带蓝色 DBG 标记 |
| 普通 Release | `com.totkrecomp.android` | 原启动图标，无 DBG 标记；非 debuggable，不能假定可以 run-as |
| Performance Test | `com.totkrecomp.android.perftest` | Debug/Release 都是此包名；Release 也使用 debug 签名并开启 run-as |

Debug 启动图标由 `android/botwrecompandroid/app/src/debug/res/mipmap-xxxhdpi/ic_launcher.png` 覆盖；
Release 使用从 `assets/artwork/triforce-icon-v2.png` 生成的主资源。图标按 variant 选择，
不受原生 profile 或签名密钥影响；Release/perftest 使用 debug 签名也不会自动带 DBG 图标。

普通 Release 从 `~/.totkrecompandroid/keystore.properties` 读取签名配置；未配置时**实际会回退到 debug 签名**。
因此“Release 构建成功”不代表已经具备正式发布签名。发布前核对签名、versionCode/versionName、包名、profile、
SHA-256 和测试范围；密钥/密码不得进入仓库、日志或 PR。

底层 Gradle 输出不作为稳定交付路径：dev 通常在 `app/build/`，显式性能 profile 在
`.work/totk-port/builds/<profile>/{app,perftest}/`。dev 普通包与测试包的中间目录尚未隔离，
同一源码树的 Gradle 构建应串行，构建后立即通过打包脚本固化身份。直接 `gradlew assemble*` 只作局部诊断，
不会自动更新 packages 里的 APK/`libmain.so`/`build.json`。

### 4.2 准备和编译游戏库

首次用自己的本体、1.4.3 更新、密钥按 [构建指南](docs/BUILD.md#准备自己的游戏) 生成
`.work/totk-port/game/exefs/`、`romfs.bin` 和视频目录。先构建匹配 APK，再构建游戏库：

```bash
# 与上面 APK 选择同一组参数；三选一，不需要每次全量重建
bash android/build_gamelib.sh .work/totk-port/game/exefs debug --profile dev
bash android/build_gamelib.sh .work/totk-port/game/exefs release --profile performance
bash android/build_gamelib.sh .work/totk-port/game/exefs release --profile snapdragon-8elite --perftest
```

生成目录默认 `.work/totk-port/generated`，可用 `GAMELIB_GEN` 覆盖；输出可用 `GAMELIB_OUT` 覆盖。
`input.id` 绑定四模块输入和生成器；`GAMELIB_ID` 绑定生成器/公共头/生成编译参数。
修改生成器、ABI/公共头、profile 时重建配套游戏库；只改普通运行时代码通常只需重建 APK，仍要核对身份。
NDK/依赖升级另做工具链验收，不能认为当前 `GAMELIB_ID` 已自动涵盖所有工具链二进制变化。
严禁手改 `.id`/`.profile`、去掉版本检查或给不匹配的库换名来“解决加载失败”。

手机端完整游戏代码编译仍待验收，当前受支持的开发流程是电脑生成 `totkgame.so` 后通过 APK 装载。
手机本地的**着色器准备**已接入，不要把这两种编译混为一谈。

## 5. 部署与设备操作

先在 `dev` 内用 `adb devices -l` 确认目标，再 `export ANDROID_SERIAL='<实际一加序列号或无线地址>'`。
历史测试机是 PJZ110 / 一加 13 / Adreno 830，旧无线地址不是固定配置；**不操作 Quest 3 或其他无关设备**。
所有 ADB 命令显式 `-s "$ANDROID_SERIAL"`，不清空全局 logcat，不覆盖普通包的设置/存档来做试验。

首次部署示例（在 dev、游戏输入和配套库已就绪）：

```bash
TOTK_STAGE=.work/totk-port/packages/debug-dev
"$PYTHON" tools/deploy_totk.py .work/totk-port/game \
  --serial "$ANDROID_SERIAL" --variant debug --profile dev \
  --package com.totkrecomp.android.debug --library "$TOTK_STAGE" \
  --apk "$TOTK_STAGE/TOTKRecompAndroid-debug-dev-arm64-v8a.apk" --launch
```

换包/档位时同时改目录、variant、profile、package 和 APK；部署器检查实际 manifest、运行时和库身份。
`android/push_game.sh <game> ...` 仅推送配套数据，不能替代 APK 安装。
日常运行时更新先用 `build_totk.sh` 重新打包，再安装 packages 中的确切 APK。
`android/run_boot.sh` 会停止/重启所选包，只限时收日志、结束后保留游戏；它不是自动退出的性能测试入口，
且通过 `TOTK_BUILD_VARIANT/PROFILE/PERFORMANCE_TEST` 环境选择包，缺省是 Debug/dev。

## 6. 测试与证据

### 6.1 本地验证

所有命令在 `dev` 内、先 `source android/dev_env.sh`。按修改范围选择既有测试；
文档/纯排版不新增镜像实现的测试，解析、身份、数值、缓存、并发变更需覆盖失败路径。
当前没有一个能宣称验证整个游戏的统一测试命令。

| 修改范围 | 现有入口 |
| --- | --- |
| 输入/生成器 | `"$PYTHON" tools/test_totk.py`、`"$PYTHON" tools/test_romfs_build.py`、`"$PYTHON" tools/test_exclusive_pair.py`、`"$PYTHON" tools/test_local_branches.py` |
| 游戏计时/呈现间隔 | `"$PYTHON" tools/test_totk_framepace.py`（可传 `--image <私有 main-image.bin>`）、`bash tools/test_present_interval.sh` |
| APK/库身份、端侧参数 | `"$PYTHON" tools/test_android_packages.py`；`"$PYTHON" tools/test_gamebuild_flags.py`；实际构建相关 variant/profile |
| 诊断构建隔离 | `"$PYTHON" tools/check_android_diagnostics.py <实际 APK> --expect off\|on`；Release 的 off 和 Debug/测试包的 on 都须验收；编译运行 `tools/test_release_diagnostics.c` 验证调度抢占未被裁掉 |
| 着色器缓存/编译 | `bash tools/test_shader_runtime.sh`；`bash tools/test_shader_cache_compat.sh`；`bash tools/test_shader_dictionaries.sh` |
| 驱动常量 | `"$PYTHON" tools/test_shader_driver_constants.py`（点尺寸上限、阶段隔离、存储表和派发尺寸） |
| 整数着色器语义 | `"$PYTHON" tools/test_shader_int32.py --out .work/totk-port/shader-int32-tests`（含 IADD3 移位/半字选择） |
| 图元展开/全局内存类型 | `"$PYTHON" tools/test_primitive_indices.py`；`"$PYTHON" tools/test_global_arena_memory.py` |
| 程序/管线索引容量、重启 | `"$PYTHON" tools/test_shader_program_cache.py`；`"$PYTHON" tools/test_shader_lifetimes.py` |
| 共享映射、异步复制 | `bash tools/test_shared_memory.sh`；`bash tools/test_buffer_copy.sh` |
| 纹理描述符注册/队列 | `bash tools/test_texture_registration.sh`（含真实请求队列、引用寿命和对象锁回归） |
| 独立 Vulkan 录制 worker | `bash tools/test_vk_command_worker.sh`（参数快照/背压、实际请求队列、真实 Vulkan 同步验证；`TOTK_WORKER_TSAN=1` 追加线程检查，实验仅诊断包且默认关闭） |
| 描述符更新模板/分类比较 | `bash tools/test_descriptor_updates.sh`（实际 stage_set、失败/生命周期与原写入/模板/worker 组合的真实 GPU 值；实验仅诊断包且默认关闭） |
| 首次完整清除合入 loadOp / 深度统计 | `bash tools/test_pass_clear.sh <输出目录> [自己的 romfs.bin]`（实际 pass/scissor/clear 的 ASan/UBSan、传统/动态 pass 与 worker 的 GPU 比较；强制验证层；实验仅诊断包且默认关闭） |
| GPU 纹理排布转换 / 写通知策略 | `bash tools/test_vk_deswizzle.sh`（真实 Vulkan、同步验证、快照与回收）；`"$PYTHON" tools/test_texture_change_policy.py <输出目录>` |
| 可变图像格式列表（UBWC） | `"$PYTHON" tools/test_image_format_lists.py <输出目录>`（按独立尺寸表核对整类视图格式、E5B9G9R9 排除和默认设备条件） |
| 清单/准备/生命周期 | `bash tools/test_shader_warm_list.sh`；`bash tools/test_shader_warm_prepare.sh`；`bash tools/test_shader_warm_background.sh` |
| 管线 | `bash tools/test_pipeline_cache.sh`；`bash tools/test_pipeline_warm.sh`；GPU：`bash tools/test_vk_pipeline_warm.sh` |
| Eden 对应运行时路径 | `bash tools/test_eden_runtime.sh`（含上传缓存、描述符、请求队列与编译回退） |
| 手机控制器（模拟 ADB） | `"$PYTHON" tools/test_phone_benchmark.py`，不连接手机 |
| 伴侣代码 | `npm --prefix companion/zepp test`，不代表 TOTK 地图已经适配 |

着色器 prepare/background 脚本可传 `<独立输出目录> <自己的 romfs.bin>` 执行真实输入集成检查；
不传 RomFS 时只报告实际执行的合成范围。其余纹理、音频、抓帧和 GPU 数值命令见 [测试指南](docs/TESTING.md)。
`tools/test_lift.py` 需要 macOS/ARM64，x86_64 dev 未运行不能记为通过。
桌面插件统一使用 `sh tools/build_host_shader_compiler.sh <输出目录>`，包含 C 编译器和 C++ 接口实现；
不要再复制一套只编译 `shader_compiler.c` 的旧命令。

### 6.2 常规手机测试

已有配套游戏资料、完整存档种子和设置种子后，使用 **Release/8 Elite/perftest** 及现有 O3 游戏库。
不要为了验证运行时代码重新进行 O0/O1/O3 比较或重建整套游戏库。

```bash
TOTK_STAGE=.work/totk-port/packages/release-snapdragon-8elite-perftest
"$PYTHON" tools/phone_test_session.py --serial "$ANDROID_SERIAL" \
  --package com.totkrecomp.android.perftest --output .work/totk-port/phone-check \
  benchmark --apk "$TOTK_STAGE/TOTKRecompAndroid-release-snapdragon-8elite-arm64-v8a.apk" \
  --save-slot 0 --seconds 15 \
  --game-library '<测试包内已校验游戏库的绝对路径>' \
  --settings-seed '<测试包内已有设置种子的绝对路径>'
```

尖括号处必须换成实际路径；默认存档种子为测试包 `files/performance/seed`，也可用 `--save-seed` 指定。
自动复制完整存档，固定 **60 FPS 目标**，加载就绪后计时、到时退出；不暂停、不例行截图。
指定槽位保留正常菜单确认流程，实际读取目标进度文件后停止自动按键；不要改写标题入口跳过界面初始化。
默认 15 秒，可选 1–60 秒。就绪条件是连续 90 帧场景 G-buffer/绘制量启发式，不是内部“已可操作”标志。
`--profile` 是诊断计数开关，和构建脚本的 `--profile <编译档>` 含义不同；诊断/回读结果单独记账。
`--gpu-profile-frame` / `--timing-trace` / `--shader-stats` 自动启用诊断；`--render-split-us` 只覆盖提交阈值。
`--diagnostic-env NAME=VALUE` 只接受 `LARECOMP_*` 测量开关和 Turnip 的 `TU_DEBUG` / `TU_AUTOTUNE_*`，
不能覆盖控制器自身设置；它不自动启用诊断计数，但结果属于开关对照，不能当作默认配置成绩。
这些参数记录在 `launch.json`，用法与时间戳回读的影响见 [手机性能测试](docs/PHONE-PERFORMANCE.md)。

控制器对同一工作区的设备序列号加进程锁；`benchmark --apk` 将安装纳入该锁。
直接 ADB、其他工作区、手动操作不受它保护，`start` 返回后也不持有整个交互会话的锁。
锁文件存在不是进程存活证据；先核对控制器/应用再操作，不用删除锁文件抢设备。
不要在测量中途替换 APK。开始前固定并备份所需产物，比较 `launch.json` 与目标 APK/库的 SHA-256。

每次报告保留：源码提交及脏树范围、APK/库 SHA-256、profile/GAMELIB_ID、设备/驱动、设置/开关、
存档场景、冷热缓存、温度、电源状态、有效区间及退出原因。
常规测试必须尊重分辨率设置，禁止暗中传 `LARECOMP_NO_SCALE=1`。`--texture-scaling native` 仅用于明确标注的原始尺寸对照。
静止短测不代表正常游玩；转视角问题至少另跑 `--camera-turn` 并记录首次/重复路线，不能用平均 FPS 掩盖长帧。
分别报告构建、合成测试、真实 GPU 数值、手机启动、画面/路线验收；不能互相代替。
CPU 渲染墙钟含等待，GPU 时间可能与它重叠；复制字节占比不是耗时占比；60 档不是实测 60 FPS。
超时、身份不匹配、被另一个任务停止或装包的运行不计为通过。

## 7. 功能默认值与缓存契约

下表是当前 Android 的默认值，不随 Debug/Release 自动切换。修改默认值应同时更新设置/UI、测试与本表；
只增加实验代码不代表已批准默认开启。已有设置和启动环境可以覆盖默认值，多数开关需重启生效。

| 功能 | 默认 | 控制与正式来源 |
| --- | --- | --- |
| FP32 优先 | 开 | `shader_fp32`；`settings.c` / `exec.c`，关闭保留整数双精度路径 |
| 缺失/损坏着色器运行时编译 | 开 | `shader_runtime_compile`；`LARECOMP_SHADER_RUNTIME_COMPILE` 优先覆盖，见 `shaders.c` |
| 标题/后台 SPIR-V 准备 | 开 | `LARECOMP_SHADER_WARM=0` 关闭，见 `shader_warm_job.cpp` / `shader_warm_boot.cpp` |
| 已准备的接口元数据与 SPIR-V 直接加载 | 开，完整校验后复用 | `LARECOMP_SHADER_ARTIFACTS=0` 对照原译码；诊断源码导出/替换/插桩自动走源码路径，见 `shader_artifact.cpp` |
| 实际管线预热 | 开，有记录才工作 | `LARECOMP_PIPELINE_WARM=0`；总开关 `LARECOMP_SHADER_WARM=0` 也关闭，见 `pipeline_warm.c` |
| 等待首次需要的着色器/管线 | 开 | 默认完成后执行绘制/计算；`LARECOMP_ASYNC_SKIP_DRAWS=1` 仅诊断旧跳过策略 |
| 提交时内存快照 | 关 | `LARECOMP_VK_SUBMIT_SNAPSHOT=1`，`submit_snapshot.c` |
| 顶点采样量化 | 关 | `LARECOMP_VS_SNAP=1`，`exec.c`；不能标成已修复草消失 |
| 长绘制阶段拆分 | 按驱动 | Turnip + Adreno 8xx 为 1 ms，其他非高通原厂驱动为 3 ms，高通原厂关闭；`LARECOMP_VK_SPLIT_US` / `exec.c`，仍保留阶段边界提交与同步 |
| 可变格式颜色图像的视图格式列表 | Turnip + Adreno 8xx 开 | `LARECOMP_VK_FORMAT_LIST=0\|1`；`vk_image_format_lists_default()` / `textures.c` / `image_format_list.h`，使渲染目标和纹理保留 UBWC；存储图像与其他设备不变 |

Turnip 对带 `MUTABLE_FORMAT` 却没有格式列表的颜色图像一律关闭 UBWC；Adreno 7xx gen3/8xx 只要列表中的格式都支持 UBWC 即保留压缩，
旧款 Adreno 遇到混合列表反而会关压缩甚至改线性排布，因此默认只用于 Adreno 8xx。列表包含 `fmt_of()` 在同一兼容类里可能产生的全部格式，
只排除不能压缩的 E5B9G9R9；超出列表的视图会打印 `view format ... outside` 日志，出现时先补列表或回退，不能忽略。

`LARECOMP_VK_SHARED_ALIAS` 默认关闭，对应 `shared_memory.c`，通过 `--shared-alias` 验证。
共享映射最多保留 512 MiB backing，并保留至少一半驱动分配槽；超出时新范围使用原 CPU 路径。

完全相同的纹理描述符默认复用；同步发布时 `LARECOMP_TEXTURE_REUSE=0` 可对照相同注册仍排空的旧行为。
**TotK 1.4.3 + Turnip + Adreno 8xx** 默认同时开启纹理描述符排队发布和异步 GPU buffer-copy；
其他游戏/GPU/驱动保留关闭，条件统一由 `vk_deferred_resources_default()` 判断。
`LARECOMP_TEXTURE_QUEUE` / `--texture-updates queued|sync` 与 `LARECOMP_VK_ASYNC_BUFFER_COPY` /
`--async-buffer-copy on|off` 可分别覆盖。本轮同步对照须同时指定 `--texture-updates sync --async-buffer-copy off`；
若还要恢复重复注册的旧排空行为，再加 `--texture-reuse off`，该开关不改变队列模式。
保留新纹理预留引用、CPU 读写保护、快照及不支持映射的正确回退，不能只启用 GPU 复制而删除它的同步。
`--game-dynres default` 不覆盖普通启动策略；当前运行时默认冻结游戏 GPU 计数器驱动的分辨率变化，
与菜单的内部 Dynamic 分辨率是两条不同路径。`on|off` 仅用于明确的对照。
2026-10-29 发现旧控制器强制 `NO_SCALE=1` 且 `GAME_DYNRES=1`，绕过玩家分辨率并与普通启动不同。
旧结果只能说明原始尺寸负载下的相对表现，不能当作 2× 或正常游玩帧率，见 [诊断与性能口径修正](docs/RELEASE-DIAGNOSTICS-20261029.md)。

运行时编译关闭后使用严格完整准备门禁；部分预热清单不能满足该门禁。
后台顺序是最近使用状态、当前版本 257 个计算输入的默认状态、共享清单；不恢复旧百万级全变体任务。

| 身份/记录 | 约束 |
| --- | --- |
| `GAMELIB_ID` | APK 与生成游戏库 ABI/生成器/编译参数身份；不用于着色器缓存 |
| `SHADER_BUILD_ID` | 翻译/准备相关构建身份，参与准备有效性；与稳定存储身份分开 |
| `SHADER_CACHE_ID` | `shader_cache_contract.cmake` 对六项编译/键/精度/接口/shaderc 输入计算的存储身份 |
| `SHADER_WARM_ID`、profile、输入与清单 | 准备配方/收据身份；单项请求映射也核对实际配置与模块 |
| driver cache / 管线 recipe | 绑定构建、设备/驱动/UUID 与布局状态；不跨驱动无条件复用 |

缓存生成头由 `totk_generate_shader_cache_contract` 统一生成，APK 与独立审计工具共用。
`shader_cache_compat.json` 是显式审核的旧目录白名单，构建不能自动重写批准新兼容性。
同格式可读不等于源码键命中；只在准确键匹配时复用。字典和派生模块必须从同一 namespace 验证。
不伪造准备收据，不把单项映射当完成证明，不为提高命中率对不等价源码建立别名。
准备元数据放应用可写 `cache/shader-warm/<规范化游戏路径摘要>/`，不要求输入游戏目录可写。
损坏/缺失回退、原子落盘、取消/重启和旧缓存保留是必须维护的行为。
接口元数据按完整 NVN 输入、状态、实际译码配置和编译 namespace 匹配；不持久化源码、guest 地址或 Vulkan handle。
元数据独立写入线程最多排队 128 项，每个当前 namespace 最多保留 32,768 个文件；它是可重建的加速层，收据仍只证明所列 SPIR-V 完整。
设备内模块/反射缓存最多保留 128 项、64 MiB SPIR-V；引用保护在途对象，超过保留预算的临时模块不额外留住 SPIR-V。
shaderc、接口派生、shader module 和管线创建共用编译预算。8 核设备加载阶段最多 4 项、普通后台最多 3 项；游玩阶段最多 2 项、普通后台最多 1 项。
前台所需的模块依赖/管线可提升优先级；取消后台依赖时前台接着构建。Maxwell 译码保持串行，不因预算改变而删除其锁。
启动先准备标题与默认计算输入，补计算配方种子，再在加载阶段等待已排队管线最多 15 秒；取消或没有 worker 时立即回到正常首次使用等待。
种子只记录完整源码键、实际 NVN workgroup 或已成功创建管线的固定状态，在当前设备重新创建；不证明覆盖所有场景。
译码/接口键、准备范围、SPIR-V 编译 namespace、管线 recipe ABI 和设备身份分别约束复用；无关的录制/清除/调度改动不使准确映射失效。
实现与本地验收见 [着色器运行时改造](docs/SHADER-RUNTIME-20261030.md)。
完整准备使用同目录的 `full.receipt` 保存准确源码键集合，`full.log` 记录过程；旧 `.shaders-ready` 不再作为门禁。
UI 只查询收据身份，独立可取消任务逐项检查模块和字典；每次启动在设置加载后重新验证。
缓存验证只证明所记录模块完整，不证明覆盖全部运行时状态；默认保留运行时编译回退。

## 8. 代码规范与复用

- 沿用邻近文件的语言和排版，新增块通常四空格；不顺手全文件格式化。C++ 使用现有 C++17，
  Java 遵守当前 17 兼容级别，Python 使用标准库/现有依赖，shell 明确声明 Bash 或 POSIX sh。
- 函数/变量沿用现有 `snake_case`，类型遵循相邻模块；对外接口放对应头文件，内部函数尽量 `static`/内部链接。
  C/C++ 边界明确所有权、长度和 `extern "C"`，不从 C++ 向 C/JNI 边界抛未处理异常。
- 新代码先 `rg` 查正式 helper 和测试入口。优先复用：`totk.py`/`game_profile.h` 输入契约、
  `package_android.py` 身份解析、`totk_romfs.py`、`tegra.c`/`formats.c`、`vertex_format.h`、
  正式 `translate.c`、`shader_interface.cpp`、`pipeline_state.c`、现有缓存/队列/复位 API。
- NVN `SetShaders` 的程序代次与明确写入通知共同参与代码缓存失效；不能用代码地址相同推断内容相同。
  游戏仍须先等待在途 GPU 工作，再修改或重新绑定代码。未经同步/通知的内存改写不属于该保证。
- 正常绘制与预热共用管线创建逻辑；运行时与审计共用解析/身份逻辑。合成测试使用独立期望值和错误样本，
  不把生产算法复制一遍再和自己比较。跨平台确需不同实现时写明语义差异，不强行抽象。
- SHA-256 复用 `third_party/d2r-digest`，上游清单复用 vendored MacNativeGames 工具，
  不再手写第二套摘要、清单解析器或目录扫描器。
- 解析不可信输入先检查范围/溢出、长度、对齐、版本、枚举和分配上限。使用定宽整数表示文件/协议字段；
  不把主机 struct padding、指针或 Vulkan handle 直接当持久化协议。
- 出错必须区分“不支持/可回退”和“状态已损坏”。缓存写入失败可保留本次可用结果；
  不吞掉身份错误，不以空文件、假收据或跳过绘制掩盖失败。
- 日志解释行为与原因，带明确前缀；高频诊断受开关/预算限制。UI 写玩家能理解的选项，不暴露内部调试流程。
  设置项同时维护默认值、读写、UI 和简繁中文翻译，不能只接一个环境变量就宣称用户流程完整。

### 8.1 内存、线程与生命周期

- 明确 CPU guest、渲染线程、翻译/编译 worker、GPU 队列各自的所有权；原子变量用于状态发布，锁用于复合不变量。
  Maxwell 翻译器使用现有串行保护，不因增加工作线程假定它可重入。
- 统一内存只说明物理架构，不代表任意 guest 地址都可直接绑定 GPU；先验证 backing、可见性、范围和完成序号。
  同一页面也需要读写顺序、在途源保护和缓存失效。不能为了零拷贝移除同步。
- 上传缓存按提交、全局失效与主堆写入代次判断有效性。当前 LDG/STG/RED 及其窗口/复制表只寻址主堆，
  全局存储必须使整个主堆的旧上传失效，但不清空堆外顶点上传；明确的 CPU/复制写入仍按重叠范围失效。
  修改着色器可寻址范围时，同时修改缓存失效范围与边界回归，不能只依据绑定的 SBUF 推断实际写入范围。
- 共享映射部分释放保留仍存活页面的连续 CPU 映射；禁止先取消整个映射再恢复邻页。
  资源销毁等待真正使用它的 GPU 工作完成，测试覆盖局部释放、并发读取、失败和重启。
- buffer-copy 的 CPU 读写保护在获取将要使用的 command buffer/render pass **之前**完成；
  等待可能提交并切换命令缓冲。保留提交快照语义和 overlap/不支持情况的正确回退。
- CPU 展开图元索引也必须经过 GPU 写入预检查。QUAD_STRIP 保留顶点对、各段重启/奇数尾与 NVN 平面属性来源；
  调整到 Vulkan 的 provoking vertex 规则，不能只把三角形数量改对。
- CPU 直接读写的全局 arena 要求 HOST_COHERENT；普通非一致读回保留完成等待、主机可见屏障和缓存失效。
- 复制结果直接绑定 UBO 时，预检查必须固定绑定决定，并把读取寿命延长到实际绘制/计算提交；
  复制完成不能解除后续 GPU 读取的 CPU 写入保护。纹理句柄等 CPU 读取单独预检查，
  容量不足回退 CPU 路径，描述符绑定期间不得等待。命令热路径的 pending 判断不查询驱动完成序号。
- 禁止形成“对象锁 → 等渲染队列 → 渲染线程再取对象锁”的环；纹理通知沿用渲染线程登记表和已有通知 API。
- 排队发布纹理描述符时复制视图值并预留新纹理引用。渲染回调只更新槽位和完成链表，不取 NVN 对象锁；
  游戏线程在锁外收集待释放引用。池销毁/重启先排空并收集，不能让引用寿命取决于调用者的临时视图或对象是否仍存在。
- 停止/重启先停止提交，再排空翻译以及它可能新增的编译任务，最后更换 guest 内存。
  先提交并完成尚未提交的 command buffer，再销毁管线、预热产物、程序、布局和 ShaderInfo；清空所有快捷缓存。
  单次游戏状态通过 `RT_ON_RESET` 和已有后端重置入口清理；取消后 join 再释放 worker 依赖。
- Android 生命周期在 SDL event watch 中先发布状态；不能等默认阻塞的 `SDL_PollEvent` 返回后才暂停后台工作。
  watch 中只请求取消，不 join worker。游戏帧边界等待使用 `app_lifecycle`，释放 kernel core-0 所有权；
  teardown 在 join guest 前唤醒等待者。前台恢复、窗口重建和连续切换都需实机验证，不能仅证明画面隐藏。

## 9. 上游、提交和文档

优先查现有上游实现再设计。引用固定 repository/commit/path，说明直接复用还是独立实现；
保留许可证、版权、补丁与校验信息。`third_party/macnativegames/UPSTREAM.json` 的脚本和清单来自同一审核版本，
更新后运行 importer/原始键审计。Metal 状态是语义参考，不能直接当 Vulkan 管线字节或忽略接口类型。
Eden 的 GPL 代码不能直接作为 MIT 代码复制进来；算法参考与实际可分发实现分开记录，详见
[上游关系](docs/UPSTREAM.md)、[上游整合](docs/UPSTREAM-INTEGRATION.md)、[Eden 对应机制](docs/EDEN-INTEGRATION.md)。

只提交源码、合成测试、允许分发的元数据和文档。自己的游戏包/NSO/NCA/RomFS、密钥、字体、存档、生成 C/GLSL/SPIR-V、
原始 GPU 抓取、APK/SO、工具链与私有会话原文不进入 Git。不要全局 `git add .`、`git reset --hard` 或 `git clean -fdx`。
清理明确选中的可再生产物前核对所需输入和证据，保留游戏、存档、已用 APK/库以及既有着色器缓存。

提交前检查 `git diff --check` 和实际待提交差异，按问题划分源码/测试/文档，保留其他任务的未提交部分。
提交消息沿用 `fix(area): ...`、`refactor(area): ...`、`docs: ...` 等现有风格。
PR 写清触发条件、最终行为、为何这样改、实际验证及未覆盖范围；本地构建不等于已安装/发布。

文档默认中文。`Agent.md` 管长期规范，`BUILD.md` 管详细构建/部署，`TESTING.md` 管验证/诊断，
`STATUS.md` 管现状，专题文档保留带日期的证据。改规范同时更新 README/贡献指南入口及受影响的中英文命令；
历史章节加日期/被取代说明，不删除负结果，也不让旧“下一步”自动成为新任务。
