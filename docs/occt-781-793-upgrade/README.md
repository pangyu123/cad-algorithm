# OCCT 7.8.1 → 7.9.3 升级分析：复现材料

本目录是 [OCCT-7.8.1-to-7.9.3-升级需求设计文档.md](../OCCT-7.8.1-to-7.9.3-升级需求设计文档.md) 的配套材料，用于复现文档中的全部统计数字。

## 目录结构

```
occt-781-793-upgrade/
├── scripts/                 分析脚本
│   ├── build_normalized.py      7.8.1 包级归一化视图（硬链接）
│   ├── build_normalized_793.py  7.9.3 包级归一化视图（硬链接）
│   ├── normalize_tokens.py      生成去注释/空白/机械重命名的 token 流
│   ├── deep_analysis2.py        结构、扩展名迁移、分模块 churn
│   ├── pair_ext.py              同 stem 扩展名迁移配对
│   ├── token_multiset.py        token 多重集净变化
│   └── final_stats.py           文件级分布、新增/删除构成
├── data/
│   ├── diff-numstat-tok2-781-793.txt  token 级逐文件增删行数
│   ├── diff-by-package.csv            按包汇总的变更量
│   └── commits-793-all.json           7.9.0–7.9.3 的 400 条提交记录
└── reference/
    ├── UPGRADE-PATHS.md   7.8.1 → 7.9.3 → 8.0.1 升级路径与兼容矩阵
    └── REPORT.md          7.8.1 → 8.0.1 结构与变化总量分析（8.x 背景）
```

## 前置条件

- 官方源码 tarball：`OCCT-7.8.1.tar.gz`、`OCCT-7.9.3.tar.gz`
- Python 3.9+
- 脚本中的路径常量 `BASE = r'D:/workspace/occt-src'` 需按实际解压位置修改。

## 复现步骤

```bash
# 1. 包级归一化（7.8.1 与 7.9.3 布局相同，均为 src/<Package>/）
python scripts/build_normalized.py
python scripts/build_normalized_793.py

# 2. token 流
python scripts/normalize_tokens.py

# 3. 逐个文件的行级 / token 级 diff
git diff --no-index --numstat    normalized/781 normalized/793 > diff-numstat-781-793.txt
git diff --no-index -w --numstat normalized/781 normalized/793 > diff-numstat-w-781-793.txt
git diff --no-index -w --numstat tok2/781 tok2/793 > diff-numstat-tok2-781-793.txt

# 4. 统计
python scripts/deep_analysis2.py
python scripts/pair_ext.py
python scripts/token_multiset.py
python scripts/final_stats.py
```

> Windows 控制台输出中文/特殊字符时，设置 `PYTHONIOENCODING=utf-8`。

## 度量口径

| 口径 | 做法 | 用途 |
|---|---|---|
| 原始行 churn | `git diff --numstat` | "看起来改了多少" |
| 忽略空白 | 加 `-w` | 去掉缩进噪声 |
| Token 编辑 churn | 去注释/空白，统一 `Handle`、`Standard_*` 等机械重命名后逐 token diff | 真实编辑量 |
| Token 多重集净变化 | 按包统计频次差 `Σ\|count_new − count_old\|` | 净增删；移动/改名/合并全部抵消 |

归一化会抹平：`occ::handle<T>` / `Handle_T` → `Handle(T)`；`Standard_Integer/Real/Boolean/...` 统一；`Standard_TRUE/FALSE` 统一；`Standard_EXPORT/HIDDEN` 删除；注释与空白。

## 关键结论

- 语义净变化 **+29,542 token（+0.26%）**，净 churn 64,918（0.56%）。
- 真实新增 98 个文件、删除 66 个文件；`.hxx → .pxx` 迁移 689 个。
- 编译级必改点 4 处，行为级必测点 3 处，预估工作量 6.5–11.5 人日。

详细分析见上层目录的需求设计文档。
