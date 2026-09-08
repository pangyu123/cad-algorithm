# AgentMemoryManager 本地 Rerank 增量包

这是发给已有 AgentMemoryManager 工程同事的增量包，不是完整工程备份。

## 内容

- 最新 `app/` 源码：本地 Cross-Encoder、fusion+rerank 秩融合、模型失败回退。
- `models/mmarco-mMiniLMv2-L12-H384-v1/`：本地 rerank 模型。
- `pyproject.toml`、`.env.example`、`Dockerfile`、`README.md`。

真实 `.env`、数据库、评测数据、虚拟环境、缓存和原有 E5 模型均未包含。

## 恢复到一个空目录

下载本目录全部文件后执行：

```powershell
.\restore_project_parts.ps1 -PackageDirectory . -Destination D:\temp\AgentMemoryManager-rerank-delta
```

恢复脚本会校验 8 个 ZIP，拼接模型权重，并校验全部源文件。恢复后，将其中内容覆盖到
同事已有工程根目录；覆盖前建议先备份或提交同事自己的改动。

## 启用配置

不要复制真实 `.env`。请在同事已有 `.env` 中加入或修改：

```env
SEARCH_RANKING_MODE=fusion
SEARCH_RERANK_PROVIDER=local
SEARCH_RERANK_CANDIDATE_LIMIT=15
SEARCH_LOCAL_RERANK_WEIGHT=0.25
SEARCH_LOCAL_RERANK_PATH=models/mmarco-mMiniLMv2-L12-H384-v1
SEARCH_LOCAL_RERANK_BATCH_SIZE=8
SEARCH_LOCAL_RERANK_THREADS=4
SEARCH_LOCAL_RERANK_MAX_LENGTH=512
```

安装本地模型依赖：

```powershell
.\.venv\Scripts\python.exe -m pip install -e ".[local-embedding]"
```

然后运行：

```powershell
.\.venv\Scripts\python.exe -m pytest -q
```
