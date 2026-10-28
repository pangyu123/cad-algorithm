# OCCT 7.8.1 → 8.0.1 代码结构与变化总量分析

> 数据来源：官方 tag `V7_8_1`（2024-04-01）与 `V8.0.1`（2026-07-30）源码 tarball。
> 两版目录布局不同，所有统计均基于 `normalized/781` 与 `normalized/801`（包级归一化视图）。
> 复现脚本与中间文件见本文末尾。

---

## 0. 结论摘要

1. **"结构变化大"主要是布局和文件组织，不是逻辑重组。**
   - 目录从 `src/<Package>/` 平铺改为 `src/<Module>/<Toolkit>/<Package>/`，15,000 余个文件全部换了路径。
   - 逻辑包数量 384 → 409，其中 **367 个包模块+工具包归属完全不变**；只有 6 个包换了工具包、2 个包换了模块。
   - 真正删除的包 9 个（Express、QANCollection、4 个旧 evaluator/LProp 包、3 个 TCol 别名包），新增 34 个（BRepGraph、Geom*Eval、Math*、DE* 等）。

2. **代码变化总量按口径差异极大：**

   | 口径 | 变化量 | 相对规模 |
   |---|---:|---:|
   | Git 原始行 churn | **3,507,153 行**（+1,976,146 / −1,531,007） | 18,434 个文件 |
   | 忽略空白行 churn | **2,689,883 行**（+1,567,511 / −1,122,372） | 18,276 个文件 |
   | Token 级编辑 churn（注释/格式/机械重命名已消除） | **4,860 k tokens** | 15,133 个文件 |
   | Token 多重集净变化（移动/改名/合并抵消） | **+2,049 k tokens（+17.8%）** | 11.53M → 13.58M |

3. **扣除新增单元测试后，代码体量净增约 +7.7%。**
   - GTests 在 8.0.1 进入 `src/`：+1,213 k tokens（约 48 万行源码行中的测试代码），占全部净增的 59%。
   - 剔除 GTests、QABugs 迁移、`_toolkit`（含 972 个兼容别名头）后：
     - 核心代码 token：11,348,948 → 12,221,257，**+872,309（+7.7%）**；
     - 净 token churn：**1,071,439（约核心代码的 9.4%）**。

4. **大部分"变化文件"其实几乎没变：**
   - 11,588 个被修改的源文件中，**1,463 个（12.6%）** 在 token 归一化后完全相同（纯格式/纯机械重命名）；另有 165 个新增 + 165 个删除文件也是 token 完全相同的纯移动。
   - 另有 4,722 个文件 token 变化 < 20；**只有 88 个文件** token 变化 > 2,000，却占了修改文件 token churn 的 40%。

5. **公共 API 层面：头文件中类/结构定义 6,216 → 6,673，新增 608、删除 151、保留 6,065。**
   删除的 151 个里约 17 个是 Express 解析器类，其余多为合并进新类的旧实现（如 `Bnd_B2d/B2f` → `Bnd_B2`）。

---

## 1. 度量口径说明

| 口径 | 做法 | 适合回答 |
|---|---|---|
| 原始行 churn | `git diff --no-index --numstat` | CI/评审看到的改动量 |
| 忽略空白 | 加 `-w` | 去掉缩进/换行噪声 |
| Token 级编辑 churn | 去注释、去空白、统一机械重命名后，每个 token 一行再 diff | 真实编辑量（但文件拆分/合并会双计） |
| Token 多重集净变化 | 按包统计 token 频次差 `Σ|count_new − count_old|` | 代码体量净增减，移动/改名/拆分全部抵消 |

Token 归一化掉的机械变化：

- `occ::handle<T>` / `opencascade::handle<T>` / `Handle_T` → `Handle(T)`
- `Standard_Integer/Real/Boolean/Byte/...` → `INT/REAL/BOOL/U8/...`
- `std::int32_t/uint8_t/size_t` 等 → 同一套规范 token
- `Standard_True/False` → `TRUE/FALSE`
- `Standard_EXPORT` / `Standard_HIDDEN` → 删除
- 注释、空行、缩进、换行位置

未归一化、仍计的机械变化：

- `TColStd_Array1OfReal` → `NCollection_Array1<double>` 这类容器别名替换（数量大，但属于 API 现代化）
- 命名空间/头文件名变化

---

## 2. 结构变化

### 2.1 目录布局

| | 7.8.1 | 8.0.1 |
|---|---|---|
| 布局 | `src/<Package>/<file>` | `src/<Module>/<Toolkit>/<Package>/<file>` |
| 模块 | 9 个（含 DETools） | 7 个 + `Deprecated` |
| 工具包元数据 | `src/<TK>/{PACKAGES,EXTERNLIB,FILES}` | `src/<Module>/<TK>/*.cmake` |

模块：FoundationClasses、ModelingData、ModelingAlgorithms、Visualization、ApplicationFramework、DataExchange、Draw。
`DETools` 被撤销，`TKExpress` 工具包并入 ModelingAlgorithms；EXPRESS 解析器（`Express` 包）与 `ExpToCasExe` 工具整体删除。

### 2.2 包 / 工具包 / 模块归属

| 项目 | 数量 |
|---|---:|
| 7.8.1 包 | 384 |
| 8.0.1 包 | 409 |
| 两版都有 | 375 |
| 模块+工具包都不变 | **367** |
| 仅换工具包 | **6** |
| 换模块 | **2** |
| 删除 | **9** |
| 新增 | **34** |

仅换工具包的 6 个：

| 包 | 7.8.1 | 8.0.1 |
|---|---|---|
| GProp | ModelingData / TKG3d | ModelingData / TKGeomBase |
| GeomLProp | ModelingData / TKG3d | ModelingData / TKGeomBase |
| LProp | ModelingData / TKG2d | ModelingData / TKGeomBase |
| OpenGl | Visualization / TKOpenGles | Visualization / TKOpenGl |
| OpenGlTest | Draw / TKOpenGlesTest | Draw / TKOpenGlTest |
| Precision | FoundationClasses / TKMath | FoundationClasses / TKernel |

换模块的 2 个：`Expr`、`ExprIntrp`（FoundationClasses/TKMath → ModelingAlgorithms/TKExpress）。

删除的 9 个包：

| 包 | 原归属 | 说明 |
|---|---|---|
| Express | DETools/TKExpress | EXPRESS 解析器整体删除 |
| QANCollection | Draw/TKQADraw | QA 集合测试删除（测试迁往 GTests） |
| GeomEvaluator | ModelingData/TKG3d | 被 `GeomEval` 取代 |
| Geom2dEvaluator | ModelingData/TKG2d | 被 `Geom2dEval` 取代 |
| LProp3d | ModelingData/TKG3d | 被 `GeomLProp` 等合并 |
| Geom2dLProp | ModelingData/TKG2d | 合并到 LProp/GeomLProp |
| TColgp / TColGeom / TColGeom2d | TKMath / TKG3d / TKG2d | 改为 `Deprecated/NCollectionAliases` 下的别名头 |

新增的 34 个包（按领域）：

- **拓扑图**：BRepGraph、BRepGraphInc（约 26 万 token）
- **几何求值/包围盒**：GeomEval、Geom2dEval、GeomGridEval、Geom2dGridEval、GeomBndLib、ExtremaPC、GeomHash、Geom2dHash
- **数学**：MathInteg、MathLin、MathOpt、MathPoly、MathRoot、MathSys、MathUtils
- **数据交换**：DEBREP、DEXCAF、DEGLTF、DEIGES、DEOBJ、DEPLY、DESTEP、DESTL、DEVRML、StepTidy、XSMessage
- **其他**：HelixBRep、HelixGeom、SHMessage、Shaders、Textures、GTests

### 2.3 扩展名与可见性迁移

| 扩展名 | 7.8.1 | 8.0.1 | 说明 |
|---|---:|---:|---|
| `.hxx` | 7,827 | 7,084 | 691 个内部头改为 `.pxx` |
| `.cxx` | 5,795 | 6,333 | 新增 GTests 与 `.pxx` 拆分 |
| `.pxx` | 53 | 865 | 私有/模板实现，主要来自 `RWStep*` |
| `.gxx` | 98 | 34 | 模板实现并入 `.lxx`/`.pxx`/`.cxx` |
| `.lxx` | 433 | 388 | 部分并入 `.pxx` |

严格同目录同名的扩展名迁移：`.hxx → .pxx` **691** 个、`.gxx → .hxx` 3 个、`.gxx → .pxx` 2 个。
`RWStep*` 的 Read/Write 工具类几乎全部转为 `.pxx`（53 个 STEP 包共 691 个头），这是 `.hxx` 数量下降的主因，**不是 API 被删除**。

### 2.4 兼容层与资源

- `src/Deprecated/NCollectionAliases/`：**972 个头文件**，把 `TColgp_*`、`TColStd_*`、`TopTools_*` 等旧容器名 typedef 到 `NCollection_*`，并标记 `Standard_DEPRECATED`。约 5.4 万 token。
- `Standard_Handle.hxx` 仍保留 `#define Handle(Class) opencascade::handle<Class>`。
- 资源文件从 `src/*Resource`（StdResource、XRResources、XSTEPResource、XmlOcafResource、DrawResources）迁到顶层 `resources/`。
- `Handle` 宏、`Standard_*` 基本类型 typedef、`D0..DN` 虚函数包装仍保留，源码级兼容性做了较大保留。

---

## 3. 代码变化总量

### 3.1 四种口径

| 口径 | 文件数 | 新增 | 删除 | churn |
|---|---:|---:|---:|---:|
| 原始行 | 18,434 | 1,976,146 | 1,531,007 | **3,507,153** |
| 忽略空白行 | 18,276 | 1,567,511 | 1,122,372 | **2,689,883** |
| Token 级编辑 | 15,133 | 3,454,016 | 1,405,532 | **4,859,548** |
| Token 净变化 | — | 2,239,798 | 191,222 | **2,431,020** |

Token 总量：**11,528,290 → 13,576,866（+2,048,576，+17.8%）**。

### 3.2 文件级分布（忽略空白行）

| 文件处置 | 数量 |
|---|---:|
| 同路径修改 | 11,601（其中源文件 11,588） |
| 重命名/移动 | 58 |
| 新增 | 3,504 |
| 删除 | 3,113 |
| 内容完全不变 | 140 |

Token 归一化后，源文件中有 1,463 个修改文件、165 个新增文件、165 个删除文件的 token 流完全相同 —— 即纯格式/重命名/移动。

Token 级修改文件的严重度分布：

| Token 变化 | 文件数 | 占总 token churn |
|---|---:|---:|
| 0（纯格式/机械重命名） | 164 | 0 |
| 1–19 | 4,722 | 3% |
| 20–99 | 3,442 | 12% |
| 100–499 | 1,570 | 25% |
| 500–1,999 | 304 | 20% |
| ≥2,000 | **88** | **40%** |

### 3.3 分模块（仅"同路径修改文件"，已排除 GTests）

| 模块 | 文件数 | 行 churn（−w） | token churn |
|---|---:|---:|---:|
| ModelingAlgorithms | 3,028 | 625,878 | 401,669 |
| DataExchange | 4,567 | 404,998 | 277,516 |
| FoundationClasses | 799 | 262,918 | 180,321 |
| ModelingData | 917 | 185,073 | 176,610 |
| Visualization | 1,021 | 167,121 | 100,138 |
| Draw | 428 | 122,685 | 165,979 |
| ApplicationFramework | 841 | 79,422 | 46,903 |
| **合计** | **11,601** | **1,848,095** | **1,349,136** |

### 3.4 新增 / 删除代码构成（token）

| 类别 | token |
|---|---:|
| 全部新增文件 | 2,644,883 |
| 全部删除文件 | 864,589 |
| 新增：GTests | 1,213,036 |
| 新增：`_toolkit`（含 972 个别名头） | 54,371 |
| 新增：QABugs 之外的纯新包 | 约 812,000 |
| 删除：Express + ExpToCasExe | 42,098 |
| 删除：QANCollection | 23,854 |
| 删除：旧 evaluator/LProp 包 | 约 33,000 |

**文件重组造成的"假新增/假删除"**（同一包内改名、拆分、合并，总量基本守恒）：

| 包 | 假新增 | 假删除 |
|---|---:|---:|
| AppCont | 351,311 | 351,357 |
| AdvApp2Var | 105,689 | 107,088 |
| IntCurveSurface | 24,607 | 21,666 |
| RWStep* 若干包 | 约 82,000 | 约 78,000 |
| Bnd | 10,602 | 10,511 |

典型例子：`AdvApp2Var_Data_0..7.cxx` 合并重命名为 `AdvApp2Var_Data_Main.pxx`、`_Mlgdrtl.pxx`、`_Mmapgs*.pxx`；`IntPatch_ImpImpIntersection_0..6.gxx` 并入 `IntPatch_ImpImpIntersection.cxx`（单文件 +45,302 token）。

### 3.5 修改量最大的单文件（token churn）

| Token | 文件 | 说明 |
|---:|---|---|
| 45,345 | IntPatch/IntPatch_ImpImpIntersection.cxx | 6 个 `.gxx` 模板并入 |
| 39,986 | QABugs/QABugs_20.cxx | 测试迁往 GTests |
| 36,270 | QABugs/QABugs_19.cxx | 同上 |
| 21,806 | RWStepAP214/RWStepAP214_ReadWriteModule.cxx | STEP 实体表重写 |
| 21,784 | OpenGl/OpenGl_View.cxx | 渲染管线 |
| 21,016 | QABugs/QABugs_11.cxx | 测试迁移 |
| 16,412 | IntWalk/IntWalk_IWalking.gxx | 模板重写 |
| 12,546 | RWGltf/RWGltf_CafWriter.cxx | glTF |
| 11,483 | BOPDS/BOPDS_DS.cxx | 布尔 |
| 10,675 | RWGltf/RWGltf_GltfJsonParser.cxx | glTF |

前 25 个文件占修改文件 token churn 的 26%，前 100 个占 41%。

---

## 4. 机械变化 vs 语义变化

| 变化类型 | 规模 | 是否影响升级 |
|---|---:|---|
| 目录布局迁移 | 全部文件 | 只影响构建脚本/包含路径 |
| clang-format 全库格式化 | 约 3,100 个文件 token 完全不变 | 不影响 |
| `.hxx → .pxx` | 691 个 | 只影响直接 include 内部头的代码 |
| `Handle → occ::handle`、`Standard_* → std` | 全库 | 源码兼容宏仍在；影响模板/重载解析的少数场景 |
| 容器别名迁移 `TCol* → NCollection_*` | 972 个兼容头 | 旧名可用但被标记 deprecated |
| 真实逻辑修改 | 约 1.35M tokens 编辑量；净增 0.87M tokens | 需要逐包评审 |
| 真正新增功能 | BRepGraph 261k、Geom*Eval 约 200k、Math* 约 80k、DE* 约 50k | 可按需采用 |
| 真正删除 | Express/ExpToCas、QANCollection、旧 evaluator 约 100k tokens | 使用 EXPRESS 解析器的项目受影响 |

---

## 5. 对升级工作的含义

- **不是"重写级"升级。** 逻辑包归属 367/384 不变，公共类保留 6,065/6,216（97.6%）。
- **主要工作量在编译适配**，而不是业务代码重写：目录/包含路径、`.pxx` 私有化、`Handle`/`Standard_*`/容器别名、`StepType()` 返回 `string_view`、`NCollection_Array1::Assign` 语义、`Bnd_Box::Center()` 返回 `optional` 等。
- **需要重点回归的功能区**：STEP（实体表重写 + 去重）、BRepMesh（插件注册表）、布尔/相交（BVH）、OpenGL 渲染管线、glTF。
- **测试代码是最大增量**：GTests 进入 `src/` 带来 121 万 token，占净增 59%；这部分不进入产品库。

---

## 6. 复现

```bash
cd /d/workspace/occt-src

# 1. 归一化目录（硬链接，不占空间）
python build_normalized.py

# 2. 三种行级 diff
git diff --no-index --numstat normalized/781 normalized/801 > diff-numstat.txt
git diff --no-index -w --numstat normalized/781 normalized/801 > diff-numstat-w.txt

# 3. token 流（去注释/空白/机械重命名）
python normalize_tokens.py
git diff --no-index -w --numstat tok2/781 tok2/801 > diff-numstat-tok2.txt

# 4. 统计
python deep_analysis2.py      # 结构、扩展名迁移、分模块 churn
python pair_ext.py            # 扩展名迁移配对
python token_multiset.py      # token 多重集净变化
python final_stats.py         # 文件级分布、新增/删除构成
```

关键中间文件：`diff-numstat.txt`、`diff-numstat-w.txt`、`diff-numstat-tok2.txt`、`diff-by-package.csv`。
