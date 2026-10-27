# 上游整合与帧率适配（开发中）

目标是合入两个参考项目中适用于 Android TotK 的修复，并提供 **30、45、60、120 FPS** 四档。120 是用户后续明确追加的要求。目标帧率与硬件实际达到的帧率分别验收。

## 固定的参考版本

- [BOTWRecompAndroid](https://github.com/mamcjr/BOTWRecompAndroid)：从 `ef7a306` 审计到 [`4572f067`](https://github.com/mamcjr/BOTWRecompAndroid/commit/4572f06716a49207fdcc13d34a45f154644202f3)，57 个提交，发布版 v0.2.1-beta。
- [UltraTears PR #27](https://github.com/kuraeow/ultratears/pull/27)：尚未合并，head `4ea835e4`，base/dev `1970c7ad`。PR 主要添加启动器；120 选项传入 `TOTK_UNCAPPED_HZ=120`。dev 的 `src/gfx/nvn/hle_nvn.c` 明确说明游戏补丁和实时步长尚未移植，因此不能把该选项当成解帧已验证的证据。
- UltraTears 的注释指向原始 [MacNativeGames TotK 1.4.3 计时实现](https://github.com/ly05010419/MacNativeGames/blob/ff63a162b35d8b486b941191785d8e957c1f5df2/games/totk/runtime/game_patches_v143.c)。据此核对自有 1.4.3 输入并适配生成器。该来源是 MIT，保留了 [许可证](../runtime/hle/LICENSE-MacNativeGames.txt)。UltraTears 的整体 GPL 许可证不改作 MIT，也未直接复制其 GPL 实现。

## 已落入工作分支的代码

| 来源 | 整合内容 | 本地保留/补充 |
| --- | --- | --- |
| `502088f`、`e0df814` | GPU 回写满槽等待，128 个小槽和32个大槽 | 保留 `nvn_texture_row_stride()`；跨池与跨圈按复制顺序回写；等待超时不读取未完成数据；等待和丢弃分别计数 |
| `1312e88`、`be08e10` | 缓存片元输出掩码；每次 draw 只计算一次写附件；按字哈希；关闭 AB 时跳过调用 | 不把上游其他手机的百分比当成本机提升 |
| `262653c` | GPU heap placement 失败后启用写追踪；Mali-G2-Ultra 的兼容默认值 | 保留现有 Adreno 8xx 与测试库选择逻辑 |
| `1075f5e`、`9603800`、`fc0bb10` 的生成器部分 | FP load/store 不再冒充通用寄存器写；只缓存分支；已解析跳转表的 fallback 限于基本块起点 | 保留本地 ADR 局部返回与 relocation 支持；必须重新生成并构建游戏库 |
| `b8dec17`、`6582d40` | 手机构建巨大函数独占编译、O0；Zig O0 不隐式带入 UBSan/栈保护/调试信息 | 桌面 NDK 的 LICM 修复与资源上限保留，新增生成结果不能沿用旧编号的 O0 例外 |
| `4a27085` | MapMemory / UnmapMemory 的地址边界检查 | 保留 TotK 的区域锁、物理内存映射和预算处理 |
| `526fc76`、`0464be0` | 崩溃报告串行化，先输出 guest 寄存器与调用链 | 调用链边界检查避免整数溢出 |
| `201e99b` | 文档提供器重复条目去重；重新导入保留 `.gamebuild` | 沿用输入与构建身份校验，不把旧对象当成新版本游戏代码 |
| `9f94aba` | 精简 GLSL prelude、内建 casts、去掉 SPIR-V 调试名字 | 保留顶点与显式片元深度的 NoContraction |
| `9c3a0f3` | 顶点 2D/array 采样按 1/256 texel 取整 | 不替换现有 8-bit gather 修正；真实地形仍需实机复测 |
| MacNativeGames 1.4.3 | 解除整数 VI 档位，时间生产者、按键计时、布料/头发第二时钟与高帧率蹦床修正 | 生成时逐指令验证；钩子在原指令前，直接 BL 与 LR 保留；支持30/45/60/120；Android surface 请求对应刷新率 |

## 计时约束

原 20 FPS 来自游戏选择 present interval=3，运行时等待三个 60 Hz VI 周期。仅改变等待会让游戏加速。新路径使用独立纳秒节拍，并把实测时长送入 TotK 时间生产者；其原来的半步选择和慢动作倍率继续执行。不能使用 BOTW 的 VFR 地址。

布料/头发的独立时钟仅修改两个 inline 位置与两个固定时间调用者，已经传入实测时间的第三个调用者不重复缩放。蹦床修复把“FPS >100”的慢动作判定换算成与帧率无关的时间倍率判定。

时长使用五帧均值，没有 1/120 秒下限（否则 120 附近的抖动会使计时加速）。长暂停清空历史；单步最长 1/15 秒，持续低于15 FPS时会放慢游戏以限制物理步长。视频默认30 FPS。TotK 的菜单/事件策略尚待玩法验收，不借用 BOTW 地址猜测状态。

`LARECOMP_FRAMEPACE=0` 保留原版指令与计时，供对照；`LARECOMP_FPS` 覆盖设置（TotK 上限120），`LARECOMP_LOG_FRAMEPACE=N` 输出目标档位和实测步长。原 O1 库仅作为旧路径基线；新运行时与新库必须匹配 GAMELIB_ID。

## 后续整合与验收

- UltraTears 的提交时 UBO/SSBO/复制源快照和 GPU 写入失效处理值得继续适配。当前异步队列只复制命令记录，不能据此声称已经固定了所引用的常量字节。需要从 MIT 原始来源核对，并验证同一提交中的 GPU 写、跨提交未完成写及内存上限。
- Android 前台构建服务、手柄死区/陀螺仪静态偏置、存档导入导出便利功能继续逐项审查；不直接搬 BOTW 的存档文件判定或 DLC 逻辑。
- Sync lag 的上游默认值来回变更，并有脚陷地、白水面和地形问题记录，本次不默认开启。BOTW 1.9.0 地址、DLC、Windows/Tauri 打包与 MetalFX 插帧不直接合入 Android TotK。
- 还需验证最终 APK、配套 O1 游戏库和实机30/45/60/120。指定存档、相同驱动与降温方法沿用 [手机性能会话](PHONE-PERFORMANCE.md)。画面、菜单、动画/物理与性能都必须检查；不能把编译成功当作实机完成。

## 当前可重复的 CPU 检查

在 dev 容器内、工作分支根目录执行，Python 依赖使用主目录 `android/dev_env.sh` 中的环境：

```bash
"$PYTHON" tools/test_totk_framepace.py
"$PYTHON" tools/test_jump_table_stores.py
"$PYTHON" tools/test_local_branches.py
"$PYTHON" tools/test_writeback.py
"$PYTHON" tools/test_gamebuild_hot.py
"$PYTHON" tools/test_shader_sampling.py
"$PYTHON" tools/test_shader_control_flow.py
```

已通过：四档计时/切档/暂停/抖动/低帧率与各计时钩子；14个补丁位置及邻接 guard 与私有映像匹配；FP load/store 局部跳转；回写池满、延迟完成、跨池顺序、行距、超时；14项构建策略；48个采样 shader；8192项控制流执行与96个 GLSL/SPIR-V shader。既有深度精度和 TotK 输入契约检查亦通过。上述测试不使用手机 GPU，不证明实机场景已完成。
