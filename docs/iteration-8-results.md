# 圆角识别改进：第八轮结果

## 主曲率方向连续性

对两个带状候选面的真实公共边，在边中点分别取得两侧曲面的 UV 参数，计算两个主曲率方向。
其中绝对曲率较小的方向作为局部圆角脊线方向，并使用方向无关的绝对点积比较：

`alignment = abs(directionA dot directionB)`

默认阈值为 `cos(15°) = 0.9659258263`。方向相反但共线时 alignment 仍为 1，避免受曲面参数方向
和面朝向影响。

## 证据字段

`Link` 和 `links.csv` 新增：

- `used_in_graph`
- `spine_direction_evaluated`
- `spine_direction_compatible`
- `spine_direction_alignment`

`Feature` 和 `features.csv` 新增：

- 已评估方向的有效连接数量；
- 方向冲突 Link ID；
- 平均方向一致度；
- `spine_direction_validated`。

方向冲突目前不会删除候选或断开图，而是将 Feature 置信度乘以 0.8，保留完整审计证据。

## used-in-graph 修正

算法会在 `links.csv` 保留部分被检查但未进入最终链图的邻接关系。本轮最初汇总时发现
CrankArm 的全部已评估连接最低 alignment 约为 0.032，但检查后确认低值来自未被采用的邻接边。
因此新增 `used_in_graph`，Feature 方向校验只统计真正参与链路构建的连接。

## 五模型回归

| 模型 | 进入图且完成方向评估的 Link | 最低 alignment | 平均 alignment | 方向验证 Feature | 方向冲突 Feature |
|---|---:|---:|---:|---:|---:|
| bottle.brep | 8 | 1.0 | 1.0 | 2 | 0 |
| bearing.iges | 0 | — | — | 0 | 0 |
| hammer.iges | 0 | — | — | 0 | 0 |
| screw.step | 0 | — | — | 0 | 0 |
| CrankArm.brep | 4 | 约 0.99999987 | 约 0.99999995 | 2 | 0 |

候选面、链、路径和 Feature 数量相对第七轮没有变化。

bearing 和 hammer 主要依靠几何支撑恢复形成孤立候选或无公共边关系，因此没有足够的公共边
主方向证据；结果保持未知，而不是错误标成通过或失败。

## 输出与测试

- 报告：`D:\workspace\cad-algorithm-reports\iteration8`
- OCCT 7.8.1 Debug 自动测试：1/1 通过

## 下一步

当前只在公共边中点取一个样本。工业模型中长边可能存在局部扭转，下一步应改成沿边多点采样，
记录最小值、均值、离散程度和有效样本覆盖率，并处理靠近脐点时主方向不稳定的问题。
