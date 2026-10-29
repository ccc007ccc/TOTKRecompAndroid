# 参与 TOTKRecompAndroid

简体中文 | [English](CONTRIBUTING.en.md)

本分支开发《王国之泪》Switch 1.4.3 的 Android ARM64 移植。直接上游是
[mamcjr/BOTWRecompAndroid](https://github.com/mamcjr/BOTWRecompAndroid)。
请先阅读统一的 [项目规范](Agent.md)，再按需查阅 [状态](docs/STATUS.md)、[架构](docs/ARCHITECTURE.md) 与 [上游关系](docs/UPSTREAM.md)。

## 提交内容

源代码、合成输入测试和文档可以提交。手写代码与 AI 辅助代码遵循相同验证要求。
文档默认中文；代码注释沿用所在文件风格，解释约束、原因和可验证的行为。

不要提交游戏包、NSO/NCA、RomFS、密钥、从游戏生成的 C/着色器、字体、纹理、音频或存档。
实机原始缓冲与回放材料留在被忽略的 `.work/totk-port/`。问题报告可以附必要截图、短视频和不含私有数据的日志。

## 开发与验证

SteamOS 宿主不可变，编译、依赖安装、生成器与测试使用 `distrobox enter dev`。
从项目根目录加载 `android/dev_env.sh`，临时数据留在项目内，工具链默认位于 `/home/deck/Code/Toolchains/`。
具体命令见 [构建](docs/BUILD.md) 与 [测试](docs/TESTING.md)。

- 保留四模块身份检查、应用数据隔离和已确认的人物修复。
- 不把 BOTW/LAS 的地址、版本或定点补丁直接用于 TOTK。
- 实现变更运行相关检查；图形修复提供实机或独立回放证据，说明覆盖范围。
- 编译通过、少量正常帧和用户确认是不同证据。保留负结果与用户反馈。
- 先从源码和已有抓取定位，必要时才重启；已启用诊断的进程可用 F9 再次抓帧。
- 每轮记录 APK/提交、驱动、环境变量、场景和有效帧范围。诊断回读不能用于性能测试。
- 属于一次启动的状态需随游戏重启清理，参照 `RT_ON_RESET` 与后端重置入口。

## 问题与合并请求

在 [本仓库 Issues](https://github.com/ccc007ccc/TOTKRecompAndroid/issues) 报告设备、Android 版本、GPU/驱动、
游戏版本、复现步骤、预期与实际结果。Android 日志标签是 `TOTKRecompAndroid`。

合并请求写明具体问题、最终行为、测试和未验证部分，保留来源、版权与许可证。
同步上游后检查游戏版本、资源校验、界面文案与渲染计数器；没有文本冲突不代表已经适配 TOTK。
