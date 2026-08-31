# 第二十六轮：解析桥接进入全局方向动态规划

## 目标

第二十四轮会在全局流线方向选定后验证解析中间面，因此动态规划使用的仍是两个远端自由面的直线
间隙与三维方向点积。对于弯曲的圆柱/圆环路径，这个端点代价不代表沿曲面的真实连续性。本轮让
动态规划和最终证据共享同一段解析桥方向信息。

## 符号不变的轴链

曲面主方向本质上是一条无向轴，OCCT 返回 `d` 或 `-d` 都正确。直接依赖单个向量符号会导致结果
随参数化或导入顺序变化。本轮按以下序列传播：

`自由面末端 → Link 第一侧 → Link 第二侧 → 解析面另一端 → ... → 下一自由面起点`

相邻轴的有符号点积沿桥相乘。任何内部轴都参与前后两个点积，因此单独翻转它会同时改变两个
符号，最终端点相对方向保持不变。对每条 Link 使用报告中已有的多点方向样本构造代表轴，并继续
用最低绝对 alignment 作为质量上限。

## 解析面内传播

首版实现曾直接比较解析面两端的三维轴。混合圆环回归立即暴露问题：四分之一圆环面的切向量会
自然旋转约 90°，直接点积仅得到约 0.02，虽然跨边连续性实际很好。

最终实现使用 Link 样本的 UV 锚点，在圆柱或圆环面内部沿最短周期参数方向分成 12 步，逐点重新
计算主方向并传播符号。解析面自身的平滑转弯只负责传递朝向，不降低跨边质量；质量惩罚仍来自
自由面端点接入和逐 Link 的最低 alignment。

解析桥转移代价为：

`逐跳局部归一化间隙 + 方向权重 × (1 - 桥接有符号 alignment)`

默认方向权重为 `0.10`，对应 `Options::radiusProfileDirectionCostWeight`，CLI 可用
`--orientation-direction-weight` 配置。桥上存在未评估 Link 或缺少方向样本时不会构造虚假代价，
而是回退原端点成本并继续把证据标为不足或冲突。

## 结果

| 模型 | 桥接尝试 | 进入优化 | 有符号 alignment | 总方向成本：25轮→26轮 | 证据结果 |
|---|---:|---:|---:|---:|---|
| generated-mixed-analytic-bspline-fillet-ring | 1 | 1 | 0.996257 | 0.644560 → 0.088671 | 1 个桥通过，闭环完全验证 |
| CrankArm | 1 | 1 | 0.999056 | 0.524200 → 0.086324 | 1 个桥通过 |
| linkrods | 8 | 6 | 约 `4e-12`～`2e-10` | 1.790450 → 1.713060 | 8 个桥仍为明确冲突 |

混合圆环单个解析桥自身的成本为 `0.0003743`。若继续使用远端直线，闭环跨过圆环四分之一周的
空间距离会被错误计入间隙；现在只累计两个真实共享边附近的局部 gap。

CrankArm 的面 33→14 经面 16 的两跳开放桥自身成本约 `9.44e-5`，说明圆柱轴向传播稳定。

linkrods 有六个桥拥有完整样本，因此进入动态规划；其有符号 alignment 接近零，与上一轮逐 Link
方向冲突一致。另两个桥包含未评估的方向 Link，保持端点回退，不伪造完整证据。全局反转的流线面
次数由 13 降到 7，但八个冲突没有因优化而被掩盖，Feature 状态仍为 2 个冲突、4 个证据不足，
平均置信度仍为 0.7072。

generated-variable、generated-branched、bottle、bearing、hammer、screw 和 Motor-c 没有可用解析
桥，其链、路径、半径分类、Feature 三态和方向成本保持不变。CrankArm 与混合圆环的 Feature 状态
和置信度也不变；本轮改变的是路径方向的几何依据，而不是放宽验证门。

## 审计字段

`radius-orientation-transitions.csv` 新增：

- `analytic_bridge_included_in_optimization`；
- `analytic_bridge_signed_alignment`；
- `analytic_bridge_orientation_cost`。

`radius-profiles.csv` 新增 `analytic_bridge_optimized_orientation_transition_count`，CLI 同时汇总该计数。
最终证据还会检查动态规划选中方向的桥接符号 alignment；若低于配置的路径转移阈值，会明确记为
方向冲突。

## 回归与下一步

回归测试要求混合解析/BSpline 圆环恰好一个桥进入优化、符号 alignment 大于 0.95、报告成本与
动态规划实际成本完全相同。Visual Studio 2019 Debug 构建成功，`ctest` 结果 1/1 通过。完整报告
位于 `D:\workspace\cad-algorithm-reports\iteration26`。

下一轮进入人工真值评测：建立面级、链级和 Feature 级标签，计算 precision、recall、F1，并按
解析曲面、自由曲面、变半径、闭环和分叉类型分别统计，避免只用候选数量评价算法。
