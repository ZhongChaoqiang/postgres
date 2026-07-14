# PREDICT 功能设计文档

## 1. 概述

### 1.1 功能简介

PREDICT 列属性用于标记需要自动预测的列。当定义一个 PREDICT 列时，系统会自动创建隐藏的伴随列来存储预测值和实际值，并通过触发器自动调用预测表达式。

### 1.2 设计目标

- 提供灵活的数据预测机制，支持用户自定义预测算法
- 支持即时预测（immediate）和延迟预测（deferred）两种模式
- 允许用户直接写入 PREDICT 列值（与 GENERATED 列不同）
- 支持 VOLATILE 函数（如 LLM 推理）
- 保持与现有 PostgreSQL 架构的兼容性

### 1.3 语法

```sql
column_name data_type PREDICT AS (expression) STORED
```

### 1.4 与 GENERATED ALWAYS AS 的关键区别

| 特性 | GENERATED ALWAYS AS STORED | PREDICT AS STORED |
|------|---------------------------|-------------------|
| attgenerated 值 | 's' | 'p' |
| 允许直接写入 | 否 | 是 |
| 表达式必须 IMMUTABLE | 是 | 否（允许VOLATILE） |
| 伴随列 | 无 | _predict, _actual |
| 计算方式 | ExecComputeStoredGenerated | predict_trigger |
| 支持延迟计算 | 否 | 是（predict_timing=deferred） |
| 支持异步后台计算 | 否 | 是（async_predict worker） |

## 2. 架构设计

### 2.1 系统架构图

```mermaid
graph TB
    A[SQL语句] --> B[语法解析器]
    
    subgraph "DDL流程"
        B --> C[CREATE TABLE语句]
        C --> D[列定义处理]
        D --> E{是否PREDICT列?}
        E -->|是| E1[设置attpredict标志]
        E1 --> E2[创建_predict隐藏列]
        E2 --> E3[创建_actual隐藏列]
        E3 --> F[创建预测触发器]
        E -->|否| F
    end
    
    subgraph "INSERT/UPDATE流程"
        G[INSERT/UPDATE语句] --> H[预测触发器]
        H --> I[检查PREDICT列]
        I --> I1[同步值到_actual列]
        I1 --> I2{PREDICT列为NULL且timing=immediate?}
        I2 -->|是| J[获取预测表达式]
        J --> K[调用预测表达式]
        K --> L[保存结果到_predict列]
        L --> M[返回修改后的元组]
        I2 -->|否| M
    end
```

### 2.2 核心组件

#### 2.2.1 PREDICT 列属性

- 在 `pg_attribute` 系统表中添加 `attpredict` 布尔字段
- 标记该列需要预测处理
- `attgenerated = 'p'`（ATTRIBUTE_GENERATED_PREDICT）

#### 2.2.2 自动创建伴随列

当定义一个 PREDICT 列时，系统自动创建两个隐藏列：

| 列名 | 类型 | 用途 |
|------|------|------|
| `{column}_predict` | 与原列相同 | 存储预测表达式的计算结果 |
| `{column}_actual` | 与原列相同 | 存储用户输入的实际值 |

同时自动创建复合 B-tree 索引：`{table}_{column}_predict_idx`

#### 2.2.3 隐藏列机制

- 在 `pg_attribute` 系统表中添加 `atthidden` 布尔字段
- 隐藏列在 `SELECT *` 展开时被跳过
- 显式指定列名时仍可正常查询
- 利用现有的 `p_dontexpand` 机制实现（与 CTE 的 SEARCH/CYCLE 列相同）

#### 2.2.4 预测触发器

- 自动创建的 BEFORE INSERT OR UPDATE 触发器
- 处理所有 PREDICT 列的预测逻辑
- 触发器名称：`jolix_predict_{column}_{oid}`

#### 2.2.5 预测表达式机制

- 通过 `PREDICT AS (expr) STORED` 语法在列定义中内联指定
- 表达式存储在 `pg_attrdef` 系统目录中
- 支持自定义预测算法，可引用同表其他列
- 支持 VOLATILE 函数（如 LLM 推理）

## 3. 数据模型设计

### 3.1 系统表扩展

```c
// pg_attribute 表扩展
bool        attpredict BKI_DEFAULT(f);  // PREDICT列标记
bool        atthidden BKI_DEFAULT(f);   // 隐藏列标记

// attgenerated 新值
#define ATTRIBUTE_GENERATED_PREDICT 'p'
```

### 3.2 解析器扩展

```c
// ColumnDef 结构体扩展
typedef struct ColumnDef
{
    bool        is_predict;     // PREDICT选项指定
    bool        is_hidden;      // 隐藏列标记
} ColumnDef;
```

### 3.3 TupleConstr 扩展

```c
typedef struct TupleConstr
{
    bool        has_generated_predict;  // 包含predict列
} TupleConstr;
```

## 4. 触发器行为设计

### 4.1 predict_trigger 执行流程

```
INSERT/UPDATE → BEFORE触发器 → predict_trigger
    │
    ├── 遍历所有 PREDICT 列
    │
    ├── INSERT 时：
    │   ├── PREDICT列为NULL + immediate → 计算表达式 → 保存到predict列和_predict列
    │   ├── PREDICT列非NULL → 保留用户值 → 保存到_actual列
    │   └── PREDICT列为NULL + deferred → 不计算，等待SELECT时按需推理或async worker
    │
    └── UPDATE 时：
        ├── PREDICT列被修改 → 保存新值到_actual列
        └── PREDICT列未被修改 + immediate → 重新计算表达式
```

### 4.2 deferred 模式下的 SELECT INFER 按需推理流程

```
SELECT [INFER] → ExecScanExtended → ExecPredictOnDemand(rel, slot, infer_predict)
    │
    ├── 检查 infer_predict 标志（来自 SELECT INFER 关键字）
    │   └── 如果未指定 INFER（默认）→ 跳过推理，直接返回
    │
    ├── 检查关系是否有 deferred predict 列
    │
    ├── 检查 predict_timing == DEFERRED
    │
    ├── 检查当前元组的 predict 列是否为 NULL
    │
    ├── 如果 predict 列为 NULL 且 infer_predict == true：
    │   ├── 获取 PREDICT AS 表达式
    │   ├── 执行表达式计算推理值
    │   ├── 修改 slot 中的 predict 列和 _predict 列值
    │   └── 通过 SPI 执行 UPDATE 持久化推理结果
    │
    └── 如果 predict 列非 NULL：
        └── 直接返回（无需推理）
```

**推理执行时机**：推理在 WHERE 条件检查之后、投影之前执行，确保 `SELECT INFER ... WHERE sentiment IS NULL` 能正确找到未推理的行。

### 4.3 表达式计算方式

在触发器中使用 `build_column_default` + `ExecPrepareExpr` + `ExecEvalExpr` 的方式计算内联表达式，而不是通过 SPI 执行 SQL 查询。这避免了 BEFORE INSERT 触发器中 ctid 无效的问题。

## 5. predict_timing 表选项

### 5.1 两种模式

| 模式 | 值 | 触发时机 | 处理方式 |
|------|-----|---------|---------|
| **immediate** | 立即预测 | INSERT/UPDATE 时 | 触发器实时调用预测表达式 |
| **deferred** | 延迟预测（默认） | SELECT INFER 时按需推理 + 后台异步处理 | 默认 SELECT 跳过推理，SELECT INFER 触发推理并持久化，后台 Worker 批量处理 |

### 5.2 deferred 模式的按需推理机制

当 `predict_timing = deferred` 时，系统采用双重推理策略：

1. **按需推理（On-Demand）**：当用户执行 `SELECT INFER` 查询访问到 predict 列为 NULL 的行时，立即执行推理表达式，将结果返回给查询并持久化到表中。默认 `SELECT`（不带 INFER）跳过推理，直接返回当前值
2. **后台推理（Background Worker）**：异步预测 Worker 定期扫描表，处理 predict 列为 NULL 的行

按需推理的优势：
- 用户通过 `SELECT INFER` 显式控制是否触发推理，避免意外触发耗时操作
- 默认 `SELECT` 快速返回当前数据状态，适合查看未推理行数等场景
- 推理结果自动持久化，后续查询无需重复推理
- 与后台 Worker 互补，确保所有行最终都被处理

### 5.3 数据结构

```c
typedef enum StdRdOptPredictTiming
{
    STDRD_OPTION_PREDICT_TIMING_DEFERRED = 0,
    STDRD_OPTION_PREDICT_TIMING_IMMEDIATE,
} StdRdOptPredictTiming;

typedef struct StdRdOptions
{
    StdRdOptPredictTiming predict_timing;
} StdRdOptions;
```

## 6. 异步预测模块

### 6.1 概述

异步预测模块（Async Predict Worker）是一个后台工作进程，用于异步处理 `predict_timing = deferred` 的 PREDICT 列预测任务。与按需推理机制互补，确保所有 predict 列为 NULL 的行最终都被处理。

### 6.2 配置参数

| 参数名 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `async_predict_workers` | int | 2 | 工作进程数量 |
| `async_predict_naptime` | int | 60 | 扫描间隔时间（秒） |
| `async_predict_batch_size` | int | 100 | 每次处理的最大行数 |
| `async_predict_enabled` | bool | true | 是否启用异步预测 |

### 6.3 工作流程

1. 后台工作进程定期扫描表
2. 查找 PREDICT 列为 NULL 的行
3. 通过 SPI 执行预测表达式
4. 通过 SPI 执行 UPDATE 更新 PREDICT 列和 `_predict` 列

### 6.4 注意事项

- Worker 是独立后台进程，GUC 参数必须使用 `ALTER SYSTEM SET` 设置为全局级别
- Worker 执行 UPDATE 时会触发 `predict_trigger`，触发器通过比较新旧值来检测 worker 操作
- Worker 使用 PG_TRY/PG_CATCH 捕获错误，防止 LLM 调用失败导致 worker 崩溃

## 7. 代码修改文件清单

| 文件路径 | 功能说明 |
|---------|----------|
| `src/backend/parser/gram.y` | PREDICT关键字语法解析 |
| `src/backend/parser/parse_utilcmd.c` | 建表时列定义处理，自动创建伴随列 |
| `src/backend/parser/parse_target.c` | INSERT时目标列处理 |
| `src/backend/parser/parse_relation.c` | SELECT时隐藏列的p_dontexpand设置 |
| `src/backend/commands/tablecmds.c` | 建表逻辑，创建预测触发器 |
| `src/backend/catalog/heap.c` | 系统表插入，跳过IMMUTABLE检查 |
| `src/backend/executor/nodeModifyTable.c` | 跳过predict列的ExecComputeStoredGenerated |
| `src/backend/rewrite/rewriteHandler.c` | 允许predict列接受INSERT/UPDATE的用户值 |
| `src/backend/utils/adt/predict.c` | 预测触发器函数实现，ExecPredictOnDemand按需推理 |
| `src/backend/postmaster/async_predict.c` | 异步预测后台工作进程 |
| `src/backend/access/common/reloptions.c` | predict_timing表选项定义（async/immediate） |
| `src/include/catalog/pg_attribute.h` | attpredict和atthidden字段定义 |
| `src/include/nodes/parsenodes.h` | ColumnDef.is_predict和is_hidden字段定义 |
| `src/include/utils/rel.h` | StdRdOptions.predict_timing字段定义 |
| `src/include/utils/predict.h` | ExecPredictOnDemand函数声明 |
| `src/include/executor/execScan.h` | ExecScanExtended中调用ExecPredictOnDemand |
| `contrib/jolix_predict/` | jolix_predict扩展（LLM推理函数） |

## 8. jolix_predict 扩展

### 8.1 功能

jolix_predict 扩展提供大语言模型（LLM）推理功能，允许用户在 SQL 中直接调用 LLM API。

### 8.2 核心函数

| 函数 | 说明 |
|------|------|
| `llm_infer(prompt, question)` | LLM推理函数 |
| `llm_predict(prompt, question)` | LLM预测函数（带历史对话） |
| `llm_rag_infer(prompt, question, table, column)` | RAG推理函数 |
| `llm_rag_predict(prompt, question, table, column)` | RAG预测函数 |

### 8.3 GUC 参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `jolix_predict.llm_api_url` | 空 | LLM API 地址 |
| `jolix_predict.llm_api_key` | 空 | LLM API 密钥 |
| `jolix_predict.llm_model` | gpt-3.5-turbo | LLM 模型名称 |
| `jolix_predict.llm_history_table` | default | 默认历史记录表名，空字符串禁用自动记录 |

### 8.4 参数默认值回退机制

当 `llm_infer` 和 `llm_rag_infer` 的 `system_prompt` 或 `user_input` 参数为 NULL 或空字符串时，自动使用 `set_llm_config()` 配置的默认值：

| 参数 | 回退配置 | 说明 |
|------|----------|------|
| `system_prompt` | `set_llm_config(p_system_prompt := '...')` | 系统提示词默认值 |
| `user_input` | `set_llm_config(p_prompt_template := '...')` | 用户输入默认值 |

### 8.5 RAG 上下文自动包含 PREDICT 列的隐藏伴生列

#### 8.5.1 设计目标

当 `llm_rag_infer` 通过 EMBEDDING 列检索相似记录并构建 RAG 上下文时，会自动将所有 PREDICT 列的隐藏伴生列（`{col}_actual`、`{col}_predict`）一并包含在上下文中，并在头部添加说明文字，告知 LLM 这些字段的含义：

- 以 `_actual` 结尾的字段表示用户真实输入
- 以 `_predict` 结尾的字段表示 LLM 推理结果

这样 LLM 既能看到用户原始问题/内容，也能看到历史推理结果，从而生成更准确、更有上下文感知的回答。

#### 8.5.2 实现细节

`do_rag_retrieval()` 函数（位于 `contrib/jolix_predict/jolix_predict.c`）的执行流程：

1. **发现 PREDICT 列**：通过 `relation_open` 打开表，遍历 `pg_attribute`，统计 `attpredict = true` 的列。
2. **定位隐藏伴生列**：对每个 PREDICT 列 `{col}`，通过 `get_attnum()` 查找 `{col}_actual` 和 `{col}_predict` 两个隐藏列的 attnum。
3. **显式查询隐藏列**：由于 `SELECT *` 会跳过 `atthidden = true` 的列，因此在 SELECT 语句中显式添加这些隐藏列：
   ```sql
   SELECT *, answer_actual, answer_predict, ...
   FROM rag_table
   ORDER BY embedding_col <=> $1::vector
   LIMIT rag_topn
   ```
4. **构建上下文文本**：遍历 SPI 查询结果，将每列的 `列名: 值` 格式追加到上下文。若检测到存在伴生列，在上下文头部添加说明：
   ```
   Retrieved context:
   Note: Fields ending in "_actual" represent the user's actual input.
         Fields ending in "_predict" represent the LLM's inferred value.

   --- Result 1 ---
   id: 1
   content: PostgreSQL is an advanced open source database.
   answer: PostgreSQL is an open source database.
   answer_actual: PostgreSQL is an open source database.
   answer_predict: PostgreSQL is a powerful RDBMS.
   ...
   ```
5. **过滤规则**：
   - 跳过 `attisdropped` 的列
   - 跳过 `attembedding` 的列（EMBEDDING 列本身）
   - 跳过名为 `_embedding`、`_predict`、`_actual` 的列（避免与伴生列混淆）
   - 跳过值为 NULL 的列（未推理的 `_predict` 列不会出现在上下文中）

#### 8.5.3 兼容性

- 若表中没有 PREDICT 列，或 PREDICT 列没有对应的隐藏伴生列，则跳过说明头部，行为与旧版本完全一致。
- 若某些 `_predict` 列为 NULL（尚未异步推理完成），该字段不会出现在上下文中，不影响其他字段。
- 内存管理：所有动态分配的列名数组在函数返回前通过 `pfree` 释放，避免内存泄漏。

#### 8.5.4 示例

```sql
CREATE TABLE rag_knowledge (
    id serial PRIMARY KEY,
    content text EMBEDDING AS (st_embedding(content)),
    answer text PREDICT AS (llm_rag_infer(
        'Answer questions based on the provided context. Reply in one short sentence.',
        content
    ))
) WITH (predict_timing = immediate, vector_len = 384);
```

当插入新问题触发 `llm_rag_infer` 时，`do_rag_retrieval` 会：
1. 用 `st_embedding` 计算问题的向量
2. 检索最相似的 Top-N 记录
3. 构建包含 `answer_actual`、`answer_predict` 的上下文
4. 添加说明头部
5. 将完整上下文与问题一起发送给 LLM

---
**文档版本**: 1.2  
**最后更新**: 2026-07-08
