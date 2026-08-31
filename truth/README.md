# 真值标注格式

每行对应输入模型中的一个面，`persistent_id` 是跨重复导入匹配的主键。允许的
`expected_label` 为：

- `FILLET`：真实圆角面；
- `NOT_FILLET`：非圆角面；
- `UNKNOWN`：尚未人工复核，不参与面级混淆矩阵。

正类面使用 `expected_chain` 和 `expected_feature` 分组。只有所有模型面都已复核，且每个正类面都
填写了对应分组时，评测器才发布链级或 Feature 级指标。`expected_feature_kind` 可填写
`isolated-patch`、`simple-chain`、`closed-loop` 或 `composite-junction-network`；
`expected_radius_behavior` 可填写报告中的半径行为名称。

`predicted_label` 仅用于方便审阅，是本次算法输出，不是真值。标注者必须检查几何模型，不得直接
复制该列。`face_id` 也只是当前导入序号，评测匹配使用 `persistent_id`。

当前目录中的两个真值集来自可重复构造的生成模型：

- `generated-closed-bspline-fillet-ring.csv`；
- `generated-variable-fillet.csv`。

真实 OCCT 模型尚未完成人工复核，生成的模板继续保持 `UNKNOWN`，不得计入精度结论。
