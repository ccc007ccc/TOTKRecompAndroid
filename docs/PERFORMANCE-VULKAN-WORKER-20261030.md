# 2026-10-30：独立 Vulkan 录制线程

目标保持一加 13 / Snapdragon 8 Elite 的真实 2× / 60 FPS，包括转视角。前序纹理对照与游戏间隔修正没有
证明达成该目标，负结果分别保留在 [纹理记录](PERFORMANCE-TEXTURES-20261030.md) 和
[间隔记录](FRAMEPACE-20261030.md)。用户本轮要求先写代码、不要操作正在使用的手机，因此只做容器构建和主机验证。

## 实现与边界

已有 `async.c` 把 NVN 命令解析移到了渲染线程，但解析、资源准备和驱动录制仍在同一条 CPU 路径上。
新增 `command_worker.c` 让渲染线程复制 Vulkan 录制参数，由第二个线程按顺序调用 `vkCmd*`，以便与 NVN 解析重叠。
资源创建、描述符更新、提交和 GPU 等待继续由原路径执行；这不是完整的后端线程迁移，也没有减少 GPU 绘制量。

概念参考来自前序已审核的 Eden
[`a2aa4d97e10a8345babfe647e1abd5ed5909c623`](https://git.eden-emu.dev/eden-emu/eden/commit/a2aa4d97e10a8345babfe647e1abd5ed5909c623)
的 `src/video_core/renderer_vulkan/vk_scheduler.{h,cpp}`：解码和 Vulkan 录制分阶段排队。
本项目使用独立编写的 C 队列与 Vulkan 参数生成器，没有复制 Eden 的 GPL 类、命令块或调度实现；
不照搬其最高线程优先级与性能核绑定策略。

- 单生产者、单消费者；16 个 64 KiB 槽分配共 1 MiB，达到约 4 KiB 时发布一批。槽满等待消费者退回，命令不丢弃。
- 指针数据复制进包内，包括 descriptor sets、dynamic offsets、vertex buffers/offsets、复制区域、清除值，
  以及 rendering / dependency / render-pass begin 结构的嵌套附件与三类 barrier 数组。
- 生成器扫描 Vulkan 源码与头文件，按实际 Vulkan PFN 声明生成当前 57 个录制调用及 27 个同步边界。
  未分类的指针或数组元素会使生成失败；超过单槽容量、未知 `pNext` 等先排空，再执行原调用。
- 函数形式宏只包装项目调用；Volk 全局 PFN、裸指针可用性检查及 core/extension 兼容别名保持原语义。
- Begin/End/Reset/Allocate/Free command buffer、command pool、提交、idle、descriptor pool/sets 复用和显式资源销毁
  先等待录制完成。录制完成与 GPU 完成仍是两件事，原提交序号、快照和 GPU 资源寿命保护继续生效。

VMA 通过自己的 PFN 释放资源，不能依赖 Vulkan 调用宏覆盖。既有按 GPU 完成序号回收的资源继续使用该序号；
请求回调前后另排空 worker，保护经回调发生的间接释放与同步返回。`vk_async_drain()` 在 NVN 请求完成后重新检查
worker 是否刚启动，并通过有序请求让生产者发布和排空暂存数据；渲染线程内部重入则直接排空。
线程启动标志改为原子发布，创建失败明确终止原请求队列的启动流程。

ImGui 使用自己的 Vulkan PFN，进入其绘制前交回 command pool。重启在渲染生产者上排空、stop/join，随后沿原流程
提交和等待 GPU，再释放后端对象；没有把 join 放进 SDL event watch。

## 实验开关与测量口径

`LARECOMP_VK_WORKER=1` **默认关闭，仅诊断构建可用**，而且只在异步 NVN 渲染线程上启动。
普通 Release 不编入 worker、包装入口及开关字符串，不因环境变量而启用它。内部接口隐藏，不扩大游戏库 ABI。

启用 `LARECOMP_VK_PROF` 后，worker 单独累计线程 CPU 时间及排空等待墙钟；未启用时只报告命令、批次及回退计数，
不把未测量的时间打印成零。此 CPU 时间不是 GPU 执行时间，也不能直接与原渲染墙钟相加推算 FPS。
当前仍有批次复制和同步边界开销，是否改善一加 13 的吞吐与长帧必须另测。

## 本地验证

在 `dev` 内、仓库根目录执行：

```bash
source android/dev_env.sh
TOTK_WORKER_TSAN=1 bash tools/test_vk_command_worker.sh .work/vulkan-worker-20261030/tests
bash tools/test_texture_registration.sh .work/vulkan-worker-20261030/texture-regression
```

本轮实际通过：

| 验证 | 实际覆盖 |
| --- | --- |
| 生成器 3 项回归 | 头文件调用扫描、确定性输出、未知指针与未来含指针的数组元素拒绝 |
| CPU ASan/UBSan 与 TSAN | 消费者阻塞后改写/释放调用者参数，嵌套附件/barrier 深复制、动态偏移、顺序、同步回退、40,003 draws 的背压与槽复用、stop 排空与重启、每个槽的分配失败及线程创建失败、VkResult 与裸 PFN |
| 实际 Android 请求队列 ASan/UBSan 与 TSAN | 排空开始时 worker 尚未启动、首个 EXEC 后启动的竞态；回调前后交接、嵌套同步回调、渲染线程自身排空及 stop |
| 真实 Vulkan 1.3 / RADV PHOENIX / Radeon 780M | worker 关/开各 3 次计算与图像结果，动态 UBO 偏移、颜色/深度/模板 rendering 附件、同池第二个 command buffer 交接、Submit1/2、pool 复用和 stop；同步验证警告/错误均为零 |
| 既有纹理发布回归 | 同步/排队/default、视图值复制、引用寿命、对象锁与真实请求队列；worker 关闭时保留旧行为 |

Release / 8 Elite / perftest 与普通 Release / 8 Elite 均构建成功；实际 APK 通过新增 worker 开关及隐藏接口检查的
诊断隔离 `expect on/off`，普通 Release 保留调度抢占的 CPU 回归也通过。

| 本机产物 | APK SHA-256 | libmain SHA-256 |
| --- | --- | --- |
| Release / 8 Elite / perftest | `bdf1ee216d947f2a2125a78d5ea5ae081ae7a98469efae76f4c52b9433262003` | `598bf462bdf868fcacc22e1579dd7d442a3a6bebf2188881f8e8d3c582ff3989` |
| 普通 Release / 8 Elite | `2144452799f5990c3cf6ec0798ffb94e023cf95083a00c816b3aaafbf2659d84` | `ecf30ea6e847ece69e1ac5c5dc422eb6d0b3015f08c92dc49b07252296300c12` |

两包 `GAMELIB_ID=466fe664cb16f8b0`，主机 package 目录中的配套库均仍是既有 O3
`f2f5b244938709aa4e478ca0e1347cbf20d8441abe0ba4c6a759a2cc67f239c1`，没有重建整库。
冻结产物、源码身份和日志保留在忽略的 `.work/vulkan-worker-20261030/`。
本轮代码与测试提交为 `70f69af`，基于 `08aa2f1`；没有推送或安装本轮产物。

## 剩余验收

本轮没有 ADB 查询、安装、启动或测量，没有更新任何手机包。主机 GPU 正确性与线程回归不能代替 Android/Turnip
的画面、连续切换、重启和长路线验收；当前没有 worker 的手机性能数字，不能批准默认开启或宣称提升到 60 FPS。
后续代码仍包括实际描述符更新模板与分类脏标记；既有逐提交缓存和 descriptor plan 不等于这些工作已完成。
首次落水后的持久 20 FPS 仍待复现，真实 2×60 的总体目标继续保留。

同日后续：已实现并本地验证更新模板与按解析值分类比较，见 [描述符更新记录](PERFORMANCE-DESCRIPTOR-UPDATES-20261030.md)。
两个实验仍默认关闭，上方 worker 阶段的产物/结果保留，不扩大为手机收益结论。
