# Zepp API 1.0 通信模块来源

模块取自官方 [Zepp Health Fetch API 1.0 示例](https://github.com/zepp-health/zeppos-samples/tree/main/application/1.0/fetch-api/shared)，
上游记录的下载日期为 2026-10-20。Apache-2.0 许可证保留在 [LICENSE.txt](LICENSE.txt)。
这些 polyfill 和 MessageBuilder 使用旧版 hmBle/messaging API，不导入 API 2/3 模块。

继承的本地修改是 `message.js` 中请求超时与错误监听器的清理。
保留这些模块不表示 TOTK 的手表功能已经适配，范围见 [伴侣说明](../../README.md)。
