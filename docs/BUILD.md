# 构建与部署

简体中文 | [English](en/BUILD.md)

本指南针对 TOTKRecompAndroid 的 Switch 1.4.3 输入。功能边界见 [当前状态](STATUS.md)。

## 构建

SteamOS 宿主只作文件管理，构建在 Distrobox **dev** 内完成。默认 SDK/NDK 位于 `/home/deck/Code/Toolchains/`；可以用 `ANDROID_HOME`、`ANDROID_NDK` 覆盖。工作目录是项目内的 `.work/totk-port/`，编译器、Python 和 Java 的临时目录也指向这里。

```bash
bash android/build_totk.sh
```

默认构建 Debug/dev，完整包目录为 `.work/totk-port/packages/debug-dev/`，APK 名称为 `TOTKRecompAndroid-debug-dev-arm64-v8a.apk`。Debug 包名 `com.totkrecomp.android.debug`，名称带 Debug，图标带蓝色 DBG 标记；Release 使用 `com.totkrecomp.android` 和原图标。Java/JNI 源目录名称保留。APK 不包含游戏代码。

需要 Java 21、Android SDK、NDK r30、CMake 4.1.2 及常规 C/C++ 构建工具。脚本创建项目内 Python 环境并安装 Capstone、Unicorn 和 LZ4；不应使用宿主包管理器安装依赖。本文的 hactool 路径仅为示例，应替换为工具链目录中的实际可执行文件。

### 编译档

| `--profile` | 运行时及原生 hooks | 所有生成游戏单元 |
| --- | --- | --- |
| `dev` | O2 | O1，允许冷函数/巨大函数 O0 策略 |
| `performance` | O3 + ThinLTO | O3 + ThinLTO，禁止 O0 例外和 optnone |
| `snapdragon-8elite` | O3 + ThinLTO，ARMv8.7-A + crypto + FP16，Oryon 调优 | 同一组参数，巨大函数也完整优化 |

`release` 默认选择 `performance`；单独指定性能 profile 默认选择 Release。`snapdragon-8elite` 使用 `-march=armv8.7-a+crypto+fp16 -mtune=oryon-1`；NDK r30 的 Clang 21 没有独立的第二代 Oryon 调度模型。性能档保留 `-fno-fast-math`，维护游戏的浮点语义。

```bash
bash android/build_totk.sh release --profile snapdragon-8elite
bash android/build_gamelib.sh .work/totk-port/game/exefs release --profile snapdragon-8elite
```

APK、从 APK 提取的精确 `libmain.so`、游戏库及侧车文件分别放入 `.work/totk-port/packages/<variant>-<profile>/`。`build.json` 记录 APK/运行时 SHA256，以及从 stripped ELF 导出符号读取的实际 profile 和 `GAMELIB_ID`。打包与部署还从实际 APK manifest 核对包名，拒绝过期元数据、错误包名及 profile/身份不匹配的组合。

需要独立性能测试包时，两条构建命令都加 `--perftest`，目录追加 `-perftest`，包名固定为 `com.totkrecomp.android.perftest`。它的 Release 保留 Debug 签名和 `run-as`，原生 CMake 仍为 Release/O3；普通 Release 不开启调试标记。`push_game.sh --perftest` 会明确选择该包。更换 profile 必须同时更新 APK 和游戏库。

资源紧张时先在 dev 内设置 `JOBS=2 TOTK_LTO_JOBS=1`。原生 Ninja 编译池也受 JOBS 限制，链接单任务执行。可通过 `TOTK_BUILD_LAUNCHER` 为编译器/链接器设置本机资源上限。NDK 的 LICM promotion 修正同时作用于性能档前置编译和 ThinLTO 后端，参数纳入库身份。手机 Zig 参数另行转换：ARM ISA 使用 feature set，Oryon tuning 直接传给 Clang；链接以 `-O3 -flto=thin` 选择实际 LLD O3，不传 Zig 不支持的 LLD CLI 选项。

## 准备自己的游戏

以下命令在 `distrobox enter dev` 后、项目根目录执行。`android/dev_env.sh` 设置工具链、Python 和项目内临时目录。NSP 提取使用本机 hactool 1.5.4；通过 `--hactool` 指定路径。

```bash
source android/dev_env.sh
export ANDROID_SERIAL="替换为 adb devices -l 显示的一加设备序列号或无线地址"
"$PYTHON" tools/pull_totk.py \
  '/storage/emulated/0/Games/TotK/[APP][0100F2C0115B6000][1.0.0][US][16.0.0].nsp' \
  .work/totk-port/base --serial "$ANDROID_SERIAL"
"$PYTHON" tools/pull_totk.py \
  '/storage/emulated/0/Games/TotK/[UPD][0100F2C0115B6800][1.4.3][US][21.2.0].nsp' \
  .work/totk-port/update --serial "$ANDROID_SERIAL"
"$PYTHON" tools/prepare_totk.py \
  --base .work/totk-port/base --update .work/totk-port/update \
  --keys .work/totk-port/private/prod.keys --hactool /home/deck/Code/Toolchains/hactool/hactool \
  --output .work/totk-port/game
```

设备序列号用 `adb devices -l` 确认。`prod.keys` 使用自己设备上的文件；准备工具从自己的 NSP ticket 读取标题密钥，兼容副本在结束时清理，不改动原密钥。手机 `Games/TotK/` 的原始文件只读。

提取结果为 `game/exefs/`、合并后的 `game/romfs.bin`、独立视频 `game/romfs/` 和输入清单。流式读取 NSP 避免在电脑额外保存一份本体 NSP，但仍需要容纳约 16 GiB NCA、约 16 GiB RomFS 和构建产物的空间。

```bash
GAMELIB_GEN="$PWD/.work/totk-port/generated" \
bash android/build_gamelib.sh .work/totk-port/game/exefs debug --profile dev

"$PYTHON" tools/deploy_totk.py .work/totk-port/game \
  --library .work/totk-port/packages/debug-dev --serial "$ANDROID_SERIAL" \
  --apk .work/totk-port/packages/debug-dev/TOTKRecompAndroid-debug-dev-arm64-v8a.apk --launch
```

部署通过正常 APK / Activity / JNI 启动，不在手机 `/data/local/tmp` 放辅助程序。运行数据位于所选包的 `Android/data/<package>/files/`，三种包的存档分开。尚未打包端侧编译器；本流程在电脑生成游戏库。端侧构建服务与 profile 参数已适配，参数经过同版 Zig 跨编译检查，完整手机编译仍待验收。

## 验证与清理

```bash
source android/dev_env.sh
"$PYTHON" tools/test_totk.py
"$PYTHON" tools/test_exclusive_pair.py
"$PYTHON" tools/test_local_branches.py
"$PYTHON" tools/totk.py .work/totk-port/game/exefs
```

`.work/` 全部被 Git 忽略，包含本机私有输入和日志。完成部署后可删除 `.work/totk-port/base/`、`update/`、`generated/` 与 `out/gamelib-build/` 释放空间；保留 `game/`、`out/totkgame.so*` 与日志便于后续复现。不要把密钥、游戏数据或派生代码加入 Git。

历史工具和上游资料的范围见 [上游关系](UPSTREAM.md)，不能将其版本号、地址和性能结论套用到 TOTK。

## 已部署游戏后的运行时更新

修改 `runtime/` 通常只需重建 APK，不必重新生成庞大的游戏库：

```bash
distrobox enter dev -- bash <<'SH'
set -e
source android/dev_env.sh
cd android/botwrecompandroid
bash gradlew --no-daemon --max-workers=2 assembleDebug
SH
```

在 `dev` 内设置目标设备后，用 `adb -s "$ANDROID_SERIAL" install --streaming -r` 安装新的 APK。
`android/run_boot.sh` 会停止并重新启动应用，抓取限时日志后保留游戏运行；只在需要新版本或明确对照时使用。
设备列表可能同时包含其他设备，每次部署都必须指定正确目标。

只有 dev 游戏库采用 O1/巨大函数 O0；`GAMELIB_FAST_COMPILE=ON` 只允许用于 dev 排错。性能档拒绝该开关及任何额外 O0 名单。NDK 版本、依赖或生成器变化后，需按脚本输入签名重新构建相应产物。端侧续传签名检查编译/链接参数及全部四个模块的完整 32-byte build ID。

NDK r30 的巨型控制流编译问题和已验证的例外见[手机性能测试](PHONE-PERFORMANCE.md)。
`GAMELIB_DISABLE_LICM_PROMOTION` 默认 ON；`GAMELIB_EXTRA_HEAVY_SOURCES` 只在 dev 中接受额外 O0 单元，默认空。
这些是构建参数，必须记录在性能结果中；不要用全量 O0 代替性能构建。

清理前保留所需输入、游戏库、存档和有效实验日志。不要为腾空间删除尚未分析的抓取，或删除手机原始 `Games/TotK/`。
