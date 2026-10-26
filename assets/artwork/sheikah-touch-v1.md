# 触控图集：Sheikah 第一版

保留上游生成的 `sheikah-touch-atlas-v1.png`，1254×1254、RGBA、透明背景。
APK 中的资源名是 `assets/res/sheikah-touch-atlas.png`。运行时按四个象限的 UV 选取图形。
这套皮肤只负责显示，点击范围、布局和手势由触控代码决定。

图集包含圆形按键框、肩键框、摇杆外环和摇杆帽，采用暗青色、古金色与克制的蓝色光线。
每个图形独立放在自己的象限并留安全边距，前三种边框内部透明，不把文字或功能标识烘焙进纹理。

素材来自上游既有设计，本次没有重新生成。完整生成提示词见下方固定上游原文。
TOTK 输入验收范围见 [触控与设置](../../docs/ANDROID-TOUCH-SETTINGS.md)。

[完整上游历史原文](https://github.com/mamcjr/BOTWRecompAndroid/blob/ef7a3062432f33457155020b40595b81c9827a41/assets/artwork/sheikah-touch-v1.md)。原始内容也保留在 Git 历史中。
