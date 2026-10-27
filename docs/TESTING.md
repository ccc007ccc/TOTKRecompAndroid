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

游戏顶部两个按钮保持不变：**持续抓帧**（再次点击停止）和 **抓取当前帧**。
默认使用**轻量诊断**，不加载 RenderDoc、不复制整块显存。F8 开关持续抓取，F10 单次；设置 → 视频可调阈值。
持续模式在手机内存比较最终画面，超过阈值才保存相邻的变化前、变化后两帧；单次不受阈值限制。
“变化后”是已经发生变化的帧，不表示恢复。游戏自身 UI 包含在内，触控、设置、FPS 浮层不参与比较。

每帧保留如下诊断内容，而不是只有最终截图：

- 渲染目标切换时，缩略颜色、深度和支持的模板附件，最长边128像素，记录地址、格式、mip、layer、阶段编号。
- 支持的归一化／浮点颜色以 RGBA16F 保存，保留 HDR 数值；深度保留对应原生深度格式。整数颜色等不支持的格式只记录元数据及省略原因，不执行非法格式转换。文件中的图像是缩略采样，不能代替精细像素或原分辨率输入验收。
- 绘制/跳过/清除/compute 记录、viewport/scissor、深度/模板/混合相关状态、纹理/采样器/顶点流绑定信息。
- 着色器原码与翻译参数；按实际使用范围保留有上限的常量 **CPU 镜像**。GPU 写入但尚未回写的输入可能与镜像不同，精确输入需后续定向抓取。
- 最终画面的960像素宽PNG和前后对照 `preview.png`。

每帧的阶段图、常量/着色器、日志预算分别为24、8、4 MiB；一组两帧核心缓冲约80 MiB，驱动和压缩线程另有开销。
着色器单份最多128 KiB，常量单份最多4 KiB，并记录原长度和实际保存长度。日志给后续绘制/跳过摘要和着色器原码/参数关联预留空间。
达到某项预算时只省略该项并计数，**不丢弃整个事件**。数据使用gzip压缩；实际文件大小取决于场景。
默认阈值8%，可调2%–40%。存储达到64组或512 MiB后结束当前轻量会话。

保存位置仍为 `/storage/emulated/0/Android/data/com.totkrecomp.android/files/flicker/<日期时间>/`：

- 连续事件有 `01-before/`、`02-after/` 子目录和同名PNG；单次为 `frame/`、`frame.png`。
- 每帧子目录：`diagnostic.json` 索引、`stages.bin.gz` 阶段图、`inputs.bin.gz` 着色器/常量、`draws.txt.gz` 绘制记录。
- `diagnostic.json` 列出省略数量，图像状态为 `ok/unsupported/budget/allocation`；输入ID为0表示未保存。
- `event.json` 的格式为 `light-diagnostic-v1`，记录真实帧号、时间、保存字节数和 `threshold/manual` 原因。
- 父目录有 `status.txt` 和 `events.tsv`。未完成写入使用 `.partial`，全部保存成功才改为正式事件目录。

检测和选帧在手机完成，普通帧不落盘。先在手机看预览，再只取选中事件。
轻量模式仍有额外GPU拷贝和每帧一次完成等待，可能改变随机问题的时序，不能用来测正常性能。
保存期间暂停采样并计数，随后重新建立相邻帧对照；停止/中断会排空相关GPU工作再释放缓冲。

本地小工具可对选中事件的前后阶段做数值排序，只生成少量对照图：

```bash
"$PYTHON" tools/analyze_light_capture.py <事件目录> --preview .work/totk-port/stage-changes.png
```

稳定复现问题可以继续用原有F9附件诊断、`LARECOMP_DUMP_TARGET` 指定原分辨率颜色/深度/模板，
或 `LARECOMP_SKIP_FSHASH`/`LARECOMP_SKIP_CS` 临时跳过特定shader作对照。先缩小到相关阶段，避免整帧显存快照。

只有确实需要逐条完整回放时，才在启动前设 `LARECOMP_FULL_GPU_CAPTURE=1`。
该高级模式需要 `bash android/deps/build_renderdoc.sh` 后打包的RenderDoc库，仍输出`.rdc`且可能占用数GiB；
UI明确显示完整模式。缺少库时回退轻量模式，不妨碍两个按钮使用。`LARECOMP_DISABLE_GPU_CAPTURE=1` 强制不加载该层。
标准`.rdc`回放依赖兼容GPU和内存布局；同手机同Turnip的独立回放工具及补丁保留在仓库中。

真实GPU回归（先构建APK，在dev中指定一加序列号和手机上的已安装驱动路径）：

```bash
ANDROID_SERIAL=<手机序列号> REPLAY_DRIVER=<手机驱动路径> bash tools/test_light_capture.sh
```

该测试覆盖半浮点颜色/深度像素、整数格式安全省略、变化前后、单次、稳定画面不保存、停止时仍有GPU工作、取消/重开；
也故意填满图像/输入/日志预算，验证事件仍保存且后续关键绘制摘要保留。只取回独立合成测试目录。
完整模式的 `tools/test_gpu_capture.c` 和 `tools/test_gpu_capture_replay.cpp` 保留作高级回归。

一加13 / Banner Turnip r5 已实机抓到蓝色出生圆盘黑块的第1906帧：选中的单帧事件共4,837,160字节（约4.6 MiB），
包含202张阶段图，图像和输入均无省略；详细绑定日志有省略，1537次绘制的尝试/结果摘要完整。
这证明轻量抓取已可用，不代表黑块已修复。

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
