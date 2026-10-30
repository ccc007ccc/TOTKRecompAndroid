# 2026-10-30：深度统计与首次完整清除合并

目标保持一加 13 / Snapdragon 8 Elite、真实 2× / 60 FPS，包含转视角。
承接 [Git 审查](UPSTREAM-PERFORMANCE-20261030.md)，本轮完成离线统计、清除实验、本机验证和两档 Release 构建。
用户“先写代码，不上手机测，我在用”的要求继续有效，本轮未使用 ADB 或操作手机。
worker/描述符实验仍待手机验收；首次落水后的持久 20 FPS 仍未复现，2×60 仍未达到。

## 深度统计的负结果

正式 shader_catalog 和 Maxwell 翻译器读取自有 1.4.3 RomFS：28,634 个去重输入中有 13,617 个片元输入，
73 个声明深度输出，原样 gl_FragCoord.z 回退路径为 **0**，翻译失败为 0。
配置为 Vulkan GLSL、FP32 优先、32-bit 地址运算、native half/minmax、8-bit gather 补偿与通用 varying 掩码。
[audit_shader_depth.cpp](../tools/audit_shader_depth.cpp) 输出机器可读统计；这是静态输入范围，不是活跃绘制频率或动态输入完整覆盖。
合成 Maxwell 回归覆盖无深度、稀疏 MRT 输出寄存器、真实和条件深度写入。
深度标志在 SPH 0x4c（完整 blob 0x7c）；深度寄存器为已启用颜色分量总数加一。

Turnip 支持 DepthUnchanged 保留 LRZ 的依据仍成立，但这个窄路径未命中，因此没有添加深度开关或修改译码器。
寄存器深度偶尔等于 FragCoord 不代表恒等；真实深度、NoContraction、discard 语义保留。

## 已实现的清除范围

LARECOMP_VK_LOADOP_CLEAR=1 仅诊断构建可启用，默认关闭。
需要开始新 pass 时，clear() 同步传入当前请求，在渲染开始前合入 loadOp CLEAR，成功后省去一次 vkCmdClearAttachments。
必须由原 scissor_rect() 证明实际缩放/裁剪后覆盖完整 render area，且等于所选附件完整 mip 物理范围。
颜色须全通道，stencil 须全低 8-bit 掩码，请求的每个 aspect 都存在；只合并实际单层 2D render view。
数组/cube 的所选层保持单层语义，3D slice、较小 render area、局部/部分掩码、无效/删除槽继续原路径。
已开始的 pass 即使尚无 draw 也继续原附件清除，避免增加 STORE/LOAD。

颜色按原 16-byte 位模式传递，支持 float/uint/sint；深度/stencil 独立设置 loadOp，未清除 aspect 保留 LOAD。
保留 STORE、屏障、缩放、重复槽删除和自动初始化；显式清除覆盖自动首次写入值。
DiscardColor 仍执行零清除。render-disabled、快照失效、alias_before_write 仍在原命令路径执行。
LARECOMP_LOG_PASS_CLEARS=1 每两秒记录已执行路径及合并 aspect 数；vk_diag 另记录逐项请求和合并结果。
普通 Release 剔除两个开关，不在玩家设置中暴露。

**没有跨命令 pending-clear 状态。** 请求在当前调用完成录制，worker 深复制 Vulkan 参数，
采样、复制、CPU 回读、别名、目标变化与提交沿用原顺序。
连续清除的后续项仍可能使用 ClearAttachments；这不是完整的 DXVK 式批量延迟清除。
扩大范围前须统计顺序/附件访问/实际命中，覆盖新增资源落实点。

## 本机验证与产物

全部在 distrobox dev：

    source android/dev_env.sh
    bash tools/test_pass_clear.sh .work/pass-clear-check .work/totk-port/game/romfs.bin
    bash tools/test_descriptor_updates.sh .work/pass-clear-descriptors
    bash tools/test_vk_command_worker.sh .work/pass-clear-worker

- 实际 exec.c pass/scissor/clear dispatch 的 ASan/UBSan，通过默认/关闭/开启、普通构建拒绝环境参数、第一写入覆盖、
  已有 pass/目标变化、2× 物理缩放、mip/layer、重复槽、缺失 aspect、空/局部/部分掩码和 STORE/屏障回归。
- **120 组真实 GPU 比较通过**：Radeon 780M / RADV PHOENIX，强制 Khronos 验证层与同步验证。
  float/uint/sint、零清除、depth/stencil 分离、局部/较小 area、两个 mip/三个数组层，
  传统/动态 pass 与 worker 关闭/开启，独立期望值及逐字节读回相同，warning/error 为零。
  GPU 专项验证清除数据，部分掩码的原 draw 路径由 CPU dispatch 覆盖，不等于游戏画面、LRZ 或 FPS 验收。
- 实际 stage_set、描述符失败/回退、原写入/模板/模板+worker GPU 值，以及 worker 参数/请求队列、交接、提交、复用和停止通过。
  本轮未新增并发算法，未重复 TSAN。
- Release/8 Elite/perftest 与普通 Release/8 Elite 的 O3/ThinLTO 构建、实际 ELF 新旧诊断开关隔离、
  Release 调度抢占回归通过。

最初 GPU fixture 遗漏传统 render-pass cache 的退出清理，验证层报告测试资源泄漏；
补齐 fixture 清理后最终矩阵零消息，没有关闭验证层或将测试泄漏视为手机崩溃。

冻结于忽略目录 .work/depth-clear-20261030/package-{perftest,release}/，完整哈希和日志保存在该目录。
两个包均匹配 GAMELIB_ID=466fe664cb16f8b0，复用完整 O3 游戏库
f2f5b244938709aa4e478ca0e1347cbf20d8441abe0ba4c6a759a2cc67f239c1，未重建或改写身份旁文件。
译码器、键和编译器不变，精确源码 SPIR-V 缓存可复用。
现有 SHADER_BUILD_ID 包含 exec.c，本次准备身份变化会重新核验旧收据，没有伪造收据或批准缓存别名。
手机最后安装包仍为此前 87006772…，新主机 packages 未部署。

后续保持默认关闭，用户恢复手机授权后，以相同路线、真实 2× / 60 目标和冷热缓存做单项/组合画面对照。
先确认深度清除合并频率、LRZ 和 GPU 时间，再扩大批量范围或改变默认值。
不降低分辨率、目标帧率或精度，不跳绘制、删同步或盲调已有负结果。

## 同日手机补测

用户恢复授权后，最新 667f3f3 测试包在一加 13 / Turnip 上完成真实 2×、60 目标、草地 15 秒转视角单项对照。
关闭 shader/pipeline 后台预热，loadOp clear 开并启用每两秒清除计数：20.704 FPS、p95 58.976 ms；
不开实验的对照为 20.946 FPS、p95 61.571 ms。日志确认颜色和深度合并命中，仍保留附件清除，skipped=0、正常退出。
本轮没有 LRZ/GPU 耗时隔离、全面画面对照或长路线验收，不能从命中次数推算速度，继续默认关闭。
最新身份/温度及采样边界见 [手机补测](PHONE-SHADER-EXPERIMENTS-20261030.md)，上文“未操作手机”为此前代码阶段历史。
