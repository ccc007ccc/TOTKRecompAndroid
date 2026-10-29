# 从游戏文件追踪着色器

2026-10-28 更新：下面的 8,697 份统计属于最初仅扫描 `Shader/` 的范围。实际启动暴露了 `Lib/agl/` 程序遗漏；
当前目录已扩展到 SDK、库、UI 和特效档案，共 28,634 份输入、23 个 FSHA 模型、2,614 个命名/独立 BNSH 程序组，
全部输入均有程序关联。库/UI 范围为 12,574 份；特效 BYML/PtclBin/VFXB 中的 BNSH 再增加 16,060 份唯一输入。
多重采样、图像读写和原子加法已修复，全部 28,634 份 FP32 翻译成功；特效反射结合原游戏格式初始化后，通用审计计划为 2,666,099 条目，PJZ110 系统驱动实际为 2,798,003 条目。原模型材质扫描统计保持原口径，
最新证据及覆盖边界见[准备流程](SHADER-PREPARATION.md)。

已在本地 1.4.3 RomFS 中恢复出以下对应关系，全部使用文件中的指针、索引和选项键关联，没有按文件名猜测配对：

```text
模型文件中的形状
  → 材质索引 + 顶点缓冲布局
  → 材质指定的着色器档案、模型、静态选项
  → FSHA 程序选择表中的候选程序
  → BNSH 中实际配对的顶点/片元代码
```

另一类 SHARCB 档案提供“程序名称 → 变体序号 → 阶段代码”的关系。
它包含草地交互、屏幕拾取、天空遮蔽等程序；序号对应的选项含义尚未从调用方恢复，工具不会自行命名。

## 实际扫描结果

| 项目 | 数量 |
| --- | ---: |
| 唯一代码/控制块输入 | 8,697 |
| FSHA 着色器模型 | 23 |
| FSHA 程序选择记录 | 23,524 |
| SHARCB 命名程序组 / 组内变体记录 | 66 / 896 |
| 能找到所属程序的代码输入 | 8,697，未关联 0 |
| 档案中明确出现的不同顶点/片元配对 | 8,632 |
| 检查的模型文件（`.bfres.mc` 和 `.bfres.zs`） | 13,876 |
| 模型 / 形状 | 12,395 / 55,778 |
| 材质 | 54,541 |
| 材质静态选项有匹配候选 / 未匹配 | 54,541 / 0 |
| 文件解析失败 | 0 |

材质候选涉及 3,574 份顶点代码、4,737 份片元代码和 8,172 种配对。
剩余 101 份顶点、278 份片元、7 份计算代码均能从 SHARCB 命名程序中找到归属。

“匹配”表示材质明确指定的静态条件与程序选择表一致。动态选项保持开放；未指定的静态选项也不会默认补成 0。
材质显式请求默认值时，才读取 FSHA 中记录的默认选项。布尔位转换为 FSHA 使用的 `0`/`1`。
目前候选数从 1 到 2,112 不等，候选仍可能包括游戏代码进一步筛选掉的组合。

## 一个具体例子

`Model/Animal_Bass.Bass.bfres.mc` 的 `Body__Mt_Body` 形状指向 `Mt_Body` 材质，
材质指定 `material` 档案的 `material` 着色器模型，即
`Shader/material.Product.140.product.Nin_NX_NVN.bfsha.zs`。

该材质的静态条件对应 120 条候选程序。选择记录 15208 是 `gsys_assign_material`，
15209 是 `gsys_assign_zonly`，15210 是 `gsys_assign_gbuffer`，15211 是 `gsys_assign_cubemap`。
这些记录明确指定各自的顶点/片元代码；编号只在本次档案内有效，不是跨游戏版本稳定 ID。

模型同时给出输入布局。例如 `_p0` 映射到位置 0、格式 `0x0515`（四分量半精度浮点），
骨骼索引 `_i0` 映射到位置 6、格式 `0x0309`（双分量无符号 8 位整数）。
缺少的属性保留 `format: -1`；需结合具体程序实际读取的属性，以及运行时提供的默认/额外输入继续核对。
工具不会把它们擅自补成浮点格式。

## 实现与复现

- `runtime/nvn/vulkan/shader_catalog.hpp`：FSHA 选项、属性位置、程序选择和 BNSH 阶段配对；SHARCB 命名程序及阶段顺序。
- `runtime/nvn/vulkan/shader_material.hpp`：MCPK 元数据、BFRES v10 外部字符串、形状/材质/顶点布局、选项映射及候选筛选。
- `tools/audit_shader_catalog.cpp`：完整静态关联审计，输出关系图与材质索引，不导出着色器代码。
- `tools/query_shader_catalog.py`：电脑端查询已有审计结果的辅助工具；手机解析和准备过程不依赖 Python。

MCPK 中的第一段是省略魔数的 Zstandard 帧，包含 BFRES 元数据。读取材质和布局不需要展开后续 FMSH 网格，
因此没有引入完整 MeshCodec 解码器，也不必生成解压后的模型副本。使用已有的原生 zstd 依赖。

在 dev 中执行：

```bash
clang++ -std=c++17 -O2 tools/audit_shader_catalog.cpp -lzstd \
  -o .work/totk-port/shader-links/audit
.work/totk-port/shader-links/audit .work/totk-port/game/romfs.bin \
  .work/totk-port/shader-links/complete
python3 tools/query_shader_catalog.py .work/totk-port/shader-links/complete \
  --asset Animal_Bass.Bass.bfres --material Mt_Body --limit 1
python3 tools/query_shader_catalog.py .work/totk-port/shader-links/complete \
  --named grass_shader/interact_cut --limit 1
```

输出为 `catalog.json`、`materials.jsonl`、`unresolved.jsonl` 和 `summary.json`。
`--catalog-only` 只更新程序关系图，不重扫模型；修改解析规则后应重新做完整审计。
报告中的 `runtime_state_complete` 始终为 `false`，不能用它生成 `.shaders-ready`。

布局参照了本地 TotK-Native 的 `shader_library.py`、`shader_archive.py`，以及以下公开实现：

- ShaderLibrary `790dd2e501926e536a9193907d1256d446104162`：FSHA 属性位置和选项键布局。
- BfresLibrary `65c005275deed1e085069e72fea523247f955b87`：BFRES v10 材质和顶点结构。
- MeshCodec `b3600954872dbc2893a1bcc9e851c1e364f303ab`：MCPK 容器和元数据帧布局。

## 验证与尚未完成的部分

`tools/test_shader_catalog.cpp` 用自造数据检查共享顶点程序、片元配对、动态选项键偏移、默认选项、
SHARCB 阶段顺序、属性缺失、索引表、MCPK 元数据解压及畸形输入拒绝。
真实语料检查逐字节比较旧提取器与新目录的全部 8,697 份代码/控制块输入，确保没有漏掉任何条目。
主机 ASan/UBSan 测试、PJZ110 手机上的相同原生测试和 APK 构建均通过；手机测试只读取已有 RomFS，
临时测试文件已清理，没有安装覆盖可玩 APK。

```bash
clang++ -std=c++17 -O1 -g -fsanitize=address,undefined tools/test_shader_catalog.cpp \
  -lzstd -o .work/totk-port/shader-links/test-catalog
.work/totk-port/shader-links/test-catalog .work/totk-port/game/romfs.bin
```

手机预编译器已改用保留关联信息的目录读取器，并在日志中记录实际配对、关联的片元程序及输入掩码数量。
`shader_variant_plan.hpp` 已据此核对具体程序读取的属性，保留已知类型、整数输入的未绑定默认路径，
并对缺失属性/未知格式枚举全部类型。无模型布局的程序保留通用组合；片元输入按档案中的程序配对生成。
上述原范围顶点类型组合为 29,791、清单 694,772 条目；当前扩展范围为 266,120 种顶点类型组合，通用审计为 2,666,099 条目、PJZ110 系统驱动实际为 2,798,003 条目，详见[准备流程](SHADER-PREPARATION.md)。
文件声明之外的动态接口仍可能未覆盖，片元输出格式也还保留保守枚举。
找到程序归属不等于完成全库 SPIR-V，也不等于恢复出每个效果的完整游戏调用逻辑。
