# Zepp 伴侣：上游保留实验

本目录来自 BOTWRecompAndroid，包含面向 Amazfit GTR 4 New 的地图 Mini Program、Side Service 与通信代码。
**TOTKRecompAndroid 目前没有完成该功能适配。** 原生 `runtime/watch_map.c` 依据 BOTW 1.0.0 的指令和内存结构，
地图导出工具也沿用 BOTW 资源布局；不能把它们用于当前 1.4.3 的王国之泪。

## 历史实现范围

手机通过认证的 loopback HTTP 服务提供状态，Zepp Side Service 经蓝牙发送给手表。
旧地图支持四级缩放、拖动、重新居中、位置方向和任务标记。API 1.0，历史目标 deviceSource 9437441，480×480。
原 0.2.1 修订处理暂停、恢复、旧回调和 UI 组件复用；其模拟测试不代替实际手表通知场景的验证。

这些版本号和记录属于上游伴侣，不是 TOTK 应用版本或本分支测试结果。
完整安装、导出、协议和历史测试说明保存在下方固定上游原文中；旧预览二维码和到期日期不再作为现行安装指引。

## 后续适配边界

需要先核实 TOTK 的版本契约、地图数据、位置与任务结构，再实现读取和设备验收。
目前主要任务是游戏渲染正确性，不会自动导出游戏地图或发布手表预览。
地图包包含自己的游戏派生资源，不进入 Git；上传到 Zepp 等外部平台需单独授权。

通信模块的来源和 Apache-2.0 许可见 [来源说明](shared/legacy/SOURCE.md) 与同目录 LICENSE.txt。
当前项目状态见 [STATUS](../../docs/STATUS.md)。

[完整上游历史原文](https://github.com/mamcjr/BOTWRecompAndroid/blob/ef7a3062432f33457155020b40595b81c9827a41/companion/zepp/README.md)。原始内容也保留在 Git 历史中。
