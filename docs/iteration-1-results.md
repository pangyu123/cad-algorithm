# 圆角识别改进：第一轮结果

## 范围

原始 `D:\workspace\cad-algorithm\识别圆角.cpp` 保持不变并作为 baseline。第一轮改进加入：

- BRep 有效性、尺度、容差、退化边和非流形边诊断；
- 根据模型尺度和原始 BRep 容差计算有效线性容差；
- 使用 `ShapeAnalysis_CanonicalRecognition` 恢复一般曲面中的圆柱和球面；
- 为每个候选记录稳定 face ID、几何来源、半径、拟合 gap、支撑面 ID、置信度和拒绝原因；
- 使用两个不同切向支撑面替代单纯的支撑边连通组作为接受证据；
- 使用公共边和共享支撑面对建立带类型连接关系。

## A/B 结果

| 模型 | Baseline 面/链 | Improved 面/链 | 标准曲面恢复 |
|---|---:|---:|---:|
| linkrods.step | 6 / 6 | 15 / 8 | 4 |
| screw.step | 0 / 0 | 0 / 0 | 0 |
| bottle.brep | 49 / 3 | 53 / 7 | 0 |
| CrankArm.brep | 0 / 0 | 4 / 4 | 2 |
| Pump_TopCover.brep | 0 / 0 | 0 / 0 | 0 |
| Motor-c.brep | 3 / 3 | 16 / 6 | 0 |
| hammer.brep | 0 / 0 | 3 / 2 | 7 |
| asahi.brep | 0 / 0 | 0 / 0 | 0 |
| bearing.iges | 0 / 0 | 0 / 0 | 22 |
| hammer.iges | 0 / 0 | 0 / 0 | 8 |

## 已知风险

- bottle 中半径约 85 的面、hammer 中半径 1494/3513 的面可能是主体曲面误报；当前结果不能视为准确率提升。
- screw 和 Pump_TopCover 仍为零，需要检查曲面类型、曲率场和支撑边失败原因。
- IGES 模型能恢复标准曲面但无法通过支撑验证，说明导入后的拓扑连接或连续性需要单独处理。
- 当前尚未实现一般 BSpline 曲率场、变半径 radius law、凹凸性和最大非分支路径分解。

下一轮优先加入凹凸性、曲面覆盖比例、半径相对模型尺度以及逐面拒绝报告导出，再进入一般曲面曲率识别。
