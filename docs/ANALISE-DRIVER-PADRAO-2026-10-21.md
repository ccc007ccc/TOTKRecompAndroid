# 上游原厂驱动研究

上游在 POCO F7 Ultra / Adreno 830 对照了高通原厂驱动和 Turnip，研究 guest heap、全局内存复制及渲染开销。当前一加 13 的独立回放与用户反馈应单独记录，不能套用其 FPS 或“画面正确”结论。

## 当前项目入口

- [TOTKRecompAndroid 首页](../README.md)
- [状态与已知问题](STATUS.md)
- [构建与部署](BUILD.md)
- [上游关系与致谢](UPSTREAM.md)

## 原始资料

[固定上游提交中的完整原文](https://github.com/mamcjr/BOTWRecompAndroid/blob/ef7a3062432f33457155020b40595b81c9827a41/docs/ANALISE-DRIVER-PADRAO-2026-10-21.md)。原文及原始引用也保留在本仓库 Git 历史中。

此页作为中文历史索引，原文的语言、作者声明、实验边界与许可证未改写；其结论不自动成为本分支的功能承诺。
