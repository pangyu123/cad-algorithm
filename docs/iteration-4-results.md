# 圆角识别改进：第四轮结果

## 本轮目标

本轮不继续凭候选数量调阈值，而是先建立可审计、可复现的评估闭环：

- 每个面增加由量化几何特征计算的 `persistent_id`；
- 导出逐面证据、连接、链、最大非分支路径和模型摘要；
- 导出 Graphviz 连接图和仅包含已接受候选面的 BREP；
- 生成可直接填写的人工真值 CSV 模板；
- CLI 增加 `--report-dir`，报告位置可明确放在 D 盘。

## 输出文件

每个模型目录包含：

| 文件 | 用途 |
|---|---|
| `summary.json` | 模型尺度、容差、候选面/链/路径统计 |
| `faces.csv` | 半径、曲面来源、支撑面、置信度、拒绝原因等逐面证据 |
| `links.csv` | 公共边、G1、半径相容、共享支撑对等连接证据 |
| `chains.csv` | 连通分量级链路结果 |
| `paths.csv` | 分叉图的最大非分支路径 |
| `graph.dot` | 可视化候选连接图 |
| `truth-template.csv` | 人工标注真值模板 |
| `accepted-faces.brep` | 在 OCCT/CAD 查看器中直接检查候选面 |

## 重复运行验证

对 `bottle.brep`、`bearing.iges`、`hammer.iges` 分别运行两次，并比较上述八类文件的
SHA-256。共 24 组比较全部一致，包括 BREP、CSV、JSON 和 DOT，说明相同 OCCT 7.8.1
导入流程下结果与几何指纹可重复。

报告位置：

- `D:\workspace\cad-algorithm-reports\run1`
- `D:\workspace\cad-algorithm-reports\run2`

## 当前判断

- `bearing.iges` 仍输出 41 个候选面、32 个连通链、37 条路径；
- `hammer.iges` 仍输出 15 个候选面、13 个连通链、13 条路径；
- `bottle.brep` 仍输出 49 个候选面、3 个连通链、105 条路径，其中 104 条接触分叉节点。

这些数字现在可以逐面复核，但还不能证明准确率。下一阶段应先在
`truth-template.csv` 标注正例、负例和链归属，再计算 precision/recall，并针对 bottle 的
高连接度从 `links.csv` 判断是链定义、节点定义还是支撑面对连接造成的过连接。

## 标识边界

`persistent_id` 不依赖 OCCT 的临时 face 序号，适合比较同一几何在重复运行中的结果。
但经过大幅修复、重新参数化或单位缩放后，量化几何特征可能变化；完全重合的装配实例也只能
通过确定性后缀区分。因此它是评估用几何指纹，不是 CAD 数据管理系统中的永久实体 GUID。
