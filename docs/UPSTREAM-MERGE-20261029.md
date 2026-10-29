# 2026-10-29 上游选择性整合

基于 `5dc2e7c`，吸收上一轮审阅确认的四项改动：IADD3 移位/半字选择、QUAD_STRIP、全局内存区一致性和共享着色器清单。
这次没有整体合并 BOTW 分支；保留 TotK 1.4.3 的输入、计时、草地修复、GPU 复制/纹理队列默认策略和普通应用数据。
私有构建、扫描和测试记录在 `.work/upstream-merge-20261029/`。

## 来源与适配

| 来源 | 落地行为 |
| --- | --- |
| [BOTWRecompAndroid 2901800](https://github.com/mamcjr/BOTWRecompAndroid/commit/2901800f6b1327a631ba49bfcede6454eafee8fa)，原始 [SavageWindRecomp 7ce70fa](https://github.com/Revivedx/SavageWindRecomp/commit/7ce70fa227ef76be9db1c3cecc41101b1a4b787d) | IADD3 寄存器形式先选择半字，再取负；a+b 按 32 位回绕并执行 `.RS/.LS`，最后加 c。保留原 carry 分支、立即数/常量寻址和无 Int64 输出。原 MIT 许可证保存在 [LICENSE-SavageWindRecomp.txt](../runtime/nvn/maxwell/LICENSE-SavageWindRecomp.txt)。 |
| [MacNativeGames bff585f2](https://github.com/ly05010419/MacNativeGames/commit/bff585f222a0e81651c214768324f4135fd7019b) | mode 8 按相邻顶点对展开四边形条带；两三角形都把 NVN 的最后顶点 d 旋转至 Vulkan 的第一个顶点，保留绕序和平面属性。支持 8/16/32 位、非对齐输入、自定义重启索引和各条带独立的奇数尾。 |
| [BOTWRecompAndroid 056a9a0](https://github.com/mamcjr/BOTWRecompAndroid/commit/056a9a0d3f71d1e8d58ca44e0ac31a4bc9740677) | 全局 arena 要求 HOST_COHERENT；不支持时走既有回退，成功分配却没有映射时回收资源。普通读回保留本地 barrier/wait/invalidate，不改成上游的强制一致内存方案。 |
| [MacNativeGames 032a05df](https://github.com/ly05010419/MacNativeGames/commit/032a05dff7215177912edfb2d01c9e20089d92a7) | 同一固定版本的 parser、seed planner、LICENSE 和清单一并核对，更新来源与摘要；前三份文件内容未变。保留正式 Vulkan 适配器的类型、阶段和配对规则。 |

QUAD_STRIP 复用 MIT 上游的顶点对规则，按 Vulkan 的 provoking vertex 规则调整三角形顺序；
不沿用 Metal 实现对 GPU 写入索引直接跳过绘制的做法。索引的 CPU 展开继续在已有 GPU 复制预检查之后，
等待发生在取得绘制命令缓冲之前；primitive restart 已在展开时消费，不对展开后的三角列表启用硬件重启。
分配按 64 位计算容量，检查 Vulkan 的 32 位索引数量限制，并初始化上传尾部四字节。

## 实际输入和清单增量

生产目录对自有 1.4.3 RomFS 的静态扫描为 28,634 个着色器输入，其中 27 个含 39 条 `IADD3.RS`：
37 条顶点、2 条计算，涉及洞穴、草和部分绘制程序。它是静态出现次数，不证明这些指令在当前场景的执行频率，
也不推翻此前用户“草好了”的反馈。

| 指标 | 旧固定清单 `f024ef04` | 新固定清单 `032a05df` |
| --- | ---: | ---: |
| 共享 warm 记录 | 70,971 | 73,397 |
| 匹配 1.4.3 的 warm 记录 | 38,390 | 39,757 |
| 1.4.3 可移植模块请求，按类型/配对去重 | 15,160 | 15,206 |
| 其中标题请求 | 446 | 446 |
| 原始 seed planner 请求 | 28,770 | 28,770 |

新增匹配记录 1,367 条，正式适配器去重后只增加 **46 个模块请求**，没有删除旧请求；标题子集不变。
模块请求仍会按正式翻译的源码键进一步复用，不代表新增 46 份最终 SPIR-V，更不代表现成 Vulkan 管线或已测得帧率收益。

翻译器变化进入 `SHADER_BUILD_ID` 和准备身份；实际变化的 GLSL 使用新的源码键。
`SHADER_CACHE_ID` 的编译契约未改，未修改兼容白名单或清空旧缓存。既有源码完全相同的模块仍可准确命中。
本次未改变游戏库 ABI/生成器，复用配套 O3 游戏库；实际产物身份与实机范围见下。

## 验证

- 整数回归先在旧代码上失败：半字选择样本实际 `0x8000001f`，期望 `0x0001001f`。
  修复后 710 个指令组合、2,908,160 个算术/地址向量通过，覆盖三操作数的半字选择、符号、移位和旧 carry 行为。
  三阶段共 2,130 份合成着色器编译通过，SPIR-V 中未引入 Int64 能力/类型；数值期望在 CPU 执行生产生成体时核对。
- `tools/test_primitive_indices.py`：ASan/UBSan 下验证条带绕序、平面属性顶点、跨 65,536 个四边形的连续性、
  重启/奇数尾、非对齐的三种索引宽度、非索引 first、旧 quads/fan/polygon 和容量拒绝。
- `tools/test_buffer_copy.sh`：既有复制、UBO/命令/通知回归通过；新增用正式 draw 预检查验证 GPU 复制的条带索引先就绪再供 CPU 读取。
- `tools/test_global_arena_memory.py`：模拟 0xb 非一致缓存内存、0x7 一致内存和 0xf 一致缓存内存，
  验证正确选择、失败回退、缺少映射时释放和不重复申请。
- `tools/test_shader_warm_list.sh`：7 项正式 parser/摘要/阶段/类型测试通过；真实 RomFS 目录校验通过，
  14,760 顶点、13,617 片元、257 计算输入。
- 编译缓存身份/兼容回归与准备计划/收据回归通过；保留损坏回退、原子替换和准确源码键匹配。
- 重新编译运行正式 `audit_shader_raw_keys.cpp`，自有 RomFS 的 28,634 个原始键与上一版逐字节一致；
  新清单通过正式 importer 的输入匹配。APK 内的 `assets/res/shader-warm.bin` 与当前正式生成结果逐字节一致，
  全版本共 39,926 个请求、965 个标题请求；上表是其中匹配 1.4.3 的子集。

## 构建与实机

Release / snapdragon-8elite / perftest 构建通过，固化产物位于私有 `package/`：

| 项目 | 身份 |
| --- | --- |
| APK SHA-256 | `dd648f800586841bbf15022148b1c654e9bee652519374385b30c62780377b99` |
| `libmain.so` SHA-256 | `f38f11ec44f4f441587e09e4acba2050fff8800a3b9dac63024cbcfc0b2bd4b8` |
| `GAMELIB_ID` | `993c0a5c82dc5099`，保持不变 |
| 复用游戏库 SHA-256 | `5f3156b208076afa8b07d831d538977e643083ed6014bfd8711760d5137f087c` |
| 设置种子 SHA-256 | `883b296224083bd8d3f049d15f72508799d2e5b265bd7ccadedf32c3b782ba5f` |

仅安装/运行一加 13 / PJZ110 的独立 `com.totkrecomp.android.perftest`。
Adreno 830 / Turnip Mesa 27.0.0-devel (`ab1502fd16`)，使用既有 `grass-recent-save-1793179059668422146/save-0` 的副本，
目标 60 FPS、动态分辨率开启、无 profiling、无暂停/截图，不显式覆盖资源优化开关。

`phone/benchmark-1793271164341614013`：就绪后 518 帧 / 15.026414 秒，**34.47263 FPS，p95 38.259375 ms**；
由运行时自行退出，错误字段为空。电池温度 34.6→35.0°C，电量 71→70%，AC/USB 均未供电。
条件与前轮充电/温度不同，不作跨版本性能收益对照。

缓存保留。新版准备身份使标题重新验证：446 个请求展开成 875 个源码请求、734 个模块，约 3.151 秒。
旧设备管线 recipe 因身份不同被忽略，随后记录 1,240 条当前状态；没有绕过身份检查复用。
日志确认纹理队列和延迟 GPU buffer-copy 仍按默认策略启用。

合成检查不代替完整足迹界面、跨区域或长期画面验收；Mali 内存选择在本轮只做模型验证，不宣称已做 Mali 实机测试。
