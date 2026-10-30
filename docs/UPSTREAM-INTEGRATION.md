# 上游整合与帧率适配（开发中）

2026-10-29 后续选择性同步：IADD3、四边形条带、全局内存一致性及最新共享清单，见 [本轮整合记录](UPSTREAM-MERGE-20261029.md)。

2026-10-30 后续选择性适配 mamcjr/TOTK 的内部间隔保存与淡入淡出，过场候选标记因草地误判默认关闭，见 [游戏间隔记录](FRAMEPACE-20261030.md)。
其宽屏仍只允许 BOTW 1.8.2；下文 10-28/29 的计时与验收结果保留当时范围。

2026-10-29 后续更新：自动读档加载背景黑底已修复，草地短线复查及范围见 [画面验收](VISUAL-VALIDATION.md)；下文旧记录保留当时状态。

简体中文 | [English](en/UPSTREAM-INTEGRATION.md)

目标是合入两个参考项目中适用于 Android TotK 的修复，并提供 **30、45、60、120 FPS** 四档。120 是用户后续明确追加的要求。目标帧率与硬件实际达到的帧率分别验收。

## 固定的参考版本

- [BOTWRecompAndroid](https://github.com/mamcjr/BOTWRecompAndroid)：先审计 `ef7a306..4572f067` 的57个提交，再核对其后的19个提交，固定到 [`5675618197`](https://github.com/mamcjr/BOTWRecompAndroid/commit/567561819724e6f7425b595de301f2dfd4592eef)，v0.2.2-beta。
- [tib-kpl/BOTWRecompAndroid](https://github.com/tib-kpl/BOTWRecompAndroid)：已审阅的运行时移植提交 `4d2c20e`、`80bc5c4` 通过 `b386cce` 合入本工作分支。没有覆盖并行的 main shader 工作，也没有合入构建输出。
- [UltraTears PR #27](https://github.com/kuraeow/ultratears/pull/27)：尚未合并，head `4ea835e4`，base/dev `1970c7ad`。PR 主要添加启动器；120 选项传入 `TOTK_UNCAPPED_HZ=120`。dev 的 `src/gfx/nvn/hle_nvn.c` 明确说明游戏补丁和实时步长尚未移植，因此不能把该选项当成解帧已验证的证据。
- UltraTears 的注释指向原始 [MacNativeGames TotK 1.4.3 计时实现](https://github.com/ly05010419/MacNativeGames/blob/ff63a162b35d8b486b941191785d8e957c1f5df2/games/totk/runtime/game_patches_v143.c)。据此核对自有 1.4.3 输入并适配生成器。该来源是 MIT，保留了 [许可证](../runtime/hle/LICENSE-MacNativeGames.txt)。UltraTears 的整体 GPL 许可证不改作 MIT，也未直接复制其 GPL 实现。

## 已落入工作分支的代码

| 来源 | 整合内容 | 本地保留/补充 |
| --- | --- | --- |
| `502088f`、`e0df814` | GPU 回写满槽等待，128 个小槽和32个大槽 | 保留 `nvn_texture_row_stride()`；跨池与跨圈按复制顺序回写；等待超时不读取未完成数据；等待和丢弃分别计数 |
| `1312e88`、`be08e10` | 缓存片元输出掩码；每次 draw 只计算一次写附件；按字哈希；关闭 AB 时跳过调用 | 不把上游其他手机的百分比当成本机提升 |
| `262653c` | GPU heap placement 失败后启用写追踪；Mali-G2-Ultra 的兼容默认值 | 保留现有 Adreno 8xx 与测试库选择逻辑 |
| `1075f5e`、`9603800`、`fc0bb10` 的生成器部分 | FP load/store 不再冒充通用寄存器写；只缓存分支；已解析跳转表的 fallback 限于基本块起点 | 保留本地 ADR 局部返回与 relocation 支持；必须重新生成并构建游戏库 |
| `b8dec17`、`6582d40` | 手机构建巨大函数独占编译；dev 档允许 O0，Zig O0 不隐式带入 UBSan/栈保护/调试信息 | 性能档全部 O3/ThinLTO、禁止 O0 回退；保留 NDK LICM 修复与资源上限 |
| `4a27085` | MapMemory / UnmapMemory 的地址边界检查 | 保留 TotK 的区域锁、物理内存映射和预算处理 |
| `526fc76`、`0464be0` | 崩溃报告串行化，先输出 guest 寄存器与调用链 | 调用链边界检查避免整数溢出 |
| `201e99b` | 文档提供器重复条目去重；重新导入保留 `.gamebuild` | 沿用输入与构建身份校验，不把旧对象当成新版本游戏代码 |
| `9f94aba` | 精简 GLSL prelude、内建 casts、去掉 SPIR-V 调试名字 | 保留顶点与显式片元深度的 NoContraction |
| `9c3a0f3` | 顶点 2D/array 采样按 1/256 texel 取整，保留为显式实验 | TotK 默认关闭；`LARECOMP_VS_SNAP=1` 开启，`LARECOMP_NO_VS_SNAP` 优先禁用；量化进入 ShaderKey，尺寸查询限制在有效 mip 范围；保留 8-bit gather |
| MacNativeGames 1.4.3 | 解除整数 VI 档位，时间生产者、按键计时、布料/头发第二时钟与高帧率蹦床修正 | 生成时逐指令验证；钩子在原指令前，直接 BL 与 LR 保留；支持30/45/60/120；Android surface 请求对应刷新率 |
| tib-kpl 运行时移植 | 上传缓存失效、动态 UBO 计划、反馈图像 mip/layer 与分配身份、heap fallback | 保留 TotK 的纹理行距、黑块和深度修复；缓存测试及边界/别名检查通过 |
| tib-kpl 命令追踪 | 命令页原子发布、无锁查询、timeline poll 诊断 | fast/locked/ab 三条查询路径均完成480000次并发核对 |
| `97afeda`、`2f627eb` | 前台 BuildService、编译/链接参数与四模块完整 build ID 的续传签名、缺符号时使 managed 库失效 | 保留明确指定的测试库；服务编译通过，完整手机构建仍未验收 |
| MacNativeGames 提交时数据快照 | UBO、SSBO、CopyBuffer 源字节冻结；回放时按捕获边界检查 GPU 写入失效 | 独立移植，默认实验关闭；总内存预算与同步降级、跨提交写、继承绑定和资源边界均有 CPU 检查 |

## 计时约束

原 20 FPS 来自游戏选择 present interval=3，运行时等待三个 60 Hz VI 周期。仅改变等待会让游戏加速。新路径使用独立纳秒节拍，并把实测时长送入 TotK 时间生产者；其原来的半步选择和慢动作倍率继续执行。不能使用 BOTW 的 VFR 地址。

布料/头发的独立时钟仅修改两个 inline 位置与两个固定时间调用者，已经传入实测时间的第三个调用者不重复缩放。蹦床修复把“FPS >100”的慢动作判定换算成与帧率无关的时间倍率判定。

时长使用五帧均值，没有 1/120 秒下限（否则 120 附近的抖动会使计时加速）。长暂停清空历史；单步最长 1/15 秒，持续低于15 FPS时会放慢游戏以限制物理步长。视频默认30 FPS。TotK 的菜单/事件策略尚待玩法验收，不借用 BOTW 地址猜测状态。

`LARECOMP_FRAMEPACE=0` 保留原版指令与计时，供对照；`LARECOMP_FPS` 覆盖设置（TotK 上限120），`LARECOMP_LOG_FRAMEPACE=N` 输出目标档位和实测步长。原 O1 库仅作为旧路径基线；新运行时与新库必须匹配 GAMELIB_ID。

## 后续整合与验收

- 后续 Eden 审查在 `main` 进行，已接入的 gather、CPU 写入失效、管线缓存修正及仍缺失的适配见 [Eden 适配记录](EDEN-INTEGRATION.md)。该记录单独保留当前代码与实机验收边界。

- 提交时快照已从 MIT 原始机制独立适配为 `LARECOMP_VK_SUBMIT_SNAPSHOT=1`，默认关闭。每请求16MiB、全部在途请求64MiB；缺少安全资源范围或超出预算时，提交在返回前等待翻译完成。捕获边界使用 GPU 回写已经落到 CPU 后发布的序号；同提交 inline/copy 写和存储 shader 写会失效，未知 STG 范围保守失效整个 heap。完整实机玩法和性能仍需验收，不能把 CPU 检查当作默认启用的依据。
- Android 调度预算已随30/45/60/120档变化，避免 render ADPF 请求始终使用60档预算。原计时关闭路径仍沿用既有提示。
- 手柄死区/陀螺仪偏置与通用存档传输仍需按 TotK 数据/输入合同审查；不直接搬 BOTW 的存档文件判定或 DLC 逻辑。
- Sync lag 的上游默认值来回变更，并有脚陷地、白水面和地形问题记录，本次不默认开启。BOTW 1.9.0 地址、DLC、Windows/Tauri 打包与 MetalFX 插帧不直接合入 Android TotK。
- 最新 native ARM64 实验依赖 BOTW 补丁/入口，尚未按 TotK 四模块与1.4.3输入验证，本次不直接移植。新增64KiB UBO分段直读在上游后续提交中恢复默认关闭，保留本地安全复制路径；回写 FIFO 修复已由本地跨池/跨圈实现覆盖。
- 三档 APK 已构建并核对包名、图标、调试标记、签名和实际 native 参数；全 O3/ThinLTO、8 Elite 游戏库已完整链接、部署并完成60档短测。O1/O3的温度和充电条件不同，不据此判断优化收益，也不重复已完成的对照；数据见 [手机性能会话](PHONE-PERFORMANCE.md)。四档目标路径已有低吞吐场景记录，稳定30帧以上及120帧玩法仍未证明。
- 常规测试现为自动指定存档、就绪后定时测60档、结束退出。用户确认草偶尔消失在普通版也存在，本轮保留待修记录；不能将顶点量化开关当作根因修复。自动测试读档的加载格线背景可变黑，用户反馈手动读档正常，已决定暂不处理。

## 当前可重复的 CPU 检查

在 dev 容器内、工作分支根目录执行，Python 依赖使用主目录 `android/dev_env.sh` 中的环境：

```bash
"$PYTHON" tools/test_totk_framepace.py
"$PYTHON" tools/test_jump_table_stores.py
"$PYTHON" tools/test_local_branches.py
"$PYTHON" tools/test_writeback.py
"$PYTHON" tools/test_gamebuild_hot.py
"$PYTHON" tools/test_gamebuild_flags.py --zig /home/deck/Code/Toolchains/zig-0.17.0/zig
"$PYTHON" tools/test_android_packages.py
"$PYTHON" tools/test_submit_snapshot.py
"$PYTHON" tools/test_shader_sampling.py
"$PYTHON" tools/test_shader_control_flow.py
```

已通过：四档计时/切档/暂停/抖动/低帧率与各计时钩子；14个补丁位置及邻接 guard 与私有映像匹配；FP load/store 局部跳转；回写池满、延迟完成、跨池顺序、行距、超时；19项 dev/performance 构建策略；NDK/Zig 参数与完整四模块续传签名；stripped ELF 及包/库匹配；10000次提交快照并发捕获/回放与边界/预算/GPU失效；50个采样 shader（含负数/超范围LOD），默认不量化与独立缓存身份检查；8192项控制流执行与96个 GLSL/SPIR-V shader。既有深度精度、TotK 输入契约、定时退出与脚本检查亦通过。上述 CPU 检查不证明完整实机场景已完成。
