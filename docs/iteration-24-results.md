# 第二十四轮：解析中间面的方向桥接

## 目标

第二十二轮只在相邻的两个自由曲面圆角面都有流线时验证路径方向。若两条流线之间夹着圆柱、
恢复圆柱或圆环面，连接会标记为 `unobserved-intermediate-faces`。这种处理不会制造假冲突，但会
让真实的混合解析/自由曲面圆角链长期停留在证据不足。本轮使用现有 Link 的解析方向证据逐跳补齐
这段空白。

## 桥接规则

仅当路径中被跨过的面全部属于以下解析圆角几何时才尝试桥接：

- 解析圆柱；
- 从一般曲面恢复出的圆柱；
- 解析圆环面。

算法按路径顺序检查从前一条流线面到后一条流线面的每一跳：

- 每一跳必须找到已经执行过方向判定的 Link；
- 每一跳的全部相关 Link 都兼容，桥接才通过；
- 任一已评估 Link 不兼容，桥接明确判为 `direction-mismatch`；
- 缺少 Link 或存在未评估 Link 时，不猜测结果，仍保留
  `unobserved-intermediate-faces`。

这里对同一面对之间的多条共享边采取保守聚合，避免只挑选一条有利边掩盖周期缝或局部反向证据。
闭环跨越序列也按相同规则处理。

## 新增回归模型

新增 `generated-mixed-analytic-bspline-fillet-ring.brep`：同一个闭合圆角环由三个 BSpline 四分之一
圆环面和一个解析圆环面组成，并由两个圆柱支撑面限定。运行测试时关闭标准曲面恢复，确保三个
BSpline 面确实走自由曲面流线分支。

该模型得到一条四面闭合路径。三条自由面流线之间有一个连接跨过解析圆环面，桥接包含两跳，
两跳均有兼容方向证据，因此方向状态由证据不足升级为 `validated`。测试件位于：

`D:\workspace\cad-algorithm-build\debug\test-artifacts\generated-mixed-analytic-bspline-fillet-ring.brep`

## 模型结果

| 模型 | 解析桥接通过 | 解析桥接冲突 | 未观测中间面 | 结果 |
|---|---:|---:|---:|---|
| generated-mixed-analytic-bspline-fillet-ring | 1 | 0 | 0 | 闭环方向证据通过，Feature 置信度 0.925 |
| CrankArm | 1 | 0 | 0 | 面 33→14 经面 16 的两跳桥接通过 |
| linkrods | 0 | 8 | 0 | 原八个缺证连接均发现明确方向冲突 |
| generated-closed-bspline-fillet-ring | 0 | 0 | 0 | 原四条直接连接保持通过 |
| bearing | 0 | 0 | 0 | 无需解析桥接，结果不变 |

CrankArm 的已接受方向转移由 5 条直接/桥接证据组成，其中一个原本未观测的连接已补齐；其平均
Feature 置信度由 0.5806 提升为 0.5900。它的十个 Feature 总状态仍为证据不足，原因是拓扑角色
证据尚未完全验证，而不是方向桥接失败。

linkrods 的结果说明桥接不是简单地把“未知”变成“通过”。八段候选桥都找到了至少一条明确不
兼容的逐跳 Link，因此升级为可审计的方向冲突。其两个冲突 Feature、四个证据不足 Feature 和
平均置信度 0.7072 保持不变，但失败原因比上一轮更明确。

其余生成模型和真实模型的链、路径、半径行为与第二十三轮一致，没有因桥接增加新的连接或
Feature。

## 报告字段

`radius-orientation-transitions.csv` 新增：

- `analytic_bridge_attempted`；
- `analytic_bridge_hop_count`；
- `analytic_bridge_evaluated_hop_count`；
- `minimum_analytic_bridge_alignment`；
- `analytic_bridge_validated`。

`radius-profiles.csv` 汇总桥接通过数和桥接冲突数，`features.csv` 新增 Feature 级桥接通过数。
命令行摘要也会打印两项计数，因此无需解析明细 CSV 即可发现模型中的桥接行为。

## 回归与限制

自动化测试覆盖混合解析/BSpline 闭环，并断言恰好一个桥接通过、零未观测连接及 Feature 方向状态
为 `validated`。Visual Studio 2019 Debug 构建成功，`ctest` 结果 1/1 通过。完整报告位于：

`D:\workspace\cad-algorithm-reports\iteration24`

当前全局二状态方向动态规划仍先用自由面流线端点成本选择方向，再用解析桥逐跳验证所选方向；
解析桥的有符号代价尚未直接进入动态规划目标。下一轮可构造受控错位、噪声和修剪缺口负样本，
并将桥接代价纳入全局方向优化，以验证接近阈值时的鲁棒性。
