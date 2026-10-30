# 2026-10-30：描述符更新模板与分类比较

目标保持一加 13 / Snapdragon 8 Elite 的真实 2× / 60 FPS，包括转视角。用户本轮仍要求先写代码，不操作正在使用的手机。
本记录承接 [纹理负载与对照](PERFORMANCE-TEXTURES-20261030.md) 和 [独立录制线程](PERFORMANCE-VULKAN-WORKER-20261030.md)，
本轮没有手机测量，也没有新的 FPS 收益结论。

## 实现

原 `stage_set()` 每次解析资源后构造 `VkWriteDescriptorSet`，对全部描述符做两次 64 位哈希，查询逐提交缓存；
缓存未命中时分配 set 并调用 `vkUpdateDescriptorSets`。新实验路径为每个实际使用的程序/set 延迟创建 Vulkan 更新模板，
按既有反射计划把描述符值放入紧凑数组。命中路径不再构造写入结构和计算两次哈希。

`descriptor_update.{c,h}` 根据**本次实际解析的值**比较四类：uniform buffer、storage buffer、combined image sampler、storage image。
某一类已有变化后跳过该类剩余比较；所有类都不变且提交序号一致时复用上次 set。缓冲内容上传、纹理句柄/视图解析、
storage image 分配、缩放表与 tint 的快照仍逐次执行，因此命中不会跳过 CPU/GPU 内存保护或纹理更新。
这里的分类脏标记用于描述符值比较，尚未实现“仅凭 NVN 命令脏位跳过全部资源解析”。

内容改变或进入新提交时，调用原分配器获得**新的 set**，再用 `vkUpdateDescriptorSetWithTemplate` 写入完整模板。
不原地修改之前绘制已经引用的 set，也不跨提交复用来自可能已重置的 descriptor pool 的 set；GPU 资源和 transient UBO
继续遵循原完成序号。动态 UBO 描述符仍归零 offset，按 binding 排序的实际偏移留在绑定命令里；只改变动态偏移可以复用 set。

模板绑定顺序来自原 `descriptor_plan`，普通扫描与已分区扫描均作回归。顶点和片元各自保留状态，计算程序沿用自己的布局。
模板仅由渲染生产者创建/使用，后台管线预热不创建它。程序释放时先销毁模板、清理状态，再释放布局；重启沿既有
翻译/编译排空、录制 worker stop、提交和 GPU 完成等待后回收的流程。

概念参考为此前审核的 Ryujinx
[`475615f0431b4995b03c930c5089d20f6f3570e0`](https://git.ryujinx.app/ryubing/ryujinx/commit/475615f0431b4995b03c930c5089d20f6f3570e0)
的 `src/Ryujinx.Graphics.Vulkan/DescriptorSetUpdater.cs` 分类状态，以及 Vulkan 1.1 descriptor update template API。
本模块独立实现，没有复制 Eden GPL 的 payload 队列/调度器或移植 Ryujinx 的 C# 后端。

## 开关与回退

`LARECOMP_VK_DESCRIPTOR_UPDATES=1` **默认关闭，仅 Android 诊断包编入**；可与 `LARECOMP_VK_WORKER=1` 独立对照。
普通 Release 剔除模块、状态、开关与入口；内部接口隐藏，游戏库 ABI/生成器身份不变。
`LARECOMP_NO_SET_CACHE=1` 在新路径也禁止复用；原 `LARECOMP_VK_DESCRIPTOR_PLAN` 开关仍可独立选择。

模板 PFN 不可用、类型不支持、状态分配或模板创建失败时保留原描述符写入路径。创建失败的状态在程序释放时回收；
绑定个数/顺序与反射计划不一致则明确报错。首个有效模板记录启用日志，失败日志有预算。
现有 `prof_set_hits/miss` 与 `prof_dsw` 继续计数/计时，未增加常规高频输出。

新状态只保存每个程序/set 最近一次的内容，不使用原 4096 槽哈希表，因此同一程序的 A→B→A 材质切换可能增加分配；
比较和 payload 复制也仍有成本。没有凭 API 调用减少推算实机加速，默认值等待实际对照。

## 实际验证

所有测试在 `dev`，不需要手机或游戏输入：

```bash
source android/dev_env.sh
bash tools/test_descriptor_updates.sh .work/descriptor-updates-20261030/tests
"$PYTHON" tools/test_shader_lifetimes.py .work/descriptor-updates-20261030/lifetimes
TOTK_WORKER_TSAN=1 bash tools/test_vk_command_worker.sh .work/descriptor-updates-20261030/worker
```

- 实际 `stage_set` 与正式模块以 ASan/UBSan 执行原写入、模板、创建失败、状态分配失败、禁用缓存和 descriptor plan 六种模式。
  覆盖 binding 写入值、动态偏移排序、命中仍解析输入、四类脏位、旧 set 不被改写、顶点/片元隔离及新提交不能复用旧池。
  模块另测 0/96/97 边界、重复 binding、未知类型、缺失 PFN、创建失败和重复销毁。
- Radeon 780M / RADV PHOENIX / Vulkan 1.3 上，原写入、模板、模板+worker 各三次。
  使用真实 UBO/storage buffer、整数采样纹理与 storage image，逐项检查计算/图像值、动态偏移、内容变化后新 set、
  提交1/2、descriptor/command pool 复用和停止。同步验证警告/错误均为零。
- 实际程序销毁函数在普通与 Android 诊断两种构建各测试 100 次重启；诊断路径额外销毁 600 个模板，全部一次释放、无泄漏。
- 既有 worker ASan/UBSan、真实请求队列、GPU 与 TSAN 回归通过；31,040 个 descriptor plan 等价用例与上传缓存回归通过。

两种 Release / 8 Elite 包构建、实际 APK `expect on/off` 诊断隔离、普通 Release 调度抢占保留检查均通过。
本机冻结产物如下，**没有安装到手机**：

| 包 | APK SHA-256 | libmain SHA-256 |
| --- | --- | --- |
| perftest | `00f21f36b20a9b95479220763edaa7a3bc8bbe7145faa848c5a537a17e07f604` | `d5f449066f88459a3fd396ab62105f02f3a2255e9d0e6097abf7703a88f9dbca` |
| 普通 Release | `cb06febfa5c81c5029e121d97c5d35e2e8fb6153b76f68cc98e12bc9cea288ab` | `911015f3d41299db553b0bfd04237d6b17f78d970940ffb1017c1b692c68c317` |

两包 `GAMELIB_ID=466fe664cb16f8b0`，配对既有 O3 库 SHA
`f2f5b244938709aa4e478ca0e1347cbf20d8441abe0ba4c6a759a2cc67f239c1`，没有重建游戏库。
冻结包、源码身份和日志保留在忽略的 `.work/descriptor-updates-20261030/`。
本轮代码与测试提交为 `02a63d3`，基于 `152b5d0`，未推送。

## 仍待验收

本轮未查询或操作手机。Android/Turnip 的画面、长路线、连续切换和实际 FPS/长帧对照仍待用户允许实机测试时验收。
分类脏位跳过资源解析、跨提交的持久缓存、push descriptors/descriptor buffers 没有实现；不把当前模板等价于整套后端优化。
前序首次落水后的持久 20 FPS 仍待复现，真实 2×60 总目标尚未达成。
