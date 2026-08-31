# 第二十五轮：受控负样本与数值分辨率门

## 目标

前几轮主要验证正确圆角能否被串成复杂链。本轮开始验证相反问题：存在修剪缺口、位置错位、
极短边或薄片面时，算法不能因公共支撑、Sewing 容差或复合连接而把明显断开的几何重新拼成
“完整圆角”。

## 新增算法防线

增加两个与单位无关的阈值：

- `minimumResolvedEdgeLengthToModelDiagonal`，默认 `1e-8`；
- `minimumResolvedFaceAreaToModelDiagonalSquared`，默认 `1e-12`。

非退化边长度除以模型包围盒对角线，面面积除以对角线平方。低于阈值的对象分别计入
`shortEdgeCount` 和 `sliverFaceCount`。薄片面以
`RejectedNumericallyUnresolved` 明确拒绝，不再进入候选图。

候选面之间若只通过低于阈值的共享边接触，该 Link 会保留在报告中，但
`rejected_short_shared_edge=true` 且 `used_in_graph=false`。算法还记录这对面，禁止后续“共同外部
支撑面”的复合连接将其绕过门控重新接回。

CLI 对应参数为：

```text
--min-edge-to-diagonal number
--min-face-area-to-diagonal-squared number
```

## 受控负样本

| 样件 | 扰动 | 期望 | 实际结果 |
|---|---|---|---|
| generated-trim-gap-bspline-fillet-ring | 一个四分之一面的端部留 `0.001 rad` 修剪缺口 | 闭环必须断开 | 4 面仍为一条开放链，`closed=0`，总证据为不足 |
| generated-offset-bspline-fillet-ring | 一个四分之一面平移 `0.25` 模型单位 | 错位面不得留在完整闭环 | 拆成 1 面与 3 面两条链，均非闭环 |
| generated-sliver-fillet-patch | U 跨度仅 `1e-8 rad` 的圆环片及两支撑片 | 严格分辨率门下全部排除 | 3 个薄片面全部诊断并拒绝，0 链、0 Feature |
| generated-closed-bspline-fillet-ring | 将边阈值故意提高到 `0.30` | 验证短边门无法被复合连接绕过 | 6 条短边被诊断，4 个圆角面分为 4 条单面链 |

前三个新 BREP 位于：

`D:\workspace\cad-algorithm-build\debug\test-artifacts`

短边的 `0.30` 和薄片面的 `1e-6` 是回归测试专用的强制阈值，用来确定门控代码路径确实生效，
不是推荐生产参数。生产默认值保持保守，只过滤相对模型尺度已经接近数值噪声的对象。

## 正常模型回归

默认阈值下重新运行 generated-variable、generated-branched、bottle、CrankArm、bearing、hammer、
screw、linkrods 和 Motor-c：

- 所有模型均为 `short-edges=0`、`sliver-faces=0`；
- 链数、路径数、Feature 三态、半径行为和方向桥接统计与第二十四轮一致；
- CrankArm 保持 1 个解析桥接通过、平均 Feature 置信度 0.5900；
- linkrods 保持 8 个解析桥接冲突、平均置信度 0.7072；
- bearing 保持 3 个冲突 Feature、34 个证据不足 Feature；
- screw 仍无通过支撑门的候选。

因此新防线默认不会改变当前正常语料，只在达到配置的数值分辨率下限时介入。

## 报告变化

- `summary.json` 新增 `short_edge_count` 和 `sliver_face_count`；
- `faces.csv` 新增 `area_to_diagonal_squared` 与 `numerically_resolved`；
- `links.csv` 新增共享边绝对长度、归一化长度及短边拒绝标志；
- CLI 摘要新增短边、薄片面和数值不可解析拒绝数。

标准报告位于 `D:\workspace\cad-algorithm-reports\iteration25`，门控路径的专用报告位于其
`short-edge-audit` 与 `sliver-audit` 子目录。

## 回归与剩余限制

Visual Studio 2019 Debug 构建成功，自动化测试包含所有新增负样本。修剪缺口测试明确断言不存在
四面闭环；错位测试断言错位后不存在完整闭环；短边测试同时断言 Link 被拒绝且复合连接不能重新
合并；薄片测试断言诊断计数大于零且不产生圆角链。

当前扰动是确定性的拓扑/几何负样本，尚未覆盖控制点级随机噪声分布，也没有对阈值附近进行批量
扫描。下一轮应把解析桥的有符号方向代价纳入全局动态规划，并用从阈值下方到上方的参数扫描验证
判定单调性与稳定区间。
