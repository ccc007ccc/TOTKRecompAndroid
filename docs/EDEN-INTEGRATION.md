# Eden 优化与修复适配记录

2026-10-29。用户要求性能分支合入 `main` 后，在 `main` 审查并接入 Eden 中适用于 Android TotK 的优化与修复。
性能分支合并、O3/ThinLTO 与 8 Elite 游戏库已经完成；本轮不重复构建档对照。
草消失、位置异常和武器无法割草，以及仅自动读档出现的加载背景变黑，仍保留为待处理问题。

本轮参考 [Eden](https://git.eden-emu.dev/eden-emu/eden) 的固定版本
[`26a03d2473787abf3223b2d0e65b291eee00c1e1`](https://git.eden-emu.dev/eden-emu/eden/commit/26a03d2473787abf3223b2d0e65b291eee00c1e1)。
Eden 使用 GPU 命令解码、SSA 着色器中间表示及 NCE/JIT；本项目拦截 NVN 并运行静态重编译游戏代码。
因此逐项核对行为并适配到现有实现，不能直接合并整个源码树，也不能把同类机制视为相同覆盖率。

## 当前已接入工作区

| 来源 / 要求 | 当前行为 | 验证边界 |
| --- | --- | --- |
| Eden [`a95efaf` / #4565](https://git.eden-emu.dev/eden-emu/eden/commit/a95efaf18f50721aa6f5cdce8b5b94e858ffd8d1) | Qualcomm 原厂、Turnip 以及 AMD/Intel 默认使用 Maxwell 的 8-bit gather 坐标补偿；不再从主机报告的精度推导。保留 `LARECOMP_GATHER_BIAS_BITS` 覆盖 | 16 组生产策略检查、50 个采样着色器验证；不据此声称草或所有黑块已修复 |
| Eden [`dbeb73e` / #4473](https://git.eden-emu.dev/eden-emu/eden/commit/dbeb73ee011416cffc6c203945f5ccc6b90621f6)、[`a3debcf` / #4528](https://git.eden-emu.dev/eden-emu/eden/commit/a3debcf731143aabd6eefee406d511551d3c9a73) 的 CPU/GPU 写入一致性原则 | CPU flush 使复制缓存、驻留缓冲检查和顶点页缓存失效；纹理视图 memo 检查 CPU 写入 epoch。缓冲失效通过 Android 渲染请求队列串行处理 | 实际 C helper、跨页/边界/溢出及队列顺序检查；未实现 Eden 的 NCE GPU 回读机制，不能把 CPU flush 修正称为完整 GPU→CPU 一致性 |
| Eden [`45b6ecf` / #4294](https://git.eden-emu.dev/eden-emu/eden/commit/45b6ecff055cc6de6e7b9edc2239e0c6636de573) 的持久管线缓存 | 游戏及内置管线共用现有 driver cache；补齐缓存增长时有限重试、驱动身份与大小检查、并发保存、写入/关闭失败处理和原子替换。只有保存成功才更新已保存大小 | 真实文件操作加模拟驱动；失败保留旧文件，关闭缓存不读写。仍需驱动管线创建，不等于消除全部卡顿 |
| 用户最新纠正 | Android 默认允许运行时编译回退；先加载缓存，缺失/损坏才编译并保存。设置 `shader_runtime_compile = 0` 或 `LARECOMP_SHADER_RUNTIME_COMPILE=0` 关闭；关闭时保留严格准备门禁和明确报错 | 严格模式、有效缓存、损坏修复、编译错误、并发首次加载和缓存不可写均通过检查。`vk_load_prepared_shader` 始终是严格审计入口 |

完整手机预编译及后续[均衡准备方案](SHADER-BALANCED.md)由另一项任务负责。
这里不恢复已经被用户纠正的“游戏运行中一律禁止编译”要求，也不启动整库准备。

## 已核对的对应机制

| Eden 参考 | 本项目当前路径与结论 |
| --- | --- |
| [`17c341f` / #3924](https://git.eden-emu.dev/eden-emu/eden/commit/17c341ff6c0f8be5d4e7614389456159cd247bd0)、[`028050c` / #3789](https://git.eden-emu.dev/eden-emu/eden/commit/028050cf044dafe667de71167eadeecbc0e328a6)：按描述符索引缩放元数据 | `RSC2` / `off_scale` 使用 `rscale[slot+1]`，后端按同一资源槽填写；当前不是 Eden 的描述符数组位图表示。已核对的索引修复无需照搬；这不证明动态描述符数组已经支持 |
| [`26a03d2` / #4512](https://git.eden-emu.dev/eden-emu/eden/commit/26a03d2473787abf3223b2d0e65b291eee00c1e1)：调整异步 shader 等待 | 后续[均衡准备工作](SHADER-BALANCED.md)已将绘制/计算改为默认等待实际程序和管线，旧跳过策略仅保留为诊断开关；没有直接套用 Eden 的深度/小绘制量启发式。新路径已通过独立 GPU 检查，首次游戏画面和帧时间仍需单独验收 |
| [`ba608d2` / #4085](https://git.eden-emu.dev/eden-emu/eden/commit/ba608d2b578b4f11784e6bac118d10790b1e7c2e)、[`a27d354` / #4182](https://git.eden-emu.dev/eden-emu/eden/commit/a27d35463ef9d78553313b568e96f98097be7c11)：提交限流和同步 | 已有有界请求队列、present 超前限制、GPU submit 槽复用等待及回写完成序号；保留这些机制。未证明照搬 Eden 的 tick 阈值能改善本项目 |
| [`573e061` / #3986](https://git.eden-emu.dev/eden-emu/eden/commit/573e06131dd969114c61f6db48254ad30aebd796)：加载界面纹理回收 | 修复 Eden LRU 回调返回类型和遍历预算；本项目按完成序号延迟销毁，没有该 LRU 模板。不能据同为加载界面问题判定是同一根因 |
| [`60a474b` / #4297](https://git.eden-emu.dev/eden-emu/eden/commit/60a474b8df051beb1d5fb84f4363d1576fcbb3fa)：QCOM DenormFlushToZero | 本项目没有注入该 SPIR-V 执行模式；保留现有深度运算 NoContraction 修正。该提交不构成当前浮点路径的完整数值证明 |

## 尚未完成，继续按此范围推进

1. **条件纹理句柄与动态描述符。** Eden [`0689f73` / #4551](https://git.eden-emu.dev/eden-emu/eden/commit/0689f73147ebeddb9ac5fb8b0edf2625425f8d71)
   按条件解析 Phi 输入；本地 `handle_cbuf` 仍是向前回溯最后一次 MOV/LDC 的直线扫描，未建模分支、谓词和寄存器拷贝。
   `a76c76d` 的动态描述符步长和 `ae75ba4` 的 cbuf 身份冲突也未完整适配。下一步应先建立真实输入/合成分支的复现，再补数据流与绑定路径。
2. **整数采样纹理。** Eden `8225151` / #4189 的整数采样类型处理尚未对应到本地普通 sampler；
   已有 storage image 的 `image_kind` 不等于 sampled texture 的整数类型支持。必须同时核对着色器键、sampler 声明、寄存器位模式、视图格式及 GPU 数值，不能仅让 GLSL 编译通过。
3. **GPU 写入后的 CPU 读取。** Eden `0e11fe8` / #4531 的 NCE 失效回读没有可直接调用的本地同构接口；
   需要追踪 NVN invalidate/readtexels 的所有权、已完成序号与实际修改范围。现有 CPU flush 测试没有覆盖这项，也没有证明它是草问题根因。
4. **缺失 subgroup 的设备兼容。** Eden `a594c8d` / #4508 的功能查询、warp 操作和 geometry 处理尚未移植。
   本地仍有直接 subgroup 调用和顶点 SHFL 的既有回退；不能声称覆盖其他 GPU。应以具体指令和设备能力建立验收。
5. **其余大型 Vulkan 提交。** `8225151`、`eb9280d`、`3d19743` 含多个独立改动；只审阅到的子项按上表记录，未将整笔提交标为已适配。
   NCE/TLS/JIT、固件系统菜单、OpenGL、桌面窗口及其他游戏地址不直接移植。

## 可重复验证

```bash
distrobox enter dev -- bash -lc 'source android/dev_env.sh; bash tools/test_eden_runtime.sh'
```

该入口验证 gather 默认值/覆盖、采样着色器、上传缓存、31,040 组描述符计划、实际 Android 请求队列、
管线缓存文件保存及可开关运行时编译。队列测试包含所需的 Volk 头路径，避免把旧日志里的首次缺头编译失败误作最新结果。
缓存测试故意触发的 `FATAL` 来自隔离的负面用例，以最终退出状态及 `PASS` 为准。

2026-10-29 接手复验在当前混合工作区通过上述检查，并成功构建 Release / snapdragon-8elite / Performance Test APK。
详细日志与会话原始证据保存在 `.work/handoff-20261029-eden-resume/`，不提交游戏数据、手机缓存或构建产物。
这些检查不等于全部 Eden 适配完成，也不证明性能收益或草问题已修复。

本次 APK（SHA-256 `c327420dd2c692a434d789a1b7ed3398ecf38b08ddeb34ecb06102a2a9d4aec0`）已在 PJZ110
使用原有 8 Elite 游戏库、Turnip、独立 slot_00 存档完成一次 60 档短测：加载就绪后 413 帧 / 15.0286 秒，
27.481 FPS，p95 45.627 ms，运行时按时自行退出。日志确认实际触发运行时编译并保存约 53.4 MiB driver pipeline cache；
没有本轮 FATAL。此轮为新改动运行验收，未作 O3 构建对照或画面正确性判断。

同一日志有 `[shader-warm] cannot write shader warm log`，应交由着色器准备任务跟进；
运行时编译回退使本次仍可进入场景，这不能视为后台预热通过。测试期间共享工作区的准备代码仍在变化，
本次实机结论只对应上述 APK，不能自动扩展到稍后的源文件。
