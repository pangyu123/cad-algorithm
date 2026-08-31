# 圆角识别改进：第十二轮结果

## 冲突自动加密采样

无共享边的投影方向连接首先执行默认 5 点采样。若覆盖率足够但出现方向冲突，算法自动执行
第二阶段加密：

- 加密目标样本数：21；
- 最大投影尝试数：400；
- 初始证据与加密证据合并，保证原始异常样本不会因为重新排序而丢失；
- 报告同时保留初始有效样本数和初始最小 alignment。

新增 Link 字段：

- `direction_refinement_attempted`
- `direction_refinement_accepted`
- `initial_direction_valid_samples`
- `initial_minimum_spine_direction_alignment`

## 主方向矢量

每个 `DirectionSampleEvidence` 新增两侧次曲率主方向的 XYZ 分量。
报告新增 `direction-conflict-vectors.brep`：对冲突 Link 的每个样本，在两侧采样点分别建立一条
无方向线段。线段以采样点为中心，避免参数方向正负翻转影响观察。

## bearing face 45–67 加密结果

- 初始：5/5 有效，1 个冲突样本；
- 加密并合并：26/26 有效；
- 冲突样本：7 个；
- 最小 alignment：约 `0.0000271`；
- 平均 alignment：约 `0.73078`；
- 标准差：约 `0.44355`。

冲突不是孤立噪声，而是集中在 face 45 的低参数区间：

- 冲突区 `v` 约为 `0.045～0.167`；
- 此区间 face 45 的次/主曲率比约从 `0.468` 增加到 `0.649`；
- face 67 的次/主曲率比仍接近 0，保持明显带状特征；
- 高 `v` 样本的 alignment 基本为 1。

因此 face 45–67 连接在局部区间存在稳定、可重复的方向不一致。当前证据比“单点异常”更强，
但在没有人工真值前仍不直接断链；Feature 继续标记方向复核并降低置信度。

## 回归

- bottle：2 个复合 Feature 继续通过方向验证；
- bearing：1 个方向验证 Feature，1 个方向冲突 Feature；
- CrankArm：2 个方向验证 Feature，2 个投影覆盖不足 Feature；
- hammer：没有可形成候选链的复合连接；
- screw：没有接受候选；
- 自动测试：1/1 通过。

候选面、链、路径和 Feature 数量保持不变。

## 输出

- 报告：`D:\workspace\cad-algorithm-reports\iteration12`
- bearing 冲突点：`bearing\direction-conflict-points.brep`
- bearing 主方向线段：`bearing\direction-conflict-vectors.brep`
- 逐样本矢量与曲率：`bearing\direction-samples.csv`

## 下一步

下一轮应开始处理受控拓扑修复：在原始 Shape 副本上执行 sewing，比较修复前后共享边、候选面、
链路和几何指纹变化，并设置最大合并 gap 与禁止跨实体合并规则。修复结果必须作为可选输入，不能
静默替换调用者提供的原始拓扑。
