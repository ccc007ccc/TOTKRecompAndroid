# 项目文档

简体中文 | [English](en/README.md)

现行指南针对 **TOTKRecompAndroid / Switch 1.4.3**，默认中文，现行指南同时提供 [英文版](en/README.md)。
直接上游是 [mamcjr/BOTWRecompAndroid](https://github.com/mamcjr/BOTWRecompAndroid)。

| 文档 | 内容 |
| --- | --- |
| [项目规范](../Agent.md) | 统一环境、编译档位、操作流程、代码和协作要求 |
| [2026-10-29 整理基线](PROJECT-BASELINE.md) | 着色器/内存两条工作线的最新范围、已实现内容与遗留项 |
| [状态与已知问题](STATUS.md) | 用户确认、当前限制和下一步 |
| [构建与部署](BUILD.md) | Distrobox、游戏输入、APK、游戏库与手机部署 |
| [运行时架构](ARCHITECTURE.md) | 模块、服务、图形和文件布局 |
| [测试与诊断](TESTING.md) | 本机检查、独立回放、实机抓取与证据范围 |
| [手机测试会话](PHONE-PERFORMANCE.md) | 独立测试包、自动读档、计时退出与历史测量 |
| [着色器均衡准备](SHADER-BALANCED.md) | 当前清单、编译、缓存复用、后台与管线准备 |
| [着色器链路审查](SHADER-CHAIN-REVIEW.md) | 全链路检查、已修复问题、旧完整门禁缺口与验证范围 |
| [着色器文件关联](SHADER-ASSET-LINKS.md) | 原生目录、材质/程序归属审计 |
| [历史完整准备](SHADER-PREPARATION.md) | 已被均衡方案替代的整库目标、严格门禁和历史证据 |
| [上游整合](UPSTREAM-INTEGRATION.md) / [Eden 对应机制](EDEN-INTEGRATION.md) | 已适配行为、来源和未完成项 |
| [适配记录](TOTK-PORT.md) | 完整调查与实验记录 |
| [上游关系](UPSTREAM.md) | Fork 来源、同步版本和历史资料 |
| [触控与设置](ANDROID-TOUCH-SETTINGS.md) | 继承界面与本项目验收范围 |
| [Zepp 伴侣](ZEPP-COMPANION.md) | 保留的 BOTW 实验代码及 TOTK 限制 |
| [贡献指南](../CONTRIBUTING.md) | 代码、文档、测试和反馈要求 |

旧 BOTW 性能报告、驱动研究与 `las/` 页面保留原路径作为中文历史索引，并链接固定提交的上游原文。
原文中的设备、版本、地址和性能数据不构成本项目的支持承诺。第三方许可证和供应商文档保持原文。
长期规范以 `Agent.md` 为入口；专题的历史“下一步”不自动覆盖当前任务范围。
