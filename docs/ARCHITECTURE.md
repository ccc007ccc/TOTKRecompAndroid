# 运行时架构

本项目沿用 [BOTWRecompAndroid](https://github.com/mamcjr/BOTWRecompAndroid) 的 AArch64 → C 静态重编译器
和 Android/Vulkan 运行时。自己的 Switch 1.4.3 程序经 NDK 编译为游戏库，资源从合并后的 RomFS 读取。

```text
+ 本体 NSP + 1.4.3 更新 + 自己的密钥
  → 流式提取、BKTR 合并、输入身份校验
  → ExeFS + romfs.bin + 单独的视频文件
  → AArch64 生成 C → NDK 编译 totkgame.so
  → Activity/JNI → 装载四模块 → 游戏
```

| 模块 | 生成时地址 | 用途 |
| --- | --- | --- |
| main | `0x7000000000` | 游戏主程序 |
| sdk | `0x7004000000` | SDK 与 HLE 接口 |
| subsdk0 | `0x7006000000` | multimedia，不能省略 |
| rtld | `0x7007000000` | 动态链接模块 |

运行时选择可用 guest 地址窗口，重定位模块与分派表。游戏库 ABI 为 2，四模块完整 Build ID 与 ExeFS 比对。
版本契约位于 `runtime/game_profile.h` 与 `tools/totk.py`，需同步维护。

| 路径 | 职责 |
| --- | --- |
| tools/gen.py、lifter.py、nso.py | 指令翻译、控制流、NSO 读取与代码生成 |
| tools/pull_totk.py、prepare_totk.py、totk_romfs.py | 提取、合并更新、输入检查和视频导出 |
| runtime/loader.c、gamelib.c、dispatch.c | 模块装载、游戏库契约和分派 |
| runtime/kernel.c、services/ | Switch 内核与服务替代实现 |
| runtime/hle/ | SDK 替代函数；游戏专用 hook 按版本隔离 |
| runtime/nvn/cmdbuf.c、objects.c | NVN 命令记录、对象与纹理布局 |
| runtime/nvn/maxwell/translate.c | Maxwell 着色器翻译 |
| runtime/nvn/vulkan/ | Vulkan 资源、同步、绘制与诊断 |
| runtime/ui/ | 启动器、设置、触控和性能叠层 |
| android/ | Gradle APK、依赖和 NDK 游戏库构建 |

NVN 55.15 控制块决定着色器代码和常量范围。Maxwell 经 GLSL/SPIR-V 进入 Vulkan。
Turnip 可把 guest heap 映射为 GPU 内存；高通原厂路径使用复制和地址表。超过存储缓冲范围的 heap 使用窗口。

纹理路径处理 Tegra 排布、mip/layer、格式、CPU 上传和 GPU 所有权。颜色、深度、采样副本及不同分辨率目标
需分别核对；同一 guest 地址也可能只是不同阶段的临时内存复用。当前并非完整 NVN 实现，缺口见 [状态](STATUS.md)。

包名为 `com.totkrecomp.android`，数据与 BOTW 应用隔离。Java/JNI、部分目录和 `LARECOMP_*` 保留历史命名。
本机私有输入、生成代码、GPU 原始数据与构建缓存都保存在 `.work/totk-port/`。

本地 `TotK-Native` 提供四模块、服务和 NVN 语义参考，本分支没有改用其指令改写执行器。
参考工程、Neo 和上游 BOTW 的验证结论都需在本项目重新核验。
