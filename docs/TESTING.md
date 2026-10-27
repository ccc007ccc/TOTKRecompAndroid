# 测试与诊断

简体中文 | [English](en/TESTING.md)

在项目根目录进入 `distrobox enter dev` 并执行 `source android/dev_env.sh`。
生成器、编译器、Python 和 Java 的临时目录统一留在 `.work/totk-port/`。

## 常用检查

```bash
"$PYTHON" tools/test_totk.py
"$PYTHON" tools/test_romfs_build.py
"$PYTHON" tools/test_exclusive_pair.py
"$PYTHON" tools/test_local_branches.py
"$PYTHON" tools/test_shader_control_flow.py
```

这些检查分别覆盖输入契约／流式提取、RomFS 构建、成对独占指令、局部跳转和着色器控制流。
着色器测试还需要 `cc`、`glslangValidator`、`spirv-val`；它们应安装在 `dev`。
服务、LDC、gather、打包纹理和局部纹理更新另有 `tools/test_*.c` 合成测试，历史结果见 [适配记录](TOTK-PORT.md)。
上游 RomFS 测试中的 BOTW 版本检查是继承代码的回归验证，不能代替 TOTK 资源验收。

只运行与变更相关的检查。APK 构建命令见 [构建指南](BUILD.md)。上游 `tools/test_lift.py` 依赖 macOS/ARM64，
不能把在本机 x86_64 Linux 上未执行的测试记为通过。

## 实机证据

每轮保存提交或 APK 标识、驱动及版本、环境变量、场景、用户反馈、有效帧号和日志。
ADB 必须明确指定一加设备，日志标签为 `TOTKRecompAndroid`。上游设备记录不算本分支结果。
短暂正常不能推翻用户的闪烁反馈；不同轮次目录中同名的旧帧必须排除。

`LARECOMP_NO_SCALE=1` 只关闭运行时缩放。上游合并后，`LARECOMP_GAME_DYNRES=1` 可恢复游戏自己的动态分辨率；
默认计数器策略已变化，比较前必须记录该变量及实际附件尺寸。

## 抓帧与独立回放

`LARECOMP_DEBUG_FRAME` / `LARECOMP_DEBUG_FRAMES` 指定帧段；F9 可在同一进程中再次触发。
Android 可通过 `adb -s "$ANDROID_SERIAL" shell input keyevent KEYCODE_F9` 发送。

| 变量 | 内容 |
| --- | --- |
| `LARECOMP_DEBUG_NO_PASSES=1` | 关闭原有逐 pass 图片等待，仅保留绘制日志 |
| `LARECOMP_CAPTURE_ATTACHMENTS=1` | 在目标切换时复制真实颜色 MRT、深度和模板，帧末统一回读 |
| `LARECOMP_DUMP_VERTEX_INPUT/OUTPUT` | 指定顶点程序或目标尺寸，保存输入与 GPU 输出 |
| `LARECOMP_DUMP_FRAGMENT_INPUT` | 指定合成片元程序，保存支持的输入纹理 |
| `LARECOMP_DUMP_TARGET` | 指定 guest 纹理地址，在 pass 结束回读颜色与深度 |
| `LARECOMP_DUMP_GLOBAL_INPUT=1` | 配合顶点诊断回读全局缓冲与地址表 |
| `LARECOMP_SAVE_MSL=1` | 保存翻译后的着色器；Vulkan 下实际为 GLSL |

附件捕获在 `build/capture/<frame>/manifest.tsv` 记录格式、尺寸、地址、mip/layer 和实际字节数。
每帧最多 512 MiB、1024 张图，日志报告遗漏数；当前不支持该诊断中的 3D/MSAA/压缩附件。
最后一组是该帧提交显示的图像，与同目录绘制数据使用相同编号。

所有 GPU 回读都会改变时序；新捕获减少了逐 pass 的 CPU 等待，但仍不能用于性能验收。
插入着色器诊断也可能改变驱动优化。独立回放通过只证明相同输入下的结果，不能证明游戏中的绑定与同步均正确。
私有 GPU 数据、生成着色器和回放文件不要提交 Git。

## 手机本地闪屏抓取

游戏内打开设置 → **视频 → 闪屏抓取**，点击“开始闪屏抓取”；完成后在同处点击“停止闪屏抓取”。
默认关闭，开始后顶部显示保存组数。变化阈值默认 8%，可调 2%–40%；数值越低越容易捕获局部变化，
也越容易把快速转镜头或正常转场记为候选。检测比较游戏最终图像，不包含触控、设置和 FPS 浮层。

手机保留三个有界 GPU 回读槽：异常前一帧、异常期间变化最大的一帧、恢复或稳定后的一帧。
最长等待 1.5 秒；未恢复时会标注 `settled` / `timeout`，停止或打开设置时标注 `stopped` / `interrupted`。
这不是闪屏根因判断。GPU 回读未完成或正在写 PNG 时跳过采集，界面显示累计漏采帧数，文件记录真实帧号与时间。
每张图宽度最多 960 像素，普通 16:9 场景的三个回读槽合计约 6 MiB，另有缩小图像和 PNG 编码开销。
每次会话达到 64 组或 256 MiB 后停止；关闭并排空待处理工作后释放抓取图像内存。

保存路径：`/storage/emulated/0/Android/data/com.totkrecomp.android/files/flicker/<日期时间>/`。

- `status.txt`：开关状态、阈值、保存数量、漏采数和错误。
- `events.tsv`：候选目录、前／中／后帧号、变化分数和结束原因。
- `event-0001/01-before.png`、`02-peak.png`、`03-after.png`：同次异常的三张图。
- `event-0001/preview.png`：从左到右为变化前、峰值、变化后，绿／红／蓝色条对应三张图。
- `event-0001/event.json`：时间、帧号、图像尺寸、阈值和结束原因。

先在手机看预览，或只取 `events.tsv` 和选中事件的 `preview.png`；确认候选后再取该事件三张图片。
不要拉取整个会话目录或长视频。该功能不会自动传输任何文件，也不写全程图片。

检测逻辑的独立回归：

```bash
cc -O1 -g -fsanitize=address,undefined tools/test_flicker_detect.c -lm \
  -o .work/totk-port/scratch/test-flicker
.work/totk-port/scratch/test-flicker
```

TMML 纹理细节级别查询的回归执行正式翻译器输出，检查绑定／无绑定纹理、返回分量掩码、
负数与小数定点值以及采样器限制。Maxwell 返回“计算值、实际层级”，GLSL 的顺序相反：

```bash
mkdir -p .work/totk-port/shader-tmml-tests
cc -O1 -g -fsanitize=address,undefined tools/test_shader_tmml.c -lm \
  -o .work/totk-port/shader-tmml-tests/generate
.work/totk-port/shader-tmml-tests/generate .work/totk-port/shader-tmml-tests
c++ -fsanitize=undefined .work/totk-port/shader-tmml-tests/tmml.cpp \
  -o .work/totk-port/shader-tmml-tests/check
.work/totk-port/shader-tmml-tests/check
for s in .work/totk-port/shader-tmml-tests/*.frag; do
  glslangValidator --target-env vulkan1.3 -V "$s" -o "$s.spv"
  spirv-val --target-env vulkan1.3 "$s.spv"
done
```

这同时生成六个合成片元着色器，可用于真实 GPU 采样器验证；主机断言通过不等于游戏画面验收。

深度精度回归通过正式 shaderc 编译路径检查顶点和显式片元深度、普通颜色片元、缓存及重复处理：

```bash
mkdir -p .work/totk-port/shader-depth-tests
clang -O1 -g -fsanitize=address,undefined -ffunction-sections -fdata-sections \
  -Iandroid/deps/out/include tools/test_shader_depth_precision.c -Wl,--gc-sections \
  -lshaderc_shared -lpthread -lvulkan -o .work/totk-port/shader-depth-tests/test-depth
.work/totk-port/shader-depth-tests/test-depth "$PWD/.work/totk-port/shader-depth-tests"
for s in .work/totk-port/shader-depth-tests/*.spv; do spirv-val --target-env vulkan1.3 "$s"; done
```

还需在目标 GPU 验证片元写深度、顶点重算深度的 EQUAL 绘制。run46 在同一版 Banner Turnip 上
测试全部 256 个编码值；修复前 D16 有 12 个不匹配（包含水面），正式修复生成的模块全部通过。
这些是合成编码值数量，不是游戏中缺失材质或效果的数量。


条件清除协议使用真实命令记录器，验证记录后更新计数值、仅清除一次、忽略时间戳与 64 位比较。使用 Clang 编译：

```bash
clang -O2 -ffunction-sections -fdata-sections tools/test_render_condition.c \
  -Wl,--gc-sections -pthread -o .work/totk-port/scratch/test-render-condition
.work/totk-port/scratch/test-render-condition
```

纹理 CPU 读写测试覆盖独立 GOB 字节、mip／数组／3D／步长、压缩尾块图集和越界拒绝，
以及 `0x40` 线性目标的 260×260 / 68×68 高度与遮罩格式、显式行距、默认对齐、尾行更新及 padding 保持：

```bash
clang -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wno-format \
  -ffunction-sections -fdata-sections -Wl,--gc-sections -Iruntime \
  tools/test_texture_read.c runtime/nvn/tegra.c runtime/nvn/formats.c \
  -o .work/totk-port/scratch/test_texture_read
.work/totk-port/scratch/test_texture_read
```

错误弹窗的实机检查会启动测试 Activity、触发受控 JNI 运行时错误，并自动点击复制与 OK。
检查剪贴板、弹窗、Activity 和失败线程；测试完成后由 Android 结束测试进程。它不验证真实信号崩溃恢复。

```bash
cd android/botwrecompandroid
bash gradlew --no-daemon --max-workers=4 assembleDebug assembleDebugAndroidTest
adb -s "$ANDROID_SERIAL" install --streaming -r app/build/outputs/apk/debug/app-debug.apk
adb -s "$ANDROID_SERIAL" install --streaming -r app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk
adb -s "$ANDROID_SERIAL" shell am instrument -w \
  com.totkrecomp.android.test/com.botwrecompandroid.RuntimeErrorDialogCheck </dev/null
```

输出须包含 `PASS: Copy preserved the dialog`；不要仅根据 adb 退出码判定通过。

音频混音检查使用自造参数包和已知样本，不需要游戏文件或声卡，覆盖分流器、ADPCM、辅助效果环、限幅和 3D 混响：

```bash
clang -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wno-format \
  -ffunction-sections -fdata-sections -Wl,--gc-sections -Iruntime -pthread \
  tools/test_audio_mixer.c -lm -o .work/totk-port/scratch/test_audio_mixer
.work/totk-port/scratch/test_audio_mixer
```

`LARECOMP_AUDIO_STATS=1` 每 200 个音频帧记录直接/分流/未连接声道及解码样本数；未连接声源可能是游戏主动停用，
不能直接视作丢失。`LARECOMP_AUDIO_DUMP=1` 保存包括首次完整初始化在内的参数快照；
`LARECOMP_AUDIO_WAV=<应用数据目录内路径>` 保存 audren 的浮点立体声输出。抓取不是完整游戏音频覆盖率证明。
