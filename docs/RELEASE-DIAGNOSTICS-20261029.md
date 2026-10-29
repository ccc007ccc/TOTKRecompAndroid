# 2026-10-29：Release 诊断隔离与性能测量更正

用户报告普通 Release 带抓帧按钮，并且转视角时会掉到个位数帧；Performance Test 同场景只有十几帧。
这轮从 `629fac1` 审核实际包装、渲染设置、探针与测试控制器。此前短测中的 30 多帧不能代表普通游玩。

## 已确认的问题

1. 抓帧 UI 只判断 Android，普通 Release 因而也显示按钮、响应 F8/F9/F10；原生 CMake 用通配符编入全部抓帧与测试后端。
   RenderDoc 即使未启用也被打包。它原本需要显式启动参数才加载；按钮存在不等于始终在抓取 GPU。
2. Activity 接受任意 Intent `env` 测试参数，缺少 Release 边界。诊断、脚本输入、观察内存与额外 profiling 原本依靠运行时 opt-in。
3. `phone_test_session.py` 隐含传入 `LARECOMP_NO_SCALE=1`，让 `textures.c` 为所有纹理返回 `RS_NONE`，绕过内部与输出目标缩放。
   手机两个包虽然都设为 2×，自动测试实际上没有执行这组渲染负载。
4. 控制器还显式传 `LARECOMP_GAME_DYNRES=1`，而普通启动的实际默认为关闭；旧规范把两者混写成了默认开启。
   本轮修正测试口径，保留运行时原有默认，不用降低画质掩盖问题。

## 构建边界

Gradle 同时设置 Java `BuildConfig.DIAGNOSTICS` 和 native `TOTK_DIAGNOSTICS`：普通 Release 为 false/0，
Debug 和独立 Performance Test 为 true/1。原生 profile 仍分别决定 O2、O3/ThinLTO 与设备指令调优。

普通 Release 排除 `test_session.c`、`test_boot.c`、`flicker.c`、`diagnostic.c`、`renderdoc_bridge.c`、`timing_trace.c`，
不打包 RenderDoc。关闭抓帧按钮/设置/快捷键及测试控制文件接口；诊断环境通过统一 helper 编译为无效。
覆盖 GPU 附件/顶点/片元/常量转储、着色器替换与统计、内存观察、函数/SVC profiling、音频转储、脚本输入与故障注入等。

正式渲染、触控、按键绑定、菜单、FPS 显示、故障报告、驱动选择、着色器准备保持可用。
`fnprof_hit` 的 bit 1（数值 2）用于内核抢占，仍保留；仅函数计数与回溯记录关闭。
正常性能覆盖层和限频的 GPU 时间测量也保留，不能把所有时间戳都当作诊断删除。
Manifest 原有的 `profileable android:shell="true"` 保留，允许开发者通过 ADB 外部采样 CPU；
它不自动启动采样，也不等于 debuggable 或内置探针。普通 Release 仍为非 debuggable。
纹理快照复用与 shader 全局内存缺失范围学习属于资源正确性/复用路径，也没有按名字当成抓帧删除。

普通 Release 不再接受 Intent 的 `env` 测试注入。自动控制器只接受 `.debug` / `.perftest` 包。
性能测试默认尊重设置，`--texture-scaling native` 才显式绕过；`--game-dynres default` 才与普通启动一致。
新增 `--camera-turn` 在就绪后的计时区间连续半幅向右转镜头，状态握手拒绝不支持路线的旧 APK。

## 真实设置的初始复测

同一加 13 / PJZ110，Turnip `Mesa 27.0.0-devel (git-ab1502fd16)`，游戏 1.4.3，
O3 / 8 Elite 游戏库 `5f3156b208076afa8b07d831d538977e643083ed6014bfd8711760d5137f087c`，
`GAMELIB_ID=993c0a5c82dc5099`。普通包 external 游戏库、测试包 internal 默认库与自动测量指定库的 SHA-256 相同。
测试包为 Release/8 Elite/perftest，不能把其 debuggable 属性等同于 O1 Debug 游戏库。

控制器修正后，先用尚未修改的已安装 APK 测原固定存档，保留现有缓存，内部 2×、输出 screen、SGSR1、AA off，
目标 60 FPS，游戏动态分辨率不覆盖，20 秒静止测量：**19.71 FPS，p95 61.44 ms**。
私有证据：`.work/release-diagnostics-20261029/baseline-settings/benchmark-1793273380096680548/`。
这复现了十几帧，不能证明普通 Release 与测试包剩余差距的全部原因。

初次进入场景的日志还有 643 / 508 ms 长帧及管线编译等待；计时区间后段编译计数降到 0，仍约 19–20 FPS。
因此既要区分首次编译卡顿，也要继续分析稳定渲染吞吐；不能把全部卡顿归于缓存或诊断按钮。
两个包独立保存缓存，目录总大小不能代替同一有效命名空间/键命中率比较。本轮没有清空缓存。

## 验证约束

`tools/check_android_diagnostics.py` 从实际 APK 检查原生后端导出、敏感诊断开关、RenderDoc 包装及正常调度接口，
并复用现有 ELF 身份解析。`tools/test_release_diagnostics.c` 以 Android/诊断关闭编译，
在设置诊断环境后验证不启用测试会话，同时确认函数入口仍执行抢占。
控制器回归覆盖默认设置、显式原始尺寸对照、相机路线握手与旧 APK 拒绝；测试会话回归验证就绪前/结束后不转镜头。

## 转视角与阻塞记录

保持真实 2× 设置、60 FPS 目标、相同存档/游戏库/驱动，并保留缓存：

| 运行 | 秒数 | FPS | p95 ms | 电池温度前→后 | Android thermal status 前→后 |
| --- | ---: | ---: | ---: | --- | --- |
| 原 APK 静止，修正控制器 | 20.04 | 19.71 | 61.44 | 40.1→39.9°C | 2→3 |
| 新测试包首次连续转镜头 | 30.00 | 18.77 | 63.03 | 37.1→37.0°C | 0→2 |
| 相同 APK 重复路线 | 30.04 | 19.31 | 64.31 | 38.1→38.8°C | 1→3 |
| 最终测试包，带计时/单帧 GPU 时间戳 | 20.03 | 18.32 | 66.16 | 39.8→40.1°C | 2→3 |

温控等级 3 为 severe；电池温度不是 SoC 温度，不能据电池 37°C 宣称无温控。
这些结果证实实际负载下的低吞吐，不构成隔离温控的统计性性能比较；诊断行也不计为优化成绩。
首次/重复路线使用 APK `f5a4625b9f840eb2ee942093726ac1886132a57dfd753b906f5962c4462503df`；
最终诊断 APK 为 `94f4a05a4c6e2823eee788c814f6be801059bb336ee2c1ea96dd4ac6169f618c`。
中间差异为补齐未使用诊断入口的构建隔离；不把两个 APK 的差值算作优化收益。

最终诊断只取完整落在测量区间内的 336 帧统计窗口：

| 项目 | ms/帧 |
| --- | ---: |
| GPU submission 区间总时间 | 40.54 |
| CPU 等 GPU timeline | 20.11 |
| 等命令翻译 | 5.66 |
| 排空渲染队列 | 0.89 |
| 等窗口图像 | 0.059 |
| Present 调用 | 0.49 |
| 限帧等待 | <0.001 |

这些值可能重叠，不能相加或相减推导收益。GPU 区间本身已超过 30 FPS 的 33.3 ms 预算；
窗口获取/限帧不是当前主要阻塞，纹理排空为 0，已有排队优化确实在运行。
需要继续区分高分辨率 GPU 工作、CPU 翻译/上传与温控，而不是只修改 Debug/Release 优化级别。

第 1150 帧 GPU 标记区间 50.06 ms，较长区间包括 1600×896 的多附件绘制和 3200×1792 的深度绘制。
分别记录到 4.35 / 3.54 ms；标签给的是阶段与首个着色器，不能把整段时间都算到单个 shader。
转向途中还出现管线编译等待与 100 ms 以上长帧；重复路线仍约 19 FPS，说明缓存不是全部原因。
两个包都选择同一 Turnip，Android game mode 查询也都是 performance；没有发现普通包使用了较慢的游戏库档位。
普通包与测试包手动设置的 FPS 目标分别为 60/30，本轮自动对照统一为 60，没有修改用户设置。

## 最终产物与安装

- 普通 Release APK：`e39d7ca3496c6d9c8e85b63e57bdae0ebcd873ec31754d453112fae06e3cf351`。
- 普通运行时：`b5a81a416b8ea0e63805b42927216f1c9535a0109c6dd802b1503abaa0fbd4b1`。
- Release/8 Elite、Release/8 Elite/perftest、Debug/dev 均完成构建和实际 APK 隔离检查；普通包无抓帧/会话/时间跟踪后端导出与 RenderDoc，测试构建保留。
- 关闭诊断的原生测试、测试会话/超时/后台/停帧/相机边界、自动读档、17 项控制器测试通过。
  APK/库身份与端侧编译参数、纹理发布与异步队列、buffer-copy/UBO/通知、shader runtime、TotK 帧时序与 GPU 计时历史回归通过。
- 使用同一签名 `adb install -r` 更新一加 13 普通包，appId 10429 保持不变；安装前后设置哈希、游戏库哈希和存档目录权限一致。
  没有卸载、清数据、写存档或修改其权限。传入旧的故障/抓帧 Intent 参数仍正常启动；实机游戏画面确认抓帧按钮已消失，触控与 FPS 显示保留。
- 私有原始证据、安装回执与汇总在 `.work/release-diagnostics-20261029/`，不入 Git。

尚未证明 2× 可以稳定达到 30 FPS，也没有把用户所见普通包额外掉到个位数的所有条件完全复现。
后续优先做同温控状态下的高分辨率 GPU 阶段分析、首次/重复转向编译卡顿对照；长时间、移动、战斗和跨区域验收仍未完成。
