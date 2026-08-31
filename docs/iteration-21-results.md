# 第二十一轮：闭环与多分支真值件

## 目标

补齐第二十轮缺少的多面闭环运行覆盖，并建立可重复、无需外部下载的 OCCT 多分支模型。真值件必须
进入实际识别、拓扑图、流线追踪和全局方向优化，而不是只测试独立数学函数。

## 新增真值模型

### 四面闭合 BSpline 圆角环

- 使用 OCCT 圆环曲面构造精确几何后转换为 BSpline；
- 沿环向切分为 4 个修剪面并 Sewing 为共享拓扑边；
- 截面为半圆形圆角槽，两端分别与半径 35 和 25 的圆柱支撑面相切；
- 测试时关闭标准曲面恢复，强制经过曲率场、主方向流线和跨种子稳定性；
- 结果写入 `generated-closed-bspline-fillet-ring.brep`。

最终识别结果：

- 接受圆角面 4，曲率场面 4；
- 结构支撑 2，链验证通过；
- 活动图连接恰好 4 条，形成 1 条四面闭环；
- 44 个 `R(s)` 样本，4 个面全部跨种子稳定；
- 全局闭环方向优化已执行；
- 半径稳定为 5，分类为恒定半径。

### 多分支圆角网络

- 使用 `BRepFilletAPI_MakeFillet` 对长方体 12 条边全部施加 R=5 圆角；
- 结果写入 `generated-branched-fillet-network.brep`；
- 识别出 14 个圆角面、3 条链、10 条最大非分支路径；
- 主网络包含 12 个面并通过双支撑验证；
- 8 条路径触及分叉，所有单分叉端路径均从分叉节点向外。

## 真值件发现的算法缺陷

半圆截面闭环最初被识别为 6 条分叉路径，而不是一个闭环。诊断发现四个周期 BSpline 修剪面的
底层曲面相同，曲面包围盒覆盖范围过宽；原复合连接仅检查“包围盒相交 + 共同双支撑 + 半径相容”，
因此错误添加了两条对角连接，把四节点环变成 K4。

修复后，复合连接还必须通过 `BRepExtrema_DistShapeShape` 的精确修剪面间距离检查，距离不得超过
自适应几何间隙容差。包围盒只保留为廉价预筛选，不能再单独证明空间邻接。

回归断言明确要求闭环：

- 只有 1 条链且链验证通过；
- 活动连接数量恰好为 4；
- 存在至少 3 面的闭合路径；
- 至少 3 个流线面进入闭环全局方向优化；
- 全局方向代价不高于贪心代价。

## 原模型回归

生成的 R=3→8 变半径样件及 bottle、CrankArm、bearing、hammer、screw、linkrods、Motor-c 全部重跑。
接受面、链、路径、半径行为和第十九/二十轮关键统计保持不变，说明精确距离门删除的是周期曲面
假连接，没有误删已知合法复合链。

Visual Studio 2019 Debug 构建成功，`ctest` 结果 1/1 通过。完整报告位于
`D:\workspace\cad-algorithm-reports\iteration21`。

## 命令行复现

```powershell
D:\workspace\cad-algorithm-build\debug\Debug\fillet-search.exe `
  --no-canonical-recovery `
  --report-dir D:\workspace\cad-algorithm-reports\iteration21 `
  D:\workspace\cad-algorithm-build\debug\test-artifacts\generated-closed-bspline-fillet-ring.brep
```

## 下一步

- 为方向转移输出逐连接证据和失败原因；
- 构造多分支变半径而不只是恒定半径网络；
- 加入局部曲率噪声、修剪缺口和容差扰动版本；
- 将闭环间隙、方向连续性和跨种子稳定性聚合到 Feature 置信度。
