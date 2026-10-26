# TOTKRecompAndroid

面向 Android ARM64 的《塞尔达传说：王国之泪》静态重编译适配，基于本仓库原有的 BOTWRecompAndroid 运行时。当前固定输入为 **Switch 本体 + 1.4.3 更新**（程序 ID `0100F2C0115B6000`）。

适配仍处于启动验证阶段，不能把生成代码或 APK 构建成功理解为已经可玩。实机结果与剩余问题记录在 [移植记录](docs/TOTK-PORT.md)。

程序、资源、密钥和生成代码均不随仓库分发。`tools/totk.py` 核对四个模块的完整 Build ID 和 NPDM 标题；运行时还会核对 ExeFS 与 `totkgame.so` 的模块身份。其他游戏或混用更新版本会被拒绝。

## 构建

SteamOS 宿主只作文件管理，构建在 Distrobox **dev** 内完成。默认 SDK/NDK 位于 `/home/deck/Code/Toolchains/`；可以用 `ANDROID_HOME`、`ANDROID_NDK` 覆盖。工作目录是项目内的 `.work/totk-port/`，编译器、Python 和 Java 的临时目录也指向这里。

```bash
bash android/build_totk.sh
```

APK 输出在 `android/botwrecompandroid/app/build/outputs/apk/debug/app-debug.apk`。安装包名为 `com.totkrecomp.android`；Java/JNI 源目录名称保留以减少迁移改动。需要网络下载固定版本的开源构建依赖。APK 不包含游戏代码。

## 准备自己的游戏

以下命令在 `distrobox enter dev` 后、项目根目录执行。`android/dev_env.sh` 设置工具链、Python 和项目内临时目录。NSP 提取使用本机 hactool 1.5.4；通过 `--hactool` 指定路径。

```bash
source android/dev_env.sh
export ANDROID_SERIAL=3be2985e
"$PYTHON" tools/pull_totk.py \
  '/storage/emulated/0/Games/TotK/[APP][0100F2C0115B6000][1.0.0][US][16.0.0].nsp' \
  .work/totk-port/base --serial "$ANDROID_SERIAL"
"$PYTHON" tools/pull_totk.py \
  '/storage/emulated/0/Games/TotK/[UPD][0100F2C0115B6800][1.4.3][US][21.2.0].nsp' \
  .work/totk-port/update --serial "$ANDROID_SERIAL"
"$PYTHON" tools/prepare_totk.py \
  --base .work/totk-port/base --update .work/totk-port/update \
  --keys .work/private/prod.keys --hactool .work/hactool/hactool \
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

历史 BOTW 文档和尚未适配的实验工具保留在 `docs/`、`tools/` 中；它们的旧版本号、地址和性能结论不适用于 TotK。
