# OCCT 7.8.x → 7.9.3 / 8.0.1 升级对比

> 实测源码：`V7_8_1`（2024-04-01）、`V7_9_3`、`V8_0_1`。
> 统计口径与 `REPORT.md` 相同：包级归一化视图 + token 流（去注释/空白/机械重命名）。
> 官方没有 `V7_8_3` tag；若“7.8.3”是厂商补丁，其 API 与 7.8.1 基本一致；若其实指 7.9.3，见第 2、3 节。

---

## 1. 结论

1. **7.8.x → 7.9.3 是小版本升级。**
   - Token 语义编辑量 **30.5 万**（8.0.1 是 **486 万**，约为其 1/16）。
   - 净代码量 11,528,290 → 11,557,822 token，**+0.3%**。
   - **0 个包被删除**，新增 9 个 `DE*` 包，公共类/接口基本全部保留。
   - 真正需要改代码的点只有：XSAlgo 重构、TopClass 3D 分类器删除、约 690 个内部 `.hxx→.pxx`、C++17、`TopoDS_Shape` 变换异常默认值变化。

2. **7.8.x → 8.0.1 是大版本断裂。**
   - Token 编辑量 **486 万**，净代码 **+17.8%**（其中 GTests 占 121 万）。
   - 9 个包 + `ExpToCasExe` 删除，34 个包新增，6 个工具包迁移。
   - 异常 `Raise`、几何 `EvalD0` 虚函数体系、`StepType` 返回类型、`PLib`/`DE`/`BRepMesh` 旧接口等均为**无兼容层删除**。

3. **先升 7.9.3 只是“低风险跳板”，不会显著减少 8.0.1 的手工适配。**
   - 7.9.3 → 8.0.1 的 token churn 仍有 **438 万**，约为 7.8.1 → 8.0.1 的 **90%**。
   - 7.9.3 的价值在于：作为 7.x 终点长期停留；或作为升级 8.0.1 前的可编译检查点（先把 deprecation 警告清掉）。

---

## 2. 量化对比

| 指标 | 7.8.1 → 7.9.3 | 7.8.1 → 8.0.1 | 7.9.3 → 8.0.1 |
|---|---:|---:|---:|
| 原始行 churn | 2,544,392 | 3,507,153 | — |
| 忽略空白行 churn（`-w`） | 1,366,680 | 2,689,883 | 1,783,099 |
| Token 编辑 churn | **304,872** | **4,859,548** | **4,378,250** |
| Token 净变化 | **+29,532（+0.3%）** | **+2,048,576（+17.8%）** | +2,019,044（+17.5%） |
| Token 多重集净 churn | 65,164 | 2,431,020 | — |
| Token diff 文件条目 | 2,766 | 15,133 | 14,372 |
| 包数量 | 384 → 393 | 384 → 409 | 393 → 409 |
| 真正删除的包 | **0** | 9（+ExpToCasExe） | 10 |
| 新增包 | 9 | 34 | 21 |
| `.hxx → .pxx` | **689** | 691 | **2** |
| 默认 C++ 标准 | C++11 → **C++17** | C++17 | C++17 |

说明：

- 7.8.1 → 7.9.3 的原始行 churn 仍有 254 万，主要来自 7.9.0 的**全库 clang-format 18.1.8 重排**；token 归一化后只剩 30 万，说明语义改动很小。
- 7.9.3 → 8.0.1 的净增 201 万 token 中，`GTests`（7.x 没有）约 **121 万**；剔除后核心净增约 **80 万（+7%）**。
- 7.9.3 → 8.0.1 的 `.hxx→.pxx` 只剩 **2 对**：这一步在 7.9.0 已完成。

### 7.9.3 内部 churn 分布

| 模块 | 修改文件数 | Token churn |
|---|---:|---:|
| ModelingAlgorithms | 325 | 73,962 |
| DataExchange | 1,651 | 72,171 |
| Visualization | 162 | 20,895 |
| Draw | 91 | 6,178 |
| FoundationClasses | 207 | 5,720 |
| ModelingData | 154 | 2,367 |
| ApplicationFramework | 27 | 463 |
| DETools | 8 | 22 |

DataExchange 的文件数多但 churn 小，主要是 `RWStep*` 的 `#include` 从 `.hxx` 改成 `.pxx`（每文件 1–2 个 token）。

Top 包（token churn）：`IntPatch` 45k、`RWGltf` 23.6k、`IntWalk` 16k、`RWStepAP214` 10.1k、`AIS` 9.3k、`STEPCAFControl` 7.1k、`MeshVS` 5.2k。

---

## 3. 7.8.x → 7.9.3 的结构变化

| 项目 | 7.8.1 | 7.9.3 |
|---|---|---|
| 布局 | `src/<Package>/` | 不变 |
| 模块 | 8 个 | 新增 `DETools`（含 `TKExpress`、`ExpToCasExe`） |
| 包 | 384 | 393（+9 个 DE 包，0 删除） |
| 工具包迁移 | — | `Precision`：TKMath → TKernel |
| 模块迁移 | — | `Expr`、`ExprIntrp`：FoundationClasses/TKMath → DETools/TKExpress |
| 内部头 | 7,827 `.hxx` / 53 `.pxx` | 7,177 `.hxx` / 747 `.pxx`（689 对迁移） |
| 代码格式 | — | 全库 clang-format 18.1.8 |

新增的 9 个包：`DEBREP`、`DEXCAF`、`DESTEP`、`DEIGES`、`DEGLTF`、`DEOBJ`、`DEPLY`、`DESTL`、`DEVRML`。

这些新包与旧的 `DEBRepCascade`、`DEXCAFCascade`、`IGESCAFControl`、`RWGltf`、`RWObj`、`RWPly`、`RWStl`、`STEPCAFControl`、`Vrml` **并存**：旧包中只留下一个 `Standard_DEPRECATED` 的 typedef 别名头。

---

## 4. 7.8.x → 7.9.3：真正的构建破坏点

### 4.1 硬编译错误（无别名）

| 项目 | 7.8.1 | 7.9.3 | 处理 |
|---|---|---|---|
| `XSAlgo_Caller`（enum） | 存在 | 删除 | 无直接替代；OCCT 内部使用，外部少见 |
| `XSAlgo_ToolContainer`（类） | 存在 | 删除 | 迁移到 `XSAlgo_ShapeProcessor` |
| `XSAlgo_AlgoContainer::SetToolContainer()` / `ToolContainer()` | 存在 | 删除 | 迁移到 `XSAlgo_ShapeProcessor` |
| `XSAlgo_AlgoContainer::ProcessShape(..., ShapeBuild_ReShape&, ...)` 重载 | 存在 | 删除 | 用不带 `ShapeBuild_ReShape` 的重载 |
| `TopClass_Classifier3d` | 存在 | 删除 | `BRepClass3d_SolidPassiveClassifier` |
| `TopClass_SolidClassifier` | 存在 | 删除 | `BRepClass3d_SolidClassifier` |
| `TopClass_SolidExplorer` | 存在 | 删除 | `BRepClass3d_SolidExplorer` |
| `IGESCAFControl_ConfigurationNode::IGESCAFControl_InternalSection` | 存在 | 删除 | `DEIGES_Parameters` / `XSAlgo_ShapeProcessor::ParameterMap` |
| 689 个内部头 `.hxx` | `.hxx` | `.pxx` | 直接 include 旧头的代码改为 `.pxx` 或改用公共头（主要是 `RWStep*`） |

> `TopClass_*` 与 `XSAlgo_Caller` 在 7.8.1 中基本只被 OCCT 自己使用；普通 CAD 应用很少直接引用。

### 4.2 链接目标变化

| 包 | 7.8.1 工具包 | 7.9.3 工具包 | 影响 |
|---|---|---|---|
| `Expr`、`ExprIntrp` | TKMath | TKExpress（DETools） | 用到表达式解析的项目需加链 `TKExpress` |
| `Precision` | TKMath | TKernel | 通常由 TKMath 传递依赖，显式链接列表需更新 |

### 4.3 仅警告（`/WX`、`-Werror` 下会变成错误）

| 项目 | 7.9.3 状态 | 替代 |
|---|---|---|
| `Handle_XXX` 类型名 | `Standard_DEPRECATED` | `Handle(XXX)` |
| `NCollection_Map::Union/Unite/Intersection/Intersect/Subtraction/Subtract/Difference/Differ` | 保留但 deprecated | `NCollection_MapAlgo::*` |
| 旧 DE 类名（`DEBRepCascade_*`、`IGESCAFControl_*` 等） | deprecated typedef | `DE<Format>_*` |

抑制方式：定义 `OCCT_NO_DEPRECATED`（`Standard_DEPRECATED` 展开为空），或 MSVC `/wd4996`、GCC/Clang `-Wno-deprecated-declarations`。

### 4.4 行为变化（编译通过，但结果不同）

| 项目 | 7.8.1 | 7.9.3 |
|---|---|---|
| `TopoDS_Shape::Move/Moved/Located` 的 `theRaiseExc` 默认值 | `true` | **`false`** |
| 带缩放/负行列式变换 | 默认抛异常 | 默认静默接受；需要旧行为时显式传 `true` |
| 形状修复参数来源 | 资源文件 / `Interface_Static` | `DE_ShapeFixParameters` + `XSAlgo_ShapeProcessor` |
| DE 参数来源 | `Interface_Static` | `DE<Format>_Parameters`（仅初始化读一次） |

形状修复参数若仍用旧资源文件/`Interface_Static` 设置，可能**不再生效**，需要改为：

```cpp
STEPControl_Reader aReader;
XSAlgo_ShapeProcessor::ProcessingData aData =
  XSAlgo_ShapeProcessor::ReadProcessingData("read.step.resource.name",
                                            "read.step.sequence");
aReader.SetShapeFixParameters(std::move(aData.first));
aReader.SetShapeProcessFlags(aData.second);
```

### 4.5 7.9.3 仍然兼容的 8.0 破坏点

这些在 7.9.3 里**全部还在**，所以 7.8.x → 7.9.3 不会遇到：

| 8.0.1 会删除/改变 | 7.9.3 状态 |
|---|---|
| `Standard_Failure::Raise/Instance/Throw`、子类 `Raise/NewInstance/Reraise` | 仍存在 |
| `Standard_ErrorHandler::Catches/LastCaughtError` | 仍存在 |
| `Geom_Curve/Geom_Surface/Geom2d_Curve` 的虚 `D0..DN` | 仍是纯虚，未引入 `EvalD0` |
| `StepData_ReadWriteModule::StepType` 返回 `TCollection_AsciiString` | 未变 |
| `Standard_Integer/Real/...` typedef | 未标记 deprecated |
| `TColStd_*`、`TopTools_*`、`TColgp_*` typedef | 未标记 deprecated |
| `Bnd_B2d.hxx`、`Bnd_B3d.hxx` | 仍存在（还是独立类） |
| `BRepMesh_DiscretFactory::SetDefault(name, factory)` | 仍存在 |
| `BRepMesh_PluginMacro.hxx` | 仍存在 |
| `PLib_Base`、`PLib_DoubleJacobiPolynomial` | 仍存在 |
| `PLib_JacobiPolynomial` 派生自 `Standard_Transient` | 仍可用 `Handle(...)` |
| `TopTools_MutexForShapeProvider`、`OSD_MAllocHook` | 仍存在 |
| `GeomEvaluator*`、`Geom2dEvaluator*`、`LProp3d`、`Geom2dLProp` 包 | 仍存在 |
| `StepData_ConfParameters` | 仍可用（已是 `DESTEP_Parameters` 的别名） |
| `NCollection_Array1::Assign` 要求长度相同 | 语义未变 |
| `BRep_Tool::CurveOnPlane` 失败时抛异常 | 未变 |
| `Bnd_Range::IsIntersected` 返回 `int` | 未变 |
| `IsClosed/IsPeriodic` 使用 `gp::Resolution()` | 未变 |

---

## 5. 7.8.x → 8.0.1：硬破坏清单

### 5.1 异常体系

- `Standard_Failure::Raise()` / `Instance()` / `Throw()` 删除。
- `DEFINE_STANDARD_EXCEPTION` 不再生成 `Raise()` / `NewInstance()` / `Reraise()`。
- `Standard_ErrorHandler::Catches()` / `LastCaughtError()` 删除。
- 迁移：`throw Standard_Failure("...")`、`throw Standard_OutOfRange("...")`。
- `Standard_Failure` 改为继承 `std::exception`；`GetMessageString()` 仅为 deprecated，`what()` 可用。

### 5.2 几何求值体系

- `Geom_Curve`、`Geom_Surface`、`Geom2d_Curve` 新增纯虚 `EvalD0/EvalD1/EvalD2/EvalD3/EvalDN`；`Value()`、`D0..DN` 变成非虚 inline 包装。
- `Adaptor3d_Curve`、`Adaptor3d_Surface` 的 `D0` 变成非虚；自定义派生类必须改写 `EvalD0`。
- `Adaptor2d_Curve2d` 仍保留虚 `D0`，`EvalD0` 默认调用 `D0`，向后兼容。
- 只**调用** `D0/Value` 的代码不受影响；**派生并 override** `D0` 的代码会编译失败或静默失效。

### 5.3 数据交换

- `StepData_ReadWriteModule::StepType()` 返回 `const std::string_view&`。
- `StepData_ConfParameters` 删除 → `DESTEP_Parameters`。
- 旧 DE 配置类头（`DEBRepCascade_*`、`IGESCAFControl_*`、`RWGltf_*` 等）在 8.0.1 已删除；7.9.3 的别名不再存在。
- `BRepMesh_PluginMacro.hxx`、`BRepMesh_PluginEntryType.hxx`、`BRepMesh_FactoryError.hxx` 删除；`BRepMesh_DiscretFactory::SetDefault(name, factory)` 删除（保留 `SetDefaultName`、`Get`、`Discret`）。
- `Express` 包、`ExpToCasExe` 工具整体删除。

### 5.4 删除的类 / 包

`PLib_Base`、`PLib_DoubleJacobiPolynomial`、`TopTools_MutexForShapeProvider`、`OSD_MAllocHook`、`GeomEvaluator_*`、`Geom2dEvaluator_*`、`LProp3d_*`（→ `LProp_*3d`）、`Geom2dLProp_*`（→ `GeomLProp_*2d`）、`BRepLProp_CurveTool`、`GeomLProp_CurveTool/SurfaceTool`、`Express_*`、`QANCollection`、`NCollection_CellFilter_InspectorXY/XYZ`、`Bnd_B2d.hxx`/`Bnd_B3d.hxx` 头。

另有类型语义变化：

- `PLib_JacobiPolynomial`、`PLib_HermitJacobi` 改为值类型（不再 `Standard_Transient`）→ `Handle(PLib_JacobiPolynomial)` 编译失败。
- `X` → `occ::handle<X>` 的迁移是机械的，`Handle()` 宏本身仍保留；`Handle_X` 在 8.0.1 仍保留（deprecated）。
- `TColGeom`、`TColGeom2d`、`TColgp` 包目录删除，但 `Deprecated/NCollectionAliases/` 提供了 972 个别名头（**只装到 install 目录，不复制到构建树 `inc/`**）。

### 5.5 容器/数据类型行为

| 项目 | 7.8.x | 8.0.1 |
|---|---|---|
| `NCollection_Array1/2::Assign`、`operator=` | 长度必须相同，否则抛 `Standard_DimensionMismatch` | 按源数组边界重新分配并复制 |
| 严格复制 | `Assign` | 新增 `CopyValues()`，长度不同抛异常 |
| `Bnd_Range::IsIntersected` | 返回 `int` | 返回 `Bnd_Range::IntersectStatus` 枚举 |
| `Bnd_Box::Center()` | 不存在 | 新增，返回 `std::optional<gp_Pnt>` |
| `BRep_Tool::CurveOnPlane` | 非平面/投影失败抛异常 | 返回空 handle |
| `IsClosed/IsPeriodic` 容差 | `gp::Resolution()` | `Precision::Computational()` |
| `Geom_BSplineCurve::Weights()` | 返回指针（可空） | 指针版 deprecated，新增 `WeightsArray()` 返回引用 |
| `TopoDS_TShape::ShapeType()` | 虚函数，可 override | 非虚，自定义 TShape 子类不能再 override |

### 5.6 构建系统

- 源码布局：`src/<Package>/` → `src/<Module>/<Toolkit>/<Package>/`。
- C++17 强制；默认 `BUILD_CPP_STANDARD=C++17`。
- 资源移到 `resources/`，安装到 `share/<OCCT_PROJECT_NAME>/resources/`。
- `USE_VTK`、`USE_RAPIDJSON` 默认 OFF；glTF 需要 RapidJSON。
- `BUILD_PATCH` 删除。
- Inspector、ExpToCas 移出主仓库。
- 提供迁移工具：`adm/scripts/migration_800/`（12 个阶段，支持 `--dry-run`）。

### 5.7 仅警告

`Standard_*` typedef、`Standard_True/False`、`Handle_*`、`TColStd_*`/`TopTools_*`/`TColgp_*` 别名、`GetMessageString()`、`Standard_Mutex`、`Weights(NCollection_Array1&)`、`Poles/Weights/Knots` 的 copy-out 重载、`GProp_*` 点云类、数学包装函数（`ACos`/`Sin`/`Sqrt`/`Min`/`Max`）等。

在 `-Werror`/`/WX` 下会变成错误；用 `OCCT_NO_DEPRECATED` 或编译器选项抑制。

---

## 6. 三条升级路线

| 路线 | 工作量 | 风险 | 适用 |
|---|---|---|---|
| **A. 7.8.x → 8.0.1 直接升级** | 最大；token churn 486 万 | 高，但一次到位 | 需要用 BRepGraph、GeomBndLib、EvalD*、新 DE 等 8.0 能力 |
| **B. 7.8.x → 7.9.3 并停留** | 小；token churn 30 万 | 低 | 只需要稳定 7.x，不追 8.0 新架构 |
| **C. 7.8.x → 7.9.3 → 8.0.1** | 中；两段合计 churn 468 万 | 中 | 需要可编译中间检查点；便于先清 deprecation 警告 |

建议：

- **如果最终目标是 8.0.1**：可以直接走 A；7.9.3 只节省约 10% 的 token 适配量，且 8.0.1 的主要手工项（异常、EvalD0、StepType、PLib、DE 头删除）在 7.9.3 中都没有预演。直接升级还能用 8.0.1 自带的 `migration_800` 工具一次性改写。
- **如果只是想升级到“较新的 7.x”**：7.9.3 是很好的终点，改动小、API 稳。
- **如果团队希望分两步**：C 路线可行，但要明确 7.9.3 只是“清警告 + 修 XSAlgo/TopClass + 适应 C++17”的检查点，不是 8.0.1 的兼容层。

---

## 7. 代码自查清单

```bash
# 1) 异常 Raise 用法（8.0.1 硬错误）
grep -rnE "::(Raise|NewInstance|Reraise|Instance|Throw)\s*\(" --include=*.h --include=*.hxx --include=*.cxx .

# 2) 自定义 Geom/Adaptor 派生类（8.0.1 硬错误）
grep -rnE "class .*: *public (Geom_Curve|Geom_Surface|Geom2d_Curve|Adaptor3d_Curve|Adaptor3d_Surface)" .
grep -rnE "void (D0|D1|D2|D3|DN)\s*\(.*\).*override" .

# 3) 自定义 STEP 模块（8.0.1 硬错误）
grep -rn "StepType" --include=*.hxx --include=*.cxx .

# 4) 7.9.3 的 XSAlgo/TopClass 用法
grep -rnE "XSAlgo_(Caller|ToolContainer)|SetToolContainer|ToolContainer\s*\(" .
grep -rnE "TopClass_(Classifier3d|SolidClassifier|SolidExplorer)" .

# 5) 8.0.1 已删除的类/头
grep -rnE "PLib_Base|PLib_DoubleJacobiPolynomial|TopTools_MutexForShapeProvider|OSD_MAllocHook|GeomEvaluator_|Geom2dEvaluator_|LProp3d_|Geom2dLProp_|StepData_ConfParameters|BRepMesh_PluginMacro|NCollection_CellFilter_InspectorXY|NCollection_CellFilter_InspectorXYZ" .

# 6) DE 旧类名（7.9.3 警告，8.0.1 错误）
grep -rnE "(DEBRepCascade|DEXCAFCascade|IGESCAFControl|RWGltf|RWObj|RWPly|RWStl|STEPCAFControl|Vrml)_(ConfigurationNode|Provider)" .

# 7) 内部 .pxx 头
grep -rn "#include *[<\"]RWStep.*\.hxx" .

# 8) 变换异常依赖
grep -rnE "\.(Move|Moved|Located)\s*\(" . | head
```

---

## 8. 数据来源与复现

- 源码：`OCCT-7.8.1/`、`OCCT-7.9.3/`、`OCCT-8.0.1/`（官方 tag tarball）。
- 归一化：`build_normalized.py`、`build_normalized_793.py`。
- Token 流：`normalize_tokens.py`（781/801）+ 复用其 `run()` 生成 `tok2/793`。
- Diff：`diff-numstat-w-781-793.txt`、`diff-numstat-tok2-781-793.txt`、`diff-numstat-w-793-801.txt`、`diff-numstat-tok2-793-801.txt`。
- 官方升级说明：`OCCT-7.9.3/dox/upgrade/upgrade.md`（7.9.0 章节）、`OCCT-8.0.1/dox/upgrade/upgrade.md`（8.0.0 章节）。
