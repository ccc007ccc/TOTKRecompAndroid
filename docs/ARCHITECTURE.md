# 运行时架构

简体中文 | [English](en/ARCHITECTURE.md)

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

NVN 55.15 控制块决定着色器代码和常量范围。GPU 着色器采用[缓存优先的均衡准备方向](SHADER-BALANCED.md)，
默认允许缺失时编译；文件设置和正常启动先准备标题，后台再处理最近状态、默认计算输入和共享清单。
Android 默认 FP32 优先，保留 GPU 整数双精度选项。关闭运行时编译才启用严格完整准备门禁，部分预热不能伪造完成标记。

| 着色器/管线模块 | 职责 |
| --- | --- |
| `shader_archive.hpp` / `shader_catalog.hpp` / `shader_material.hpp` | 有界读取原生目录、程序与材质关系；[关联审计](SHADER-ASSET-LINKS.md)使用同一解析路径 |
| `shader_compiler.c` / `shader_interface.cpp` | `libshaderprepare.so` 编译插件，处理编译与接口特殊化；缓存命中无需打开编译器 |
| `shaders.c` / `shader_cache_contract.cmake` | SPIR-V 内容键、压缩/字典缓存、稳定编译器身份、审核过的旧目录兼容 |
| `shader_warm*` / `shader_prepare*` | 清单规划、单项请求复用、收据、队列和前后台/重启生命周期 |
| `pipeline_state.c` / `pipeline_warm.c` / `pipeline_cache.c` | 正常与预热共用管线创建、真实状态记录和绑定设备身份的驱动缓存 |

已有固定存档启动、缓存复用及独立 GPU 检查；全冷缓存、跨区域路线与全游戏画面尚未验收。
旧全量展开的数量和存储测量保留在[历史准备记录](SHADER-PREPARATION.md)，不作为当前启动要求。

支持 placed mapping 的驱动可让 guest heap 与 GPU 共享页面；不支持的路径保留复制和地址表，超出存储缓冲范围时使用窗口。
`shared_memory.c` 为堆外 alias 资源提供实验性共享 backing；`buffer_copy.c` 为复制提供 GPU 排序和 CPU 访问保护，
两项默认关闭。纹理布局转换和内存可见性仍各自处理；统一内存不等于可以省去所有复制或同步。
这两个实验模块留在本机性能工作区，未随本次着色器提交合入；共用 CPU 写入失效通知已包含在提交中。
默认开关、线程/锁和释放规则见 [项目规范](../Agent.md)，阶段状态见 [整理基线](PROJECT-BASELINE.md)。

纹理路径处理 Tegra 排布、mip/layer、格式、CPU 上传和 GPU 所有权。颜色、深度、采样副本及不同分辨率目标
需分别核对；同一 guest 地址也可能只是不同阶段的临时内存复用。当前并非完整 NVN 实现，缺口见 [状态](STATUS.md)。

包名为 `com.totkrecomp.android`，数据与 BOTW 应用隔离。Java/JNI、部分目录和 `LARECOMP_*` 保留历史命名。
本机私有输入、生成代码、GPU 原始数据与构建缓存都保存在 `.work/totk-port/`。

本地 `TotK-Native` 提供四模块、服务和 NVN 语义参考，本分支没有改用其指令改写执行器。
参考工程、Neo 和上游 BOTW 的验证结论都需在本项目重新核验。
