# TOTKRecompAndroid

简体中文 | [English](README.en.md)

面向 Android ARM64 的《塞尔达传说：王国之泪》静态重编译实验移植，当前适配 **Nintendo Switch 1.4.3**。

本仓库是 **[mamcjr/BOTWRecompAndroid](https://github.com/mamcjr/BOTWRecompAndroid) 的 Fork**，现已更名为
[ccc007ccc/TOTKRecompAndroid](https://github.com/ccc007ccc/TOTKRecompAndroid)。我们沿用上游的重编译器、
Android 应用框架和 Vulkan 运行时，开发目标改为《王国之泪》。上游《旷野之息》的兼容性、版本号和性能记录
不代表本分支的结果。项目文档默认中文，来源与同步记录见 [上游关系](docs/UPSTREAM.md)。

## 当前状态

已在一加 13（Adreno 830）进入“海拉鲁城堡的地下”，显示人物、场景材质和中文游戏 UI。
用户已确认人物模型恢复正常；使用 Banner Turnip 27 A8xx 与采样补偿后，瘴气棋盘格消失。

补齐 NVN 条件渲染后，针对火把光晕截断和大面积场景闪烁的本轮修复也已获用户确认。
**项目仍处于开发阶段**：音频、存档事务、输入全流程、长时间运行及性能仍需验收。已接入 30／45／60／120 FPS 计时，提供运行时与全部游戏单元 O3／ThinLTO 的性能档；目标档位与实际帧率分别验证。O0／O1 对照结果见 [手机性能测试](docs/PHONE-PERFORMANCE.md)。
最新结论见 [状态与已知问题](docs/STATUS.md)，完整实验见 [适配记录](docs/TOTK-PORT.md)。

## 构建与运行

需要自己的游戏本体、1.4.3 更新及密钥。仓库和 APK 均不包含游戏程序、资源或密钥。
四个 NSO 模块的完整 Build ID 与 NPDM 标题 ID 会被核对；目前不宣称支持其他游戏版本。

SteamOS 宿主上的编译、依赖安装、生成器和测试统一在 Distrobox `dev` 中运行：

```bash
bash android/build_totk.sh
```

该脚本构建运行时 APK。首次准备游戏还需提取输入、生成并编译 `totkgame.so`、部署到手机，
完整步骤见 [构建与部署](docs/BUILD.md)。之后只修改运行时通常只需重建 APK。

| 项目 | 当前值 |
| --- | --- |
| 游戏 | 《王国之泪》Switch 1.4.3，程序 ID `0100F2C0115B6000` |
| 应用 | Release：`com.totkrecomp.android`；Debug 追加 `.debug`；独立测试包追加 `.perftest`；Android 11+、ARM64 |
| 游戏库 | `totkgame.so`，四模块 ABI 2 |
| APK | `.work/totk-port/packages/<variant>-<profile>/`，dev／performance／snapdragon-8elite 三档 |
| 本机工作数据 | `.work/totk-port/`，不进入 Git |
| 手机数据 | `Android/data/<所选包名>/files/` |

`android/botwrecompandroid/`、Java/JNI 类名及 `LARECOMP_*` 环境变量保留上游技术名称，
不代表应用仍以 BOTW 为目标。部分继承界面和工具还没有完成 TOTK 适配，以文档列明的流程为准。

## 文档与参与

- [文档索引](docs/README.md)：构建、架构、测试、已知问题及历史资料。
- [贡献指南](CONTRIBUTING.md)：修改范围、验证要求和问题反馈。
- [问题反馈](https://github.com/ccc007ccc/TOTKRecompAndroid/issues)：请附设备、驱动、复现步骤和日志。

## 来源与致谢

感谢 [mamcjr/BOTWRecompAndroid](https://github.com/mamcjr/BOTWRecompAndroid) 提供直接上游，
[peiikai/LASRecomp](https://github.com/peiikai/LASRecomp) 提供重编译器与原始运行时，
[mamcjr/LASRecompAndroid](https://github.com/mamcjr/LASRecompAndroid) 提供 Android 移植基础。
本次适配还参考本地 `TotK-Native` 的四模块装载、NVN 和服务实现，以及
[Ryujinx Neo 的 Turnip gather 修正](https://github.com/Sunshinin11/Ryujinx_Neo/commit/734fe55f8f7e2d2ae69449374476e360521e4d51)。
完整依赖和继承致谢见 [来源记录](docs/UPSTREAM.md)。

仓库保留原有 [MIT 许可证](LICENSE) 及第三方组件各自的许可证与版权声明。
这些许可证不授权分发任天堂的游戏、字体或其他资源。本项目不是任天堂官方产品。
