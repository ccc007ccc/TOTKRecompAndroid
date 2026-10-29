# 2026-10-29 草地交互修复与音频核对

基线 `7ec7749`。用户补充：草的位置不对，挥剑有割草特效但草不消失，随后可能随机整片消失；
UI 闪烁目前似乎已恢复，声音正常。修复后的实机测试期间，用户反馈“草好了”。

## 割草绘制的点尺寸上限

割草并不是只播放粒子：游戏还向 512×512 RGBA8 交互贴图绘制点，记录草被切割的范围。
这些顶点着色器读取驱动常量 `c0[0x610]`，将点尺寸限制在驱动允许的最大值。
本项目此前没有填这个字段，它一直是零；相关绘制不能形成应有的覆盖范围，交互贴图保持未割状态。

`cbuf_info` 现在为顶点阶段填入 `min(2047.9375, Vulkan pointSizeRange[1])`，
保留 NVN 的上限并遵守实际设备能力。参考 MacNativeGames 固定提交
[`42273782` 的 `driver_bank_fill`](https://github.com/ly05010419/MacNativeGames/blob/4227378207c7a8dd973d0de05642eeda0d9ca0b4/games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m#L9156)。
运行时已有 `largePoints` 能力启用路径，游戏库和着色器源码不需要重编。

一加 13 / PJZ110 / Adreno 830 / Turnip Mesa 27.0.0-devel（`ab1502fd16`）验证：

- 修复前，草读取的 256×256 交互贴图在挥剑后仍为全白。
- 修复后，缩略至 128×128 的贴图中，挥剑前红通道全为 1；挥剑后有 625 个采样点低于 0.99，
  出现实际切割区域。画面保留周围的草，用户同时反馈恢复。
- 运行至第 2500 帧保存诊断窗口后自行退出，总计约 102 秒。这是画面诊断，不是帧率基准。

## 随机消失的证据与范围

修复前取得了两组同进程的草消失前后捕获，不再沿用较短路线的“未复现”结论。
第二组中草的绘制命令仍存在，但顶点阶段读取的一张 260×260、9 mip 的 RG8 地形法线贴图，
从旧地址的有效 GPU 内容切换到新地址的全零副本；其余七张输入仍有内容。
新地址的 CPU 镜像也为零，旧纹理已 finalized。具体的资源更新操作尚未确定，不能仅凭地址变化认定是哪一种复制出错。

点尺寸修复后的这次短测中，地形法线仍有内容，草没有整片消失。用户反馈当前草已恢复；
旧捕获保留供问题再次出现时比较，不把这段短测扩大成长时间、跨区域或全部草地的验收。

已核对的上游机制也保留实际边界：128×128 的割草读回图已经走本项目的线性纹理写回路径；
256×256 交互采样器在此设备上支持自定义边界色，实测为 `(0.5,0.5,0.5,0.5)`。
因此本次没有把写回阈值或采样边界当作已经证实的缺陷。

新增可选输入诊断见 [测试指南](TESTING.md)：指定顶点着色器后，保留 GPU 输入缩略图、CPU 存储镜像、
纹理重叠关系、采样边界色；沿用原有数据预算，只在明确开启时读取。

## 音频日志的含义

上一轮出现一次 `AAUDIO_ERROR_DISCONNECTED`，随后打印 `Low latency audio enabled`。
源码核对表明两条都来自 SDL 3.4.16 的 AAudio 后端：`AAUDIO_PlayDevice` 收到错误后调用
`RecoverAAudioDevice`，由 `BuildAAudioStream` 重建输出。低延迟日志在打开流之前打印，
证明发生了恢复尝试，本身不证明玩家听到了断音，也不能单独证明恢复成功。
用户确认当前声音正常，因此不把它列为已确认的当前音频故障；本次没有改动音频实现。

参考的 [MacNativeGames 音频实现](https://github.com/ly05010419/MacNativeGames/blob/4227378207c7a8dd973d0de05642eeda0d9ca0b4/games/totk/runtime/audio_mixer.c)
使用 CoreAudio 输出、20 ms 预缓冲、欠载后重新预填、积压裁减和 iOS 输出重启，
并在硬件布局含中央/LFE 通道时采用 5.1，否则使用立体声。
这些输出策略与平台恢复机制分别核对；Android 当前通过 SDL/AAudio 输出并已有重建路径。

## 验证与产物

- `tools/test_shader_driver_constants.py` 在 ASan/UBSan 下通过：NVN 点尺寸、设备上限、阶段隔离、
  存储缓冲表、计算派发尺寸。把相同测试用于修改前的生产代码，点尺寸断言失败。
- Release / snapdragon-8elite / perftest 和 Debug / dev 构建通过。
- 实机 APK SHA-256：`eb695b2b04fe83fc5e16108a51a55dd2313ce5f6cf23405cc7af842332470a09`。
  `GAMELIB_ID=993c0a5c82dc5099`，复用既有 O3 游戏库
  `5f3156b208076afa8b07d831d538977e643083ed6014bfd8711760d5137f087c`。
- Debug APK SHA-256：`cef0f395eb591485fe3d2bee26b4270bee192e2f93e1a6ffe49c2d8cc94e62d1`，只作构建检查。

全部构建、测试和 ADB 在 `dev`；仅操作 perftest，完整克隆存档，复用缓存，两个实验内存开关保持关闭。
私有证据在 `.work/grass-audio-20261029/`：`loading-cut-baseline-*`、`loading-cpu-gpu-inputs-*`、
`loading-point-limit-*`、`package-point-limit/` 及构建/测试日志。截图、游戏着色器、输入和 APK 不提交 Git。
