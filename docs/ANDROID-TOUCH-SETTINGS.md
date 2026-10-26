# Android 触控与设置

当前界面继承自 BOTWRecompAndroid 和更早的 LASRecompAndroid。设置分类、全宽控件、纵向滚动、
触控布局编辑器及返回按钮由同一套运行时界面提供；本项目尚未完成所有输入流程的实机验收。

同步至上游 `ef7a306` 后包含按手机语言选择界面语言、中文翻译、系统字体兼容及 FPS 风格触控相机。
上游在 POCO 设备上的批准不能视为一加 13 / TOTK 已验收。当前只有实际游戏启动、人物与部分渲染结果得到确认。

触控显示素材与点击区域是不同层；换皮肤不应改变输入语义。旧 BOTW 的 60 FPS、超宽屏、手表地图等入口
即使仍在继承界面中出现，也不代表 TOTK 支持。

相关实现：`runtime/ui/controls_ui.mm`、`touch_controls.mm`、`i18n.mm` 与 `runtime/hle/input.c`。
开发检查可使用 `tools/test_touch_scroll.py`，它测试手势助手，不证明真实布局、Android 输入法或控制器导航正确。

当前限制见 [状态](STATUS.md)，验证方法见 [测试](TESTING.md)，上游来源见 [同步记录](UPSTREAM.md)。
