# 第二十八轮：性能预算、缓存与工程化收尾

## 目标

最后一轮把“能运行”提升为可测量、可设门、可回归的工程接口。计时只覆盖
`improved::recognize()`，不混入 STEP/IGES/BREP 文件读取以及 CSV/BREP 报告写入时间。

## 五阶段计时

`Result::PerformanceDiagnostics` 输出：

- 模型诊断；
- 逐面分类、曲率轨迹和支撑恢复；
- Link 构建、拓扑角色、链与路径分解；
- 半径轮廓和全局方向优化；
- Feature 聚合；
- 识别总耗时。

五个阶段使用同一 `steady_clock` 的连续时间点，自动化测试要求阶段和与总时间在浮点误差内一致。
`summary.json` 输出总耗时及预算状态，`performance.csv` 输出全部阶段、几何支撑测试数和缓存工作量。

## 面对 Link 索引

第二十六轮的解析桥动态规划会多次查询同一对相邻面。此前每个查询都扫描全部 `result.links`。
本轮在 Link 构建完成后生成：

`(min(faceA, faceB), max(faceA, faceB)) → Link 索引列表`

动态规划的四种方向状态、闭环代价和最终证据验证共享该索引。linkrods 有 42 条 Link，执行 238
次面对查询：旧方式理论上检查 9,996 个 Link，新方式只访问 434 个局部候选，候选访问量减少
95.7%。混合解析/BSpline 圆环执行 20 次查询，只访问 20 个候选。

缓存不改变 Link 的保守聚合：同一面对有多条共享边时仍检查该对的全部 Link，方向冲突不会因索引
只返回一条“有利边”而被隐藏。

## 性能预算门

API 使用 `Options::maximumRecognitionMilliseconds`；0 表示关闭预算。算法完成后在 Result 中记录
`budgetEnabled` 与 `budgetExceeded`，不会因超时丢弃部分识别结果。

CLI 示例：

```powershell
fillet-search.exe --improved --performance-budget-ms 30000 `
  --fail-on-performance-budget --report-dir D:\workspace\cad-algorithm-reports model.step
```

启用失败门后，批处理中超预算模型会令最终进程返回非零，但程序仍继续处理后续模型并写出诊断
报告。使用 `0.001 ms` 对生成变半径模型执行负向验证，CLI 正确打印 `budget=EXCEEDED`、保留报告
并返回退出码 1。

## Visual Studio 2019 Debug 基线

以下是同一轮顺序运行的内部识别耗时，单位毫秒。自由曲面闭环使用 5,000 ms 门，其余模型使用
30,000 ms 门，全部通过。

| 模型 | 总计 | 诊断 | 逐面分析 | 拓扑 | 半径/方向轮廓 |
|---|---:|---:|---:|---:|---:|
| generated-closed-bspline-fillet-ring | 1,594.51 | 175.00 | 273.92 | 1,145.42 | 0.15 |
| generated-mixed-analytic-bspline-fillet-ring | 1,548.08 | 229.04 | 308.63 | 1,010.07 | 0.32 |
| generated-variable-fillet | 140.84 | 59.08 | 81.66 | 0.05 | 0.04 |
| generated-branched-fillet-network | 221.75 | 44.33 | 53.65 | 123.61 | 0.11 |
| bottle | 6,506.25 | 644.33 | 752.70 | 5,108.82 | 0.30 |
| CrankArm | 10,606.76 | 941.53 | 1,601.54 | 8,061.19 | 2.35 |
| bearing | 28,635.48 | 2,148.86 | 12,024.45 | 14,460.93 | 0.74 |
| hammer | 14,638.87 | 1,121.42 | 2,905.81 | 10,611.13 | 0.29 |
| screw | 522.69 | 200.66 | 321.99 | 0.03 | 0.00 |
| linkrods | 14,078.74 | 3,310.15 | 5,199.45 | 5,563.38 | 5.63 |
| Motor-c | 18,846.64 | 2,419.32 | 3,061.29 | 13,365.31 | 0.35 |

bearing 是当前最慢模型，28.64 秒已接近本次 30 秒演示门，不应把这个单次 Debug 数值直接当成
量产 SLA。bottle、CrankArm、hammer 和 Motor-c 的主要耗时都在拓扑阶段；bearing 和 linkrods
还在逐面几何支撑恢复上消耗大量时间。最终半径聚合和动态规划本身只有毫秒级，不是当前瓶颈。

## 工程判断

- 当前 D 盘 OCCT 7.8.1 SDK 是 Debug 构建，因此本表只作为可重复回归基线；正式性能 SLA 需要先
  在 D 盘另建匹配 MSVC 2019 的 Release OCCT SDK，再建立 Release 基线。
- 本轮没有直接并行调用 OCCT 几何/拓扑对象。并行化需要先验证 Handle 共享、惰性缓存和结果顺序
  的线程安全及确定性，否则可能用速度换取不可复现错误。
- 下一处明确优化目标是几何支撑恢复的空间索引，以及复合连接中的精确面距调用；阶段计时已经能
  量化这些改动是否真正有效。
- 性能预算是完成后门控，不是中途取消。这样始终保留完整审计结果；若产品需要硬实时取消，应在
  独立进程层实施超时和隔离。

## 验证结果

自动化测试覆盖计时闭合性、默认预算关闭、极小预算超限和解析桥面对缓存实际使用。Visual Studio
2019 Debug 构建成功，`ctest` 结果 1/1 通过。完整基线位于
`D:\workspace\cad-algorithm-reports\iteration28`，超预算负向报告位于
`D:\workspace\cad-algorithm-reports\iteration28-budget-failure`。

至此既定 28 轮实现路线完成。当前版本可以称为“工业化候选基线”：算法、负样本、证据报告、
真值评测和性能门均已贯通；是否达到具体企业的量产标准，仍取决于真实业务模型盲标结果、Release
性能基线和宿主系统集成验收。
