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


条件清除协议使用真实命令记录器，验证记录后更新计数值、仅清除一次、忽略时间戳与 64 位比较。使用 Clang 编译：

```bash
clang -O2 -ffunction-sections -fdata-sections tools/test_render_condition.c \
  -Wl,--gc-sections -pthread -o .work/totk-port/scratch/test-render-condition
.work/totk-port/scratch/test-render-condition
```
