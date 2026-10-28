# OCCT 7.8.1 → 7.9.3 升级需求与设计文档

| 项目 | 内容 |
|---|---|
| 文档类型 | 需求 / 设计文档（版本升级评估） |
| 适用范围 | 基于 Open CASCADE Technology 7.8.1 构建的 CAD / 几何算法工程 |
| 目标版本 | OCCT **7.9.3**（tag `V7_9_3`），并为后续 8.x 升级预留 |
| 基线版本 | OCCT **7.8.1**（tag `V7_8_1`，2024-04-01） |
| 依据 | 官方源码 tarball 逐文件对比 + token 归一化统计 + 400 条提交记录 |
| 编写日期 | 2026-10-28 |
| 结论 | 低风险升级：语义净变化 **0.26%**，编译级必改点 **4 处**，行为级必测点 **3 处** |

---

## 1. 背景与目标

### 1.1 背景

当前工程锁定 OCCT 7.8.1。官方已发布 7.9.3 与 8.0.1 两个后续版本线。7.9.3 与 7.8.1 保持相同的源码布局（`src/<Package>/`），是通往 8.0.1（布局重构、`Handle`/`Standard_*` 移除）的天然跳板。

初步人工浏览源码时，观察到"大量文件被修改"，但其中绝大部分为 clang-format 18.1.8 全库格式化、`.gxx` 模板合并、`.hxx→.pxx` 私有化重命名，**不构成语义变化**。本文档的目的，是把这部分噪声剥离后，给出真正需要处理的差异清单。

### 1.2 目标

1. 量化 7.8.1 → 7.9.3 的真实代码变化量（剔除格式与文件移动）。
2. 列出会**打断编译**的 API 变化，并给出替代方案。
3. 列出会**改变运行结果**的默认值/行为变化，并给出回归范围。
4. 给出分阶段升级方案、工作量评估与验收标准。
5. 明确 7.9.3 → 8.0.1 的前置迁移项，避免二次返工。

### 1.3 非目标

- 不评估 8.0.1 的完整迁移（另见 `reference/REPORT.md`、`reference/UPGRADE-PATHS.md`）。
- 不涉及 Draw Test Harness 脚本层的适配。
- 不对新增功能（glTF 边/点云、STEP AP242 元数据）做选型结论，仅登记。

---

## 2. 度量口径与总体结论

### 2.1 口径说明

| 口径 | 做法 | 用途 |
|---|---|---|
| 原始行 churn | `git diff --no-index --numstat` | 反映"看起来改了多少" |
| 忽略空白行 churn | 加 `-w` | 去掉缩进噪声 |
| Token 级编辑 churn | 去注释/空白，统一 `Handle`、`Standard_*` 等机械重命名后逐 token diff | 真实编辑量 |
| Token 多重集净变化 | 按包统计 token 频次差 `Σ\|count_new − count_old\|` | 代码体量净增减；移动/改名/合并全部抵消 |

归一化掉的机械变化：`occ::handle<T>` / `Handle_T` → `Handle(T)`；`Standard_Integer/Real/Boolean/...` → 统一 token；`Standard_True/False` → 统一；`Standard_EXPORT/HIDDEN` → 删除；注释、空行、缩进。

### 2.2 总体数据

| 指标 | 7.8.1 | 7.9.3 | 变化 |
|---|---:|---:|---:|
| 逻辑包数量 | 384 | 393 | +9（全部为 DE 抽象层） |
| 源文件数 | 14,214 | 14,227 | +13 |
| Token 总量 | 11,528,255 | 11,557,797 | **+29,542（+0.26%）** |
| Token 编辑 churn | — | — | 304,872 |
| Token 多重集净 churn | — | — | **64,918（0.56%）** |
| 同路径被修改文件 | — | — | 2,766 |
| 类 / 结构定义 | 6,216 | 6,221 | +5（新增 36 / 删除 31） |
| 真实新增文件 | — | — | 98 |
| 真实删除文件 | — | — | 66 |

> **核心结论：语义净变化 0.26%（< 65k token）。** 升级工作量集中在"编译适配与回归"，不在"业务代码重写"。

### 2.3 扩展名迁移

| 扩展名 | 7.8.1 | 7.9.3 | 变化 | 说明 |
|---|---:|---:|---:|---|
| `.hxx` | 7,827 | 7,177 | -650 | 689 个 `RWStep*` 内部头转 `.pxx` |
| `.pxx` | 53 | 747 | +694 | 同上 + 少量新增 |
| `.cxx` | 5,795 | 5,805 | +10 | `.gxx` 合并抵消了新增 |
| `.gxx` | 98 | 72 | -26 | 模板实现并入 `.cxx` |
| `.lxx` | 433 | 418 | -15 | 部分并入 `.pxx` |

严格同目录同名的迁移：**`.hxx → .pxx` 689 个**（全部位于 `RWStep*`），`.lxx → .hxx` 11 个。

---

## 3. 现状分析：真实差异明细

### 3.1 结构层变化

#### 3.1.1 新增 9 个 DE 抽象层包

`DEBREP`、`DEGLTF`、`DEIGES`、`DEOBJ`、`DEPLY`、`DESTEP`、`DESTL`、`DEVRML`、`DEXCAF`，每个包包含 `*_ConfigurationNode` / `*_Provider` / `*_Parameters` 三类文件。

同时新增两个通用件：`DE_ShapeFixParameters`、`DE_ShapeFixConfigurationNode`。

**设计意图**：把各格式的读写参数从 `Interface_Static` 的字符串键值体系，迁移到强类型配置对象；并为 8.0.1 的 DE 框架统一做准备。

#### 3.1.2 包归属调整

| 包 | 7.8.1 | 7.9.3 |
|---|---|---|
| `Expr` | FoundationClasses / TKMath | DETools / TKExpress |
| `ExprIntrp` | FoundationClasses / TKMath | DETools / TKExpress |
| `Precision` | FoundationClasses / TKMath | FoundationClasses / TKernel |

其余 381 个包的模块与工具包归属不变。**无包被删除。**

#### 3.1.3 扩展名私有化

`RWStep*` 下 689 个头文件改为 `.pxx`。典型 include 变化（`RWStepAP214_ReadWriteModule.cxx`）：

```cpp
// 7.8.1
#include <RWStepXxx_RW....hxx>
// 7.9.3
#include "RWStepAP214_....pxx"
#include "../RWStepBasic/....pxx"
```

`.pxx` 分布：RWStepBasic 117、RWStepVisual 97、RWStepShape 91、RWStepGeom 83、RWStepKinematics 74、RWStepRepr 67、RWStepFEA 52、RWStepDimTol 49、RWStepAP214 29、RWStepElement 15、RWStepAP203 11、RWStepAP242 4。

#### 3.1.4 构建系统

| 项 | 7.8.1 | 7.9.3 |
|---|---|---|
| 默认 C++ 标准 | C++11 | **C++17**（可选 C++20/23） |
| 预编译头 | cotire | **原生 CMake `target_precompile_headers`**，`_toolkit` 下 8 个 PCH 头（TKernel/TKMath/TKBRep/TKDEIGES/TKDESTEP/TKMesh/TKXSBase/TKV3d），默认 `BUILD_USE_PCH=OFF` |
| 优化选项 | `-O3 -flto -ffunction-sections` 全局 | 移至 Release 专属 |
| 新增选项 | — | `BUILD_INCLUDE_SYMLINK`（默认 OFF）、include 拷贝优化 |
| 格式化 | 旧版 clang-format | clang-format **18.1.8** |
| 仓库 | — | 迁移至 GitHub |

### 3.2 编译级 API 变化（P0）

#### 3.2.1 `NCollection_Map` 的 11 个布尔运算方法废弃

`NCollection_Map.hxx` 第 364–480 行，以下方法全部标记：

```cpp
Standard_DEPRECATED("This method will be removed right after 7.9. release. Use methods from "
                    "NCollection_MapAlgo.hxx instead.")
```

涉及方法：`IsEqual`、`Contains(map)`、`Union`、`Unite`、`HasIntersection`、`Intersection`、`Intersect`、`Subtraction`、`Subtract`、`Difference`、`Differ`。

替代实现位于新增的 `NCollection_MapAlgo.hxx`，形态为**命名空间 + 自由函数模板**：

```cpp
namespace NCollection_MapAlgo
{
  template <class MapType>
  void Union(MapType& theMap, const MapType& theLeftMap, const MapType& theRightMap);

  template <class MapType>
  bool Unite(MapType& theMap, const MapType& theOtherMap);

  template <class MapType>
  bool HasIntersection(const MapType& theMap, const MapType& theOtherMap);

  template <class MapType>
  void Intersection(MapType& theMap, const MapType& theLeftMap, const MapType& theRightMap);

  template <class MapType>
  bool Intersect(MapType& theMap, const MapType& theOtherMap);

  template <class MapType>
  void Subtraction(MapType& theMap, const MapType& theLeftMap, const MapType& theRightMap);

  template <class MapType>
  void Difference(MapType& theMap, const MapType& theLeftMap, const MapType& theRightMap);
}
```

**重要**：7.9.3 仅告警，**8.0.1 已彻底移除**。这是升级 7.9.3 时必须同步完成的前置改造。

新实现内部处理了操作数别名场景（`&theMap == &theLeftMap` 时先 `Assign` 到临时对象），比旧成员版更健壮。

#### 3.2.2 `Standard_Version.hxx` 从源码树删除

- 7.8.1：`src/Standard/Standard_Version.hxx` 为签入的静态头。
- 7.9.3：改为**构建期生成**，模板位于 `adm/templates/Standard_Version.hxx.in`；同时新增运行时可查询 API：

```cpp
// Standard_VersionInfo.hxx
Standard_EXPORT const char* OCCT_DevelopmentVersion();
Standard_EXPORT double      OCCT_Version_Double();
Standard_EXPORT const char* OCCT_Version_String();
Standard_EXPORT const char* OCCT_Version_String_Complete();
Standard_EXPORT const char* OCCT_Version_String_Extended();
```

**影响**：宏 `OCC_VERSION` / `OCC_VERSION_COMPLETE` 仍存在，但只出现在构建生成目录。非 CMake 构建（手工拼 include 路径、直接拷贝源码树头文件）会找不到该文件。

#### 3.2.3 XSAlgo 整体重构

**删除**：

- `XSAlgo_ToolContainer.hxx/.cxx`（类整体消失）
- `XSAlgo_Caller.hxx`
- `XSAlgo_AlgoContainer` 的 `SetToolContainer()`、`ToolContainer()`、私有成员 `myTC`
- `XSAlgo_AlgoContainer::ProcessShape(..., const Handle(ShapeBuild_ReShape)&, ...)` 重载

**新增** `XSAlgo_ShapeProcessor`：

```cpp
class XSAlgo_ShapeProcessor
{
public:
  // 参数容器
  ParameterMap;
  ProcessingData;
  ProcessingFlags;

  // 主流程
  ProcessShape(shape, OperationsFlags, progress);
  GetContext();
  MergeTransferInfo();
  CheckPCurve();

  // 参数装配
  ReadProcessingData();
  FillParameterMap();
  SetShapeFixParameters();
  SetParameter();
};
```

**影响**：自定义过 `XSAlgo_ToolContainer` 子类、或调用 `SetToolContainer` 注入自定义修复器的项目，必须改写为 `XSAlgo_ShapeProcessor` 的 `ParameterMap` 模式。这是 7.9 中**唯一需要修改业务代码**的接口重构。

#### 3.2.4 `StepData_ConfParameters` → `DESTEP_Parameters`

旧类实现文件删除，`StepData_ConfParameters.hxx` 保留为 deprecated 别名壳（23 → 506 token）。`STEPCAFControl_Reader` 等已全部切换到 `DESTEP_Parameters`。

配套地，形状修复参数从 `Interface_Static`（字符串键值）迁移到强类型结构体：

```cpp
struct DE_ShapeFixParameters
{
  enum class FixMode : signed char
  {
    FixOrNot = -1,  // 视情况执行
    NotFix   = 0,
    Fix      = 1
  };

  double           Tolerance3d       = 1.e-6;
  double           MaxTolerance3d    = 1.0;
  double           MinTolerance3d    = 1.e-7;
  TopAbs_ShapeEnum DetalizationLevel = TopAbs_ShapeEnum::TopAbs_VERTEX;
  bool             NonManifold       = false;

  FixMode FixFreeShellMode, FixFreeFaceMode, FixFreeWireMode, FixSameParameterMode,
          FixSolidMode, FixShellOrientationMode, CreateOpenSolidMode, FixShellMode,
          FixFaceOrientationMode, FixFaceMode, FixWireMode, FixOrientationMode,
          FixAddNaturalBoundMode, FixMissingSeamMode, FixSmallAreaWireMode,
          RemoveSmallAreaFaceMode, FixIntersectingWiresMode, FixLoopWiresMode,
          FixSplitFaceMode, AutoCorrectPrecisionMode, ModifyTopologyMode,
          ModifyGeometryMode, ClosedWireMode, PreferencePCurveMode, ...;  // 共 40+ 项
};
```

**需重点关注默认值与 7.8.1 的差异**（见 §3.3.4）：

- `CreateOpenSolidMode = NotFix`
- `FixTailMode = NotFix`
- `FixEdgeSameParameterMode = NotFix`
- `AutoCorrectPrecisionMode = Fix`

#### 3.2.5 旧 DE 包装类删除

删除 `DEBRepCascade_*`、`DEXCAFCascade_*`、`IGESCAFControl_*`、`RWGltf_*`、`RWObj_*`、`RWPly_*`、`RWStl_*`、`STEPCAFControl_*`、`Vrml_*` 中的一批实现文件。

**注意**：删除的是包装层实现，公开入口类（如 `STEPCAFControl_Reader`）仍可用。但若直接 include 被删的内部头、或继承了旧 Cascade 包装类，会编译失败。

#### 3.2.6 `Standard_Type` 内部布局变化

7.8.1：

```cpp
Standard_Type::Register(const std::type_info& theInfo, const char* theName, ...)
{
  Standard_Type* aType = new Standard_Type(theInfo, ...);   // 两次分配
  ...
}
// 成员: const std::type_info& myInfo;
```

7.9.3：

```cpp
void* aMem = Standard::AllocateOptimal(sizeof(Standard_Type) + anInfoNameLen + aNameLen);
Standard_Type* aType = new (aMem) Standard_Type(...);        // 一次分配 + placement new
// 成员: mySystemName, myName, mySize, myParent（不再持有 type_info 引用）
```

**影响**：`Name()`、`SystemName()`、`Parent()`、`SubType()` 等公开方法不变；依赖 `sizeof(Standard_Type)` 或直接读取内部字段的代码会断。正常使用 `DEFINE_STANDARD_RTTIEXT` 的代码不受影响。

#### 3.2.7 文件删除但 API 未丢失（无需处理）

| 被删文件 | 实际情况 |
|---|---|
| `MoniTool_TimerSentry.cxx` | 构造/析构已全部内联进 `.lxx`（diff 仅格式差异） |
| `gce_Root.cxx` | `gce_Root.hxx` + `gce_Root.lxx` 仍在 `FILES` 中 |
| `TopOpeBRep_DRAW.cxx` | `.hxx` 保留，头文件唯一差异是一个 `#include` 的缩进 |
| `TopClass_SolidExplorer.cxx/.hxx` | 内部辅助类；公开入口为 `BRepClass3d_SolidClassifier` |
| `TopClass_Classifier3d.gxx/.lxx` | 7.8.1 通过 `#define TopClass_Classifier3d BRepClass3d_SolidPassiveClassifier` + include `.gxx` 展开；7.9.3 内联进 `BRepClass3d_SolidPassiveClassifier.cxx` |
| `TCollection.cxx`、`TopClass.cxx` | 空文件 / 纯 include 聚合文件清除 |

### 3.3 行为与默认值变化（P1）

#### 3.3.1 `TopoDS_Shape` 变换异常默认关闭（最易踩坑）

7.9.3 四个方法的默认参数由 `Standard_True` 改为 `Standard_False`：

```cpp
TopoDS_Shape Location(const gp_Trsf& theLoc,          Standard_Boolean theRaiseExc = Standard_False) const;
TopoDS_Shape Located (const TopLoc_Location&,         Standard_Boolean theRaiseExc = Standard_False) const;
void         Move    (const TopLoc_Location&,         Standard_Boolean theRaiseExc = Standard_False);
TopoDS_Shape Moved   (const gp_Trsf&,                 Standard_Boolean theRaiseExc = Standard_False) const;
```

内部仅在标志为真时校验：

```cpp
if (theRaiseExc)
  validateTransformation(aTrsf);
```

**影响**：7.8.1 下，对带非均匀缩放或镜像的 `gp_Trsf` 调用 `Located()` 会抛异常；7.9.3 下**静默接受**。依赖该异常做输入校验的代码会漏检，可能得到退化几何而非明确报错。**建议显式传 `Standard_True` 保持旧行为。**

#### 3.3.2 `XSControl_WorkSession` 引入全局互斥锁

```cpp
// XSControl_WorkSession.cxx:37
static Standard_Mutex WS_GLOBAL_MUTEX;
```

在 `SelectNorm()`（L75）、`TransferReadRoots()`（L433）、`TransferWriteShape()`（L456）三处加锁。

**影响**：7.8.1 允许"多线程各自持有独立 `XSControl_WorkSession` 并行读写 STEP"；7.9.3 会被串行化。修复了并行下的状态污染，但**并行 STEP 导入吞吐会下降**。批处理场景需重新评估。

#### 3.3.3 `Standard_Type::IsKind` 递归改迭代 + 尺寸剪枝

7.8.1：

```cpp
return ! theOther.IsNull()
    && (theOther == this || (! myParent.IsNull() && myParent->SubType(theOther)));
```

7.9.3：

```cpp
const Standard_Type* aTypeIter = this;
while (aTypeIter && theOther->mySize <= aTypeIter->mySize)
{
  if (theOther.get() == aTypeIter)
    return true;
  aTypeIter = aTypeIter->Parent().get();
}
```

语义等价（`mySize` 沿继承链单调递减，剪枝安全），消除了深继承链的递归开销。**列为"看起来像逻辑改动、实际不是"的典型。**

#### 3.3.4 形状修复默认语义变化

`DE_ShapeFixParameters` 的 `FixMode` 三态（`FixOrNot` / `NotFix` / `Fix`）与 `Interface_Static` 旧布尔/字符串语义并非一一对应。升级后必须回归 STEP/IGES 读入的修复结果，重点核对 `CreateOpenSolidMode`、`FixTailMode`、`FixEdgeSameParameterMode`。

### 3.4 性能类源码改动

| 模块 | 文件 / 位置 | 改动 | 收益 |
|---|---|---|---|
| ShapeFix | `ShapeFix_Shell.cxx:203` 新增 `GetConnectedFaceGroups()`，L398 调用，`FixFaceOrientation` 位于 L1288 | 预建 `FaceEdgesMap` / `EdgeFacesMap`（`std::unordered_map` + `NCollection_Allocator`），DFS 求面连通分量；先处理最大连通分量 | **O(n³) → 近似 O(n)**；修复 #753/#769 |
| Standard | `Standard_Type.cxx` | 单次 `AllocateOptimal` + placement new；`IsKind` 迭代化 | 类型注册内存减半，RTTI 查询免递归 |
| StepData | `StepData_StepReaderData.cxx:1986` `SetEntityNumbers` | `TColStd_IndexedMapOfInteger imap(thenbents, anAlloc)` + `NCollection_IncAllocator` | 实体表构建减少堆分配 |
| StepData | `StepData_StepReaderData.cxx:385` `SetRecord` | 由 `Contains` + `FindIndex/Add` 两次查找改为 `thetypes.ChangeValue(num) = thenametypes.Add(...)` 一次 | 每实体省一次哈希查找 |
| Interface | `Interface_Graph.cxx:138` `Evaluate`，L145 | 复用单个 `HSequenceOfTransient`，每实体 `anAlloc2->Reset()` | 消除每实体一次序列分配 |
| BOPAlgo | `BOPAlgo_PaveFiller_6.cxx:671/675`，L744 | `aAllocator` / `aTmpAllocator`，每轮 `Reset(false)`；新增 `IsPlaneFF()`（L87）、`IsClosedFF()`（L109） | 布尔运算迭代期内存复用 |
| TopoDSToStep | `TopoDSToStep_MakeStepEdge.cxx:186/268` | `BRepAdaptor_Curve` / `BRepAdaptor_Surface` 延迟到实际分支构造 | 边导出避免无条件构造 adaptor |
| GeomTools | `GeomTools.cxx` `GetReal` | `constexpr size_t THE_BUFFER_SIZE = 32`（原 256） | 栈占用下降 |
| NCollection | 新增 `NCollection_Primes.cxx/.hxx` | 素数表独立编译单元 | 避免每个模板实例重复生成 |
| 构建 | `_toolkit` PCH | 原生 CMake PCH | 整库编译时间下降（默认关闭） |

### 3.5 功能新增

#### 3.5.1 glTF 读写器重写

`RWGltf_CafWriter.cxx`（12,154 token churn，占新文件 90.3%）拆出：`dispatchShapes`、`hasTriangulation`、`saveEdgeIndices`、`saveTriangleIndices`、`saveVertexIndices`、`toSkipShape`、`writeExtrasAttributes`、`writeMaterial`、`writeShapes`、`writeShapesToBin`；删除 `toSkipFaceMesh`。

`RWGltf_GltfJsonParser.cxx`（10,245 churn，83.0%）新增 `RWGltf_ExtrasParser`（`Parse` / `ParseExtras` / `parseArray` / `parseNumber` / `parseObject` / `parseString` / `getResult`）、`parseTransformationComponents`、`parseTransformationMatrix`。

配套新增 `RWMesh_EdgeIterator`、`RWMesh_ShapeIterator`、`RWMesh_VertexIterator`。**7.9.3 的 glTF 开始支持边与点云的导出/导入，7.8.1 仅支持三角面。**

#### 3.5.2 `AIS_Manipulator` 新增皮肤模式

```cpp
enum ManipulatorSkin { Shaded, Flat };

ManipulatorSkin SkinMode() const;               // L322
void SetSkinMode(ManipulatorSkin);              // L325
void attachToPoint(const gp_Pnt&);              // L439
void RecomputeTransformation(const camera);     // L221
void RecomputeSelection(mode);                  // L226
```

`AIS_Manipulator.cxx` churn 8,839（81.7%），为可视化模块最大改动文件。

#### 3.5.3 STEP AP242 元数据支持

新增 5 个实体类及对应 RW 类：

- `StepBasic_GeneralPropertyAssociation`
- `StepBasic_GeneralPropertyRelationship`
- `StepRepr_BooleanRepresentationItem`
- `StepRepr_RealRepresentationItem`
- `StepRepr_MechanicalDesignAndDraughtingRelationship`

`STEPCAFControl_Reader.cxx`（3,794 churn，10.0%）新增 include：`StepBasic_GeneralProperty`、`StepRepr_ValueRepresentationItem`、`StepRepr_ShapeAspectRelationship`、`StepAP242_IdAttribute`、`StepShape_GeometricCurveSet` 等。

#### 3.5.4 `STEPConstruct_Styles` 重构

抽出 `ProcessAsSurfaceStyleRendering` / `ProcessAsSurfaceStyleBoundary` / `ProcessAsSurfaceStyleFillArea`，churn 3,738（79.5%）。修正渲染属性、透明度与 negative side 的处理。

#### 3.5.5 `MeshVS_MeshPrsBuilder` 多边形三角化

`MeshVS_MeshPrsBuilder.cxx:58` 新增静态 `ProcessFace(...)`，用 `alloca` 做多边形三角化并添加顶点；`AddFaceSolidPrs` / `AddVolumePrs` 改为调用它。churn 5,050（76.4%）。

#### 3.5.6 其他

| 文件 | churn | 占比 | 内容 |
|---|---:|---:|---|
| `RWStepAP214_GeneralModule.cxx` | 5,316 | 15.5% | 实体表重写 |
| `STEPCAFControl_GDTProperty.cxx` | 2,107 | 32.2% | GD&T 属性处理 |
| `ChFi3d_Builder_2.cxx` | 1,293 | — | 倒角 |
| `StdSelect_BRepSelectionTool.cxx` | 1,004 | — | 选择 |
| `UnitsAPI_Units_dat.pxx` | 1,257 | — | 单位表数据重排 |

### 3.6 "假差异"识别与排除

| 成因 | 规模 | 识别方法 |
|---|---|---|
| clang-format 18.1.8 全库格式化 | 约 2,700 个文件 | token 流完全相同 |
| `.gxx` 模板并入 `.cxx` | 约 30 个文件 | token 多重集净变化 < 500 |
| `.hxx → .pxx` 改名 | 689 个文件 | 同 stem、同目录 |
| `#include` 排序 / 重排 | 广泛 | 仅影响编译依赖 |

`3.6.1` 已用 token 多重集逐文件验证的"巨量改动"文件：

| 文件 | 7.8.1 token | 7.9.3 token | 净变化 | 编辑 churn |
|---|---:|---:|---:|---:|
| `IntPatch_ImpImpIntersection.cxx`（并入 7 个 `.gxx`） | 45,256 | 45,088 | -168 | 168 |
| `IntWalk_IWalking.gxx`（并入 6 个） | 16,095 | 16,039 | -56 | 56 |
| `Blend_Walking` → `BRepBlend_Walking` | 14,127 | 13,845 | -282 | 482 |
| `Blend_CSWalking` → `BRepBlend_CSWalking` | 4,225 | 3,945 | -280 | 306 |

即：churn 99% 的大文件，真实逻辑变化为 0。

---

## 4. 需求定义

### 4.1 功能性需求

| 编号 | 需求 | 优先级 | 验收标准 |
|---|---|---|---|
| R-01 | 工程可在 OCCT 7.9.3 下完整编译（含全部工具包） | P0 | 0 error；deprecated 告警清单归档 |
| R-02 | `NCollection_Map` 布尔运算全部迁移到 `NCollection_MapAlgo` | P0 | 无 `NCollection_Map::Unite/Intersect/...` 调用点 |
| R-03 | XSAlgo 自定义扩展迁移到 `XSAlgo_ShapeProcessor` | P0 | 无 `XSAlgo_ToolContainer` / `SetToolContainer` 引用 |
| R-04 | 版本号获取适配构建期生成的 `Standard_Version.hxx` | P0 | 非 CMake 构建路径可正常获取 `OCC_VERSION_*` |
| R-05 | 形状修复参数迁移到 `DE_ShapeFixParameters` | P1 | 关键修复项的默认值与 7.8.1 行为一致或已确认差异 |
| R-06 | STEP / IGES 读入回归 | P1 | 样本集几何、颜色、图层、GD&T 与 7.8.1 一致 |
| R-07 | 并行 STEP 读写吞吐评估 | P1 | 记录串行化后的耗时；必要时改为流水线并行 |
| R-08 | 变换异常校验策略确认 | P1 | 所有 `Located/Moved/Location/Move` 调用点明确传入 `theRaiseExc` |
| R-09 | glTF 往返测试 | P2 | 三角面输出与 7.8.1 逐字节可比；边/点云新能力可用 |
| R-10 | 构建时间基线对比 | P2 | 开启 PCH 前后编译时间记录 |

### 4.2 非功能性需求

| 编号 | 需求 | 指标 |
|---|---|---|
| N-01 | 几何算法结果可复现 | 关键算法输出与 7.8.1 的偏差在既有容差内 |
| N-02 | 编译告警不增加 | 新增 deprecated 告警全部清零后再进入 8.x |
| N-03 | 二进制兼容 | **不要求**；7.9.3 起不保证 ABI 兼容，需全量重编 |
| N-04 | 第三方依赖 | 确认 TBB / FreeType / Tcl-Tk 版本满足 C++17 构建 |

---

## 5. 设计方案

### 5.1 升级路径选择

```
7.8.1 ──► 7.9.3 ──► 8.0.1
          (本文档)   (后续)
```

**推荐分两步走**，理由：

1. 7.9.3 与 7.8.1 源码布局相同，改动面小，可先验证几何算法回归。
2. 8.0.1 的破坏性变更（目录重构、`Handle`/`Standard_*` 移除、`NCollection_Map` 旧方法删除）在 7.9.3 中已全部以 deprecated 形式出现，先在 7.9.3 清干净告警，8.0.1 迁移量可下降一个量级。

### 5.2 分阶段实施

#### 阶段 0：基线固化（0.5 人日）

- 固化 7.8.1 的几何结果基线（现有回归样本集）。
- 记录编译时间、二进制体积、关键场景耗时。
- 建立 `deprecated` 告警清单（7.9.3 编译后导出）。

#### 阶段 1：源码适配（2–4 人日）

按 §4.1 的 P0 项执行：

1. 全局替换 `NCollection_Map` 布尔运算调用（可用脚本 + 人工复核）。
2. 清理 `XSAlgo_ToolContainer` / `XSAlgo_Caller` 引用。
3. 处理 `Standard_Version.hxx` 的构建路径。
4. 修复被删除 DE 包装类的 include / 继承点。
5. 检查 `RWStep*` 内部头的直接 include。

#### 阶段 2：行为回归（3–5 人日）

1. 逐个确认 `TopoDS_Shape` 变换调用点的 `theRaiseExc` 参数。
2. STEP / IGES / BREP / glTF 读写回归。
3. 形状修复结果对比（重点：`DE_ShapeFixParameters` 默认值差异）。
4. 并行 STEP 导入性能测试。
5. 布尔运算 / 倒角 / 缝合等建模算法回归。

#### 阶段 3：收益落地（可选，1–2 人日）

1. 开启 PCH，记录编译时间收益。
2. 切换 C++17 构建。
3. 评估 glTF 边/点云导出、STEP AP242 元数据的新能力。
4. 评估 `ShapeFix_Shell` 提速对修补流程的影响。

#### 阶段 4：8.0.1 前置（后续）

- 清除全部 deprecated 告警。
- `Handle` → `occ::handle`、`Standard_*` → 原生类型的渐进替换。

### 5.3 改造点速查表

| 现象 | 位置 | 改法 |
|---|---|---|
| 找不到 `Standard_Version.hxx` | 构建脚本 / include 路径 | 使用 CMake 生成目录，或改用 `Standard_VersionInfo` |
| `NCollection_Map::Unite` 等告警 | 业务代码 | 改用 `NCollection_MapAlgo::Unite` 等 |
| `XSAlgo_ToolContainer` 未定义 | 自定义转换器 | 改用 `XSAlgo_ShapeProcessor::ParameterMap` |
| `StepData_ConfParameters` 告警 | STEP 读取配置 | 改用 `DESTEP_Parameters` |
| `RWStepXxx_RWyyy.hxx` 不存在 | 直接 include 内部头 | 改 include `*_ReadWriteModule.hxx` / `*_GeneralModule.hxx` |
| `Located()` 不再抛异常 | 几何变换调用点 | 显式传 `Standard_True` |
| 并行 STEP 变慢 | 批处理调度 | 改为流水线并行或进程级并行 |

### 5.4 工作量评估

| 阶段 | 人日 | 说明 |
|---|---:|---|
| 阶段 0 基线固化 | 0.5 | 一次性 |
| 阶段 1 源码适配 | 2–4 | 取决于 `NCollection_Map` 与 XSAlgo 的使用密度 |
| 阶段 2 行为回归 | 3–5 | 取决于格式读写与建模算法的覆盖度 |
| 阶段 3 收益落地 | 1–2 | 可选 |
| **合计** | **6.5–11.5** | 不含 8.0.1 |

---

## 6. 风险与应对

| 风险 | 概率 | 影响 | 应对 |
|---|---|---|---|
| `NCollection_Map` 调用点遗漏 | 中 | 8.x 编译失败 | 阶段 1 用 `grep` 全量扫描 + 编译告警清零 |
| 形状修复默认值变化导致几何差异 | 中 | 修复结果与旧版不一致 | 参数逐项对比，必要时显式指定全部 `FixMode` |
| 并行 STEP 吞吐下降 | 中 | 批处理耗时上升 | 阶段 2 实测；必要时改为进程级并行 |
| `Located()` 静默接受非法变换 | 低 | 退化几何流入下游 | 调用点显式传 `True`，并在 API 层封装 |
| 非 CMake 构建找不到生成头 | 低 | 构建失败 | 阶段 1 早期验证 |
| `.pxx` 私有化影响下游 include | 低 | 编译失败 | 全量扫描 `#include <RWStep...hxx>` |

---

## 7. 验收标准

1. **编译**：全部工具包在 7.9.3 下 0 error；deprecated 告警清单归档并有处置结论。
2. **功能**：回归样本集的 STEP / IGES / BREP / glTF 往返结果与 7.8.1 一致（几何容差内、属性完全一致）。
3. **性能**：关键算法耗时不低于 7.8.1 的 95%；`ShapeFix_Shell` 在大多数场景应显著提升。
4. **构建**：开启 PCH 后编译时间记录在案；C++17 构建通过。
5. **文档**：本文档随实际改动更新；所有"不需处理"项在代码中确认无残留。

---

## 8. 附录

### 8.1 可复现方法

```bash
# 1. 包级归一化（硬链接，不占空间）
python scripts/build_normalized.py
python scripts/build_normalized_793.py

# 2. token 流（去注释/空白/机械重命名）
python scripts/normalize_tokens.py

# 3. 行级 diff
git diff --no-index --numstat normalized/781 normalized/793  > data/diff-numstat-781-793.txt
git diff --no-index -w --numstat normalized/781 normalized/793 > data/diff-numstat-w-781-793.txt

# 4. token diff
git diff --no-index -w --numstat tok2/781 tok2/793 > data/diff-numstat-tok2-781-793.txt

# 5. 统计
python scripts/deep_analysis2.py    # 结构、扩展名迁移、分模块 churn
python scripts/pair_ext.py          # 扩展名迁移配对
python scripts/token_multiset.py    # token 多重集净变化
python scripts/final_stats.py       # 文件级分布、新增/删除构成
```

### 8.2 数据文件说明

| 文件 | 内容 |
|---|---|
| `data/diff-numstat-tok2-781-793.txt` | token 归一化后的逐文件增删行数 |
| `data/diff-by-package.csv` | 按包汇总的变更量 |
| `data/commits-793-all.json` | 7.9.0–7.9.3 的 400 条提交记录 |
| `reference/UPGRADE-PATHS.md` | 7.8.1 → 7.9.3 → 8.0.1 升级路径与兼容矩阵 |
| `reference/REPORT.md` | 7.8.1 → 8.0.1 代码结构与变化总量分析（8.x 背景） |

### 8.3 关键数字速记

- 语义净变化：**+29,542 token（+0.26%）**；净 churn 64,918（0.56%）。
- 真实新增 98 个文件，删除 66 个文件。
- `.hxx → .pxx` 689 个（全部 `RWStep*`）。
- 新增 9 个 DE 包；包归属调整 3 个（`Expr`、`ExprIntrp`、`Precision`）。
- 编译级必改点 **4 处**；行为级必测点 **3 处**。
- 预估工作量 **6.5–11.5 人日**。
