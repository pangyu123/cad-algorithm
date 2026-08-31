# 第二十七轮：面、链与 Feature 真值评测

## 目标

此前报告只能说明算法产生了多少候选，不能回答多少是正确圆角、多少真实圆角被漏掉。本轮增加
独立真值输入和三层评测，明确区分候选统计与准确率。

## 标注格式

新的 `truth-template.csv` 覆盖模型全部面，而不是只列算法已接受的面。核心列为：

- `persistent_id`：评测匹配主键；
- `predicted_label`：方便审阅的当前预测，不是真值；
- `expected_label`：`FILLET`、`NOT_FILLET` 或 `UNKNOWN`；
- `expected_chain`、`expected_feature`：正类面的对象分组；
- `expected_feature_kind`：孤立、简单链、闭环或复合分叉网络；
- `expected_radius_behavior`：恒定、平滑变半径、分段或不连续。

`face_id` 只用于当前报告定位。重复运行或重新导入后的真值关联使用稳定几何
`persistent_id`。标注规则和现有数据集见 `truth/README.md`。

## 指标定义

面级以 `Verdict::Accepted` 为预测正类，计算 TP、FP、FN、TN、precision、recall、F1 和 accuracy。
`UNKNOWN` 不进入混淆矩阵，同时报告复核覆盖率、缺失真值面数和多余真值记录数。

链和 Feature 将各对象转换为所属面 `persistent_id` 集合，以交并比 IoU 建立候选匹配边，再执行
一对一最大基数匹配。默认要求 `IoU >= 0.50`，可用 `--truth-iou` 调整。对象级 TP 是成功匹配数，
未匹配预测为 FP，未匹配真值为 FN；同时输出匹配对象平均 IoU。

为避免部分标注造成虚假高分，以下任一情况都会禁用相应对象级指标：

- 模型存在 `UNKNOWN` 或缺少真值记录的面；
- 任一真实圆角面没有填写期望链时，禁用链级指标；
- 任一真实圆角面没有填写期望 Feature 时，禁用 Feature 级指标。

匹配后的 Feature 还分别检查类型和半径行为是否正确。

## 已建立真值集

### 闭合 BSpline 圆角环

真值由模型构造和独立曲面类型确定：四个 BSpline 圆环片为圆角正类，两个圆柱为结构支撑负类；
四个正类面属于一个闭合恒半径 Feature。

| 层级 | TP | FP | FN | TN | Precision | Recall | F1 | 平均 IoU |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 面 | 4 | 0 | 0 | 2 | 1.0 | 1.0 | 1.0 | — |
| 链 | 1 | 0 | 0 | — | 1.0 | 1.0 | 1.0 | 1.0 |
| Feature | 1 | 0 | 0 | — | 1.0 | 1.0 | 1.0 | 1.0 |

Feature 类型 `closed-loop` 和半径行为 `constant` 均匹配。

### OCCT 生成的 3→8 变半径圆角

真值由 `BRepFilletAPI_MakeFillet` 的单边变半径构造确定：一个生成圆角面为正类，六个箱体面为
负类；正类属于一个孤立的平滑变半径 Feature。

面级 TP=1、TN=6、FP=0、FN=0；链和 Feature 均为 1/1 匹配，三级 F1 全部为 1.0，Feature 类型
`isolated-patch` 和半径行为 `smooth-variable-candidate` 均匹配。

这些分数证明评测链路和两个确定性样件结果正确，不代表真实工业模型已经达到 100% 准确率。
bottle、CrankArm、bearing、hammer、screw、linkrods 和 Motor-c 尚无独立人工真值，模板全部保持
`UNKNOWN`，因此本轮不为它们发布 precision/recall。

七个待复核模板均覆盖模型全部面：bottle 71、CrankArm 53、bearing 213、hammer 45、screw 10、
linkrods 37、Motor-c 223；模板行数已逐一与 `summary.json` 的 `model_face_count` 核对一致。

## CLI 与输出

```powershell
fillet-search.exe --improved --truth-file labels.csv --truth-iou 0.5 `
  --report-dir D:\workspace\cad-algorithm-reports model.brep
```

一次真值评测只允许一个模型，且不与 Sewing 比较混用。报告新增：

- `evaluation-summary.csv`：三级混淆矩阵和指标；
- `evaluation-coverage.csv`：复核覆盖、缺失/多余记录、类型及半径行为正确数；
- `evaluation-matches.csv`：链和 Feature 的一对一匹配及 IoU。

## 回归与下一步

自动化测试使用底层 BRep 曲面类型生成闭环独立真值，要求三级 F1、对象 IoU、Feature 类型和半径
行为全部正确；另行注入一个故意误标样本，断言能得到 1 个 FP 和 0.75 precision；部分标注测试
断言对象级指标不可用。Visual Studio 2019 Debug 构建成功，`ctest` 结果 1/1 通过。

完整报告位于 `D:\workspace\cad-algorithm-reports\iteration27`。下一阶段首先需要由 CAD 工程师在
真实业务模型上完成盲标和交叉复核；最后一轮同时加入性能计时、预算门、缓存和大模型工程化输出。
