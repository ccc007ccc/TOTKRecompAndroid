# 构建与部署

简体中文 | [English](en/BUILD.md)

本指南针对 TOTKRecompAndroid 的 Switch 1.4.3 输入。功能边界见 [当前状态](STATUS.md)。

## 构建

SteamOS 宿主只作文件管理，构建在 Distrobox **dev** 内完成。默认 SDK/NDK 位于 `/home/deck/Code/Toolchains/`；可以用 `ANDROID_HOME`、`ANDROID_NDK` 覆盖。工作目录是项目内的 `.work/totk-port/`，编译器、Python 和 Java 的临时目录也指向这里。

```bash
bash android/build_totk.sh
```

APK 输出在 `android/botwrecompandroid/app/build/outputs/apk/debug/app-debug.apk`。安装包名为 `com.totkrecomp.android`；Java/JNI 源目录名称保留以减少迁移改动。需要网络下载固定版本的开源构建依赖。APK 不包含游戏代码。

需要 Java 21、Android SDK、NDK r30、CMake 4.1.2 及常规 C/C++ 构建工具。脚本创建项目内 Python 环境并安装 Capstone、Unicorn 和 LZ4；不应使用宿主包管理器安装依赖。本文的 hactool 路径仅为示例，应替换为工具链目录中的实际可执行文件。

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
GAMELIB_OUT="$PWD/.work/totk-port/out" \
bash android/build_gamelib.sh .work/totk-port/game/exefs

"$PYTHON" tools/deploy_totk.py .work/totk-port/game \
  --library .work/totk-port/out --serial "$ANDROID_SERIAL" \
  --apk android/botwrecompandroid/app/build/outputs/apk/debug/app-debug.apk --launch
```

部署通过正常 APK / Activity / JNI 启动，不在手机 `/data/local/tmp` 放辅助程序。运行数据位于 `Android/data/com.totkrecomp.android/files/`，与其他应用的存档分开。尚未打包端侧编译器；本流程在电脑生成游戏库。

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
bash gradlew --no-daemon --max-workers=4 assembleDebug
SH
```

在 `dev` 内设置目标设备后，用 `adb -s "$ANDROID_SERIAL" install --streaming -r` 安装新的 APK。
`android/run_boot.sh` 会停止并重新启动应用，抓取限时日志后保留游戏运行；只在需要新版本或明确对照时使用。
设备列表可能同时包含其他设备，每次部署都必须指定正确目标。

默认游戏库使用 `-O1`，超大跳转表函数单独 `-O0`。`GAMELIB_FAST_COMPILE=ON` 会把全部函数设为 `-O0`，
适合定位启动问题，不适合性能验收。NDK 版本、依赖或生成器变化后，需按脚本输入签名重新构建相应产物。

清理前保留所需输入、游戏库、存档和有效实验日志。不要为腾空间删除尚未分析的抓取，或删除手机原始 `Games/TotK/`。
