# 字体

游戏的 Switch 系统字体不包含在游戏程序中，也不随本仓库分发。
当前 Android 运行时的系统字体替代由 `runtime/hle/pl.c` 提供，默认可使用 APK 中的 DejaVu Sans。
应用菜单另由 `runtime/ui/i18n.mm` 选择和合并可读的系统字体；菜单字体与游戏系统字体不是同一条路径。

如需使用自己主机导出的字体，可在运行数据根目录的 `fonts/` 中放置：

- `nintendo_udsg-r_std_003.bfttf`，也接受对应 `.ttf` 或 `nintendo_udsgr_std_003` 拼写。
- `nintendo_ext_003.bfttf` 或 `.ttf`，可选按钮字形。

主机系统归档分别为 FontStandard（`0100000000000811`）与 FontNintendoExtension（`0100000000000810`）。
`LARECOMP_FONT=<文件>` 可指定 `.ttf`、`.otf`、`.ttc` 或 `.bfttf`；实际解析器能否读取仍需验证，
不能保证任意 CFF/OpenType 字体与游戏字体引擎兼容。中文游戏 UI 已出现，不代表所有语言和字形均完成验收。

本目录除本说明外均被 Git 忽略。请勿提交或分发任天堂字体。
