# 第十五轮：半径查询语义与路径半径轮廓

本轮把原来含义不清的 `minimumRadius/maximumRadius` 扩展为显式查询语义，并在最大非分支
路径上建立可审计的半径轮廓。实现仍然直接建立在 `识别圆角.cpp` 基线集成工程之上，未恢复使用
已弃用的 `FilletSearcher` 基线。

## 本轮实现

- `NominalWithinRange`：名义半径位于区间；
- `EntireProfileWithinRange`：曲面采样半径范围完全位于区间；
- `ProfileIntersectsRange`：曲面采样半径范围和区间相交；
- `AnyRadius`：完全关闭半径过滤；
- 每条 `Path` 对应一个 `RadiusProfile`，记录逐面名义值、逐面范围、全路径范围、起止值、
  最大相邻相对跳变和自由曲面候选面数量；
- `Feature` 聚合其全部路径的 profile ID 和最严重半径行为；
- 新增 `radius-profiles.csv`，`features.csv` 增加 profile ID 与行为，`summary.json` 增加轮廓数；
- CLI 新增 `--radius-min`、`--radius-max`、`--radius-mode`。

## 七模型回归

| 模型 | 接受面 | 链 | 路径/轮廓 | 恒定 | 平滑变半径候选 | 分段 | 不连续 |
|---|---:|---:|---:|---:|---:|---:|---:|
| bottle.brep | 49 | 3 | 25 | 25 | 0 | 0 | 0 |
| CrankArm.brep | 18 | 10 | 10 | 8 | 2 | 0 | 0 |
| bearing.iges | 41 | 37 | 39 | 33 | 6 | 0 | 0 |
| hammer.iges | 15 | 15 | 15 | 11 | 4 | 0 | 0 |
| screw.step | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| linkrods.step | 25 | 6 | 17 | 9 | 8 | 0 | 0 |
| Motor-c.brep | 39 | 27 | 34 | 26 | 8 | 0 | 0 |

默认查询模式下，七个模型的接受面、链和路径数量与上一轮一致；本轮增加的是查询控制和路径级
解释结果，没有借机改变候选拓扑。

## 半径语义 A/B

在 `bottle.brep` 上使用 `2.4..2.6 + entire`，只保留半径 2.5 的链：24 个面、1 条链、
12 条路径；半径 0.6 和 1.9 的链被过滤。使用 `100..200 + any` 时仍得到原始的 49 个面、
3 条链、25 条路径，证明 `AnyRadius` 不会偷偷使用数值区间。

## 重要边界

当前的“平滑变半径”是几何候选标签。单个 BSpline 面内的最小/最大半径来自曲率网格样本，尚未
沿真实脊线恢复连续的 `R(s)` 设计函数。现有图连接仍采用较严格的相邻名义半径相容条件，因此本批
模型没有形成“分段”或“不连续”路径；下一轮需要在保持不同圆角特征不会误合并的前提下，加入
面内脊线排序、沿脊线密集采样和面间渐变连接证据。

## 验证与运行时修复

回归测试覆盖四种模式，结果为 1/1 通过。另修复了普通 `ctest --test-dir ...` 会从系统 PATH
加载错误 OCCT DLL 的问题：CMake 测试与 VS2019 F5 调试现在显式使用
`D:\workspace\cad-algorithm-deps\occt-7.8.1\win64\vc14\bind`，不向 C 盘安装依赖。

完整报告位于 `D:\workspace\cad-algorithm-reports\iteration15`。
