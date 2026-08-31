# OCCT 7.8.1 圆角搜索

这是一个锁定到 Open CASCADE Technology 7.8.1 的 C++17/CMake 调试工程。算法基线直接编译并调用 `D:\workspace\cad-algorithm\识别圆角.cpp` 原源码；`FilletBaselineAdapter.cpp` 只负责工程接口和结果转换，不包含识别规则。`ImprovedFilletRecognizer` 在不修改基线的前提下逐项实现 `圆角识别改进.txt`。

文档入口：

- `圆角识别算法基线与28轮迭代记录.md`：环境、路径、逐轮效果、测试基线和后续接手流程；
- `docs/28轮迭代技术详解.md`：28 轮技术原理的通俗解释，以及 TopologyRole/Feature 专题说明。

- **圆角链路**：半径相容、通过公共边 G1 连续的候选面连通分量；
- **链路属性**：边界边、切向支撑边、端部边、半径范围、闭合状态和分支状态。

## 环境与构建

要求 Visual Studio 2019（MSVC x64）和 CMake 3.24+。OCCT 源码、SDK 和构建产物全部位于 D 盘：

- `D:\workspace\cad-algorithm-deps\occt-src-7.8.1`
- `D:\workspace\cad-algorithm-deps\occt-build-7.8.1`
- `D:\workspace\cad-algorithm-deps\occt-7.8.1`
- `D:\workspace\cad-algorithm-build`

源码标签和安装 SDK 版本均为 OCCT 7.8.1。CMake preset 通过 `OpenCASCADE_DIR` 直接引用该 SDK。

```powershell
cmake --preset windows-debug
cmake --build --preset debug
ctest --preset debug
```

调试器可直接启动 `D:\workspace\cad-algorithm-build\debug\Debug\fillet-search.exe`。CLI 支持 STEP、IGES、BREP，并可一次扫描多个模型：

```powershell
D:\workspace\cad-algorithm-build\debug\Debug\fillet-search.exe model.step model.brep model.iges
```

默认运行改进算法。使用 `--baseline` 只运行原算法，使用 `--compare` 输出两者的 A/B 结果：

```powershell
D:\workspace\cad-algorithm-build\debug\Debug\fillet-search.exe --compare model.step
```

受控拓扑缝合只在显式指定时运行，并同时保留原始与缝合副本结果：

```powershell
D:\workspace\cad-algorithm-build\debug\Debug\fillet-search.exe --compare-sewing `
  --sewing-factor 1 --report-dir D:\workspace\cad-algorithm-reports model.iges
```

缝合容差由原始模型自适应容差乘以 factor，并受模型包围盒对角线 `1e-5` 上限约束。默认禁止
非流形 sewing，且不切割自由边。Sewing 按 Solid 优先、Shell 次之分区；只有无实体/壳归属的
散面才使用空间邻近分区，避免跨零件建立拓扑。正常 `--improved` 路径不会执行 Sewing。

半径查询不再只有一个含糊的数值参数。`--radius-min/--radius-max` 配合
`--radius-mode` 明确表达四种语义：

- `nominal`：逐面名义半径落入区间（默认，兼容原行为）；
- `entire`：逐面采样到的完整半径范围都落入区间，最严格；
- `intersects`：逐面半径范围与查询区间有交集；
- `any`：不按半径过滤，用于先识别全部候选再分析轮廓。

```powershell
D:\workspace\cad-algorithm-build\debug\Debug\fillet-search.exe `
  --radius-min 2.4 --radius-max 2.6 --radius-mode entire model.brep
```

使用 `--report-dir` 可将可审计结果写到指定目录（建议继续放在 D 盘）：

```powershell
D:\workspace\cad-algorithm-build\debug\Debug\fillet-search.exe --compare `
  --report-dir D:\workspace\cad-algorithm-reports model.step
```

每个模型会输出 `summary.json`、逐面证据 `faces.csv`、连接 `links.csv`、
链路、最大非分支路径、`radius-profiles.csv` 与复合特征 CSV、可定位的
`direction-samples.csv`、Graphviz `graph.dot`、
方向冲突点与主方向线段 BREP、人工标注模板，以及只含已接受候选面的
`accepted-faces.brep`。`persistent_id` 由量化后的包围盒、质心、面积、半径与曲面类型生成，
用于跨重复导入核对结果；装配中完全重合的重复实例会追加确定性的序号。

## API

调用 `fillet::baseline::search(shape, minimumRadius, maximumRadius, options)`。返回结果直接来自原源码的 `occ::FilletChainSearcher::Search`。输入应是已缝合且拓扑有效的实体或复合实体；BSpline、变半径与复合链路将在后续工业化迭代中加入。

改进接口为 `fillet::improved::recognize(shape, options)`。当前已加入模型有效性与尺度诊断、
自适应容差、标准曲面恢复、一般曲率场候选、几何支撑恢复、分叉路径分解、置信度、拒绝原因、
稳定几何指纹和完整审计报告。切向邻面与非圆角结构支撑面分别记录；缺少两个外部结构支撑面的
链会标记为 `NeedsReviewInsufficientExternalSupports`。拓扑角色进一步区分孤立面、端部面、带状面和
汇合面，并将一个连通分量内的多条路径聚合为 `Feature`。曲率场同时支持单主曲率带状候选和
同号双主曲率角部候选；拓扑角色会与曲面几何交叉校验并输出冲突原因。公共边两侧的带状面还会
沿边多点比较次曲率主方向，统计覆盖率、最小值、均值和标准差，验证圆角脊线方向连续性并避开
脐点附近的不稳定样本。对于没有共享拓扑边但已通过共同外部支撑面验证的复合连接，算法会使用
边界采样和曲面投影生成方向证据，并保留每个样本的 alignment 与 gap。每条最大非分支路径还会
输出有序的逐面名义半径、逐面局部最小/最大半径、起止值和最大相邻跳变，并分类为恒定、平滑变半径
候选、分段或不连续。自由曲面上的局部范围来自曲率采样，目前不是 CAD 设计参数中的精确半径函数；
`smooth-variable-candidate` 因此有意保留“候选”措辞。报告仍是候选证据，必须结合人工真值评估
准确率，不能把候选数量当作正确率。

对于带状自由曲面，算法会将三维主曲率方向投影回 UV 切平面，以中部有效点为种子进行双向
二阶流线积分，默认生成 11 个点，并形成按三维弧长归一化的 `R(s)`。只有流线因裁剪边界、脐点
或覆盖率不足而失败时才退回主参数线，且会明确标记 `parameter-line-fallback`。报告中的
`face-radius-samples.csv` 和 `radius-profile-samples.csv` 保留 UV、XYZ、半径和主方向；
`radius-trace-points.brep` 与 `radius-trace-segments.brep` 可直接在 OCCT/CAD 查看器中叠加检查。
平滑度分类同时考虑有效变化步比例、变化方向一致性和单步占总变差比例，避免把幅度较大的平滑
渐变错误地当成半径断跳。轨迹线段与无向主方向的最小对齐度低于 0.90 时，逐面样本仍保留用于
排错，但不会进入路径级半径行为分类。

流线追踪会在圆角宽度方向的不同采样行各选一个代表种子；即使第一条流线完整，也会继续取得默认
3 条来自不同横向行的有效流线。算法将每条流线按三维弧长重采样到共同参数，自动统一方向后比较
`R(s)`，默认最大跨种子相对偏差不得超过 10%。非恒定半径行为只有通过这项横向稳定性验证才会
升级为平滑、分段或不连续，否则降为证据不足；原始样本与偏差仍完整输出供审计。
当候选步越过裁剪边界，或三维弦线与端点主方向的对齐度不足时，积分器会自动缩步重算。覆盖率按
实际归一化 UV 轨迹长度计算，不能通过在种子附近堆积采样点获得假覆盖。UV 度量矩阵使用无量纲
条件数判断，积分前进阈值使用数值精度而非模型导入容差，因此支持不同单位和整体缩放模型。

第十九轮跨种子验证结果见 `docs/iteration-19-results.md`；机器可读报告位于
`D:\workspace\cad-algorithm-reports\iteration19`。

路径方向在聚合 `R(s)` 前会先规范化：只有一个分叉端点时统一从分叉点向外，其余开放路径用稳定
几何 ID 确定起止方向；闭环旋转到最小稳定 ID，并确定唯一环向。随后用二状态动态规划同时选择
整条路径上每个自由面流线是否翻转，代价由无量纲端点距离和有向主方向误差组成；闭环还包含末端
到首端的闭合代价。`radius-profiles.csv` 同时输出全局代价、贪心基线代价、翻转面数量和闭合间隙。
第二十轮结果见 `docs/iteration-20-results.md`，报告位于
`D:\workspace\cad-algorithm-reports\iteration20`。

为测试自由曲面路径而不让标准曲面恢复提前接管，可显式使用
`--no-canonical-recovery`。第二十一轮加入了可重复生成的四面闭合 BSpline 圆角环和全边圆角分叉
网络，并修复周期曲面修剪包围盒导致的非相邻面复合误连：共同双支撑现在还必须通过 OCCT 精确
面间距离门。详见 `docs/iteration-21-results.md`，报告位于
`D:\workspace\cad-algorithm-reports\iteration21`。

第二十二轮将路径方向总成本拆成逐连接证据，输出到
`radius-orientation-transitions.csv`。只有路径中相邻且都有流线的面才执行质量判定；跨过解析面或
缺失轨迹的连接标记为 `unobserved-intermediate-faces`，不会伪装成方向冲突。默认间隙阈值为模型
对角线的 0.15，签名方向点积阈值为 0.0，失败原因区分间隙过大、方向不匹配及两者同时发生。
详见 `docs/iteration-22-results.md`，报告位于
`D:\workspace\cad-algorithm-reports\iteration22`。

第二十三轮将证据提升到 Feature 层：拓扑/支撑、半径稳定性、路径方向分别输出
`validated`、`conflict` 或 `insufficient-evidence`，总状态取最严重者。最终置信度保留原链置信度为
基础值，再乘以三项证据加权分数；默认权重为 0.40/0.30/0.30，三态默认分数为
1.0/0.35/0.65，全部可通过 `Options` 调整并有合法性检查。详见
`docs/iteration-23-results.md`，报告位于 `D:\workspace\cad-algorithm-reports\iteration23`。

第二十四轮为自由曲面流线之间被解析圆角面隔开的连接补充了方向桥接。桥上的圆柱、恢复圆柱或
圆环面必须逐跳存在已评估且兼容的 Link；全部通过才将连接升级为已验证，任一明确不兼容则记为
方向冲突，缺少 Link 仍保留为证据不足。新增混合解析/BSpline 闭环回归件验证了正向桥接，同时
CrankArm 的一个缺证连接被成功补齐，linkrods 的八个可疑连接则被显式识别为冲突。详见
`docs/iteration-24-results.md`，报告位于 `D:\workspace\cad-algorithm-reports\iteration24`。

第二十五轮加入尺度归一化的数值分辨率门：面积低于模型对角线平方阈值的薄片面不进入候选图，
长度低于模型对角线阈值的共享边不能建立 Link，且不能被后续复合连接重新接回。默认阈值分别为
`1e-12` 和 `1e-8`，可用 `--min-face-area-to-diagonal-squared` 与
`--min-edge-to-diagonal` 调整。新增修剪缺口、位置错位和薄片 BREP 负样本；详见
`docs/iteration-25-results.md`，报告位于 `D:\workspace\cad-algorithm-reports\iteration25`。

第二十六轮将解析桥接的有符号方向代价直接纳入开放链/闭环二状态动态规划。主方向轴的正负号通过
整条桥的点积乘积消除，圆柱和圆环面内部则沿 UV 主方向场分步传播，避免把曲面自身的自然转弯误判
成方向失配。方向权重默认 0.10，可用 `--orientation-direction-weight` 调整；证据不完整的桥仍回退
端点代价。详见 `docs/iteration-26-results.md`，报告位于
`D:\workspace\cad-algorithm-reports\iteration26`。

第二十七轮加入独立真值评测：模板覆盖模型全部面，按 `persistent_id` 读取
`FILLET/NOT_FILLET/UNKNOWN`，输出面、链和 Feature 三级 precision、recall、F1。链和 Feature
使用可配置 IoU 阈值做一对一最大匹配；部分标注只输出面级结果，不发布误导性的对象级分数。
使用 `--truth-file labels.csv --truth-iou 0.5` 运行。详见 `docs/iteration-27-results.md`，报告位于
`D:\workspace\cad-algorithm-reports\iteration27`。

第二十八轮加入五阶段内部计时、面对 Link 索引缓存和可执行性能预算门。`performance.csv` 分别输出
诊断、逐面分析、拓扑、半径轮廓、Feature 聚合耗时及缓存命中工作量。使用
`--performance-budget-ms 30000 --fail-on-performance-budget` 可在超预算时保留完整报告并返回非零
退出码。Visual Studio 2019 Debug 全语料基线和瓶颈分析见 `docs/iteration-28-results.md`，报告位于
`D:\workspace\cad-algorithm-reports\iteration28`。
