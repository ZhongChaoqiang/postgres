# Limix 推理函数设计文档

## 1. 概述

### 1.1 功能简介

`limix_infer` 是一个**本地推理函数**，基于向量相似度（k-NN）检索历史样本数据进行推理。它利用当前表的 EMBEDDINGS 向量列进行相似度搜索，检索出与当前行最相关的历史数据行，然后根据相似行的 PREDICT 列值直接推理出当前行的预测结果。**无需调用外部 LLM API**，推理完全在本地完成。

### 1.2 设计目标

- **零参数 API**：函数无需任何参数，所有配置通过表级 WITH 参数设置
- **本地推理**：无需外部大模型，基于向量相似度的 k-NN 算法在本地完成推理
- **全自动推理**：自动检测 EMBEDDINGS 列、PREDICT 列、搜索向量
- 利用表中已有的 EMBEDDINGS 向量索引进行高效相似度检索
- 与现有 PREDICT 列机制无缝集成，作为 PREDICT AS 表达式使用
- 零网络延迟，推理速度快

### 1.3 与现有函数的关系

| 函数 | 推理方式 | 是否需要外部 API | 适用场景 |
|------|---------|----------------|---------|
| `llm_infer` | 外部 LLM API | 是 | 通用 LLM 推理 |
| `llm_rag_infer` | 外部 LLM API + RAG | 是 | 文档问答、知识检索 |
| **`limix_infer`** | **本地 k-NN 推理** | **否** | **基于历史数据的分类/预测** |

**核心区别**：
- `llm_infer` / `llm_rag_infer`：需要外部 LLM API，有网络延迟和成本
- `limix_infer`：纯本地推理，零延迟，零成本，基于向量相似度直接推理

### 1.4 推理算法

`limix_infer` 采用基于向量相似度的 k-NN（k-Nearest Neighbors）推理算法：

| limix_task | 推理算法 | 说明 |
|-----------|---------|------|
| `classification` | 加权多数投票 | 距离越近的邻居权重越大，取票数最多的类别 |
| `regression` | 加权平均 | 距离越近的邻居权重越大，取加权平均值 |
| `anomaly` | 距离阈值判定 | 与最近邻的平均距离超过阈值则判定为异常 |
| `extraction` | 最近邻复制 | 取最相似行的 PREDICT 值 |

### 1.5 典型应用场景

- **客户分群**：根据已有客户的特征和分群结果，自动对新客户分群
- **文本分类**：根据已有文本的分类结果，自动对新文本分类
- **情感分析**：根据已有评论的情感标注，自动分析新评论情感
- **优先级判定**：根据已有工单的优先级，自动判定新工单优先级
- **异常检测**：根据已有数据的标注结果，自动检测新数据是否异常
- **数值预测**：根据已有数据的数值，预测新数据的数值

## 2. 函数签名设计

### 2.1 设计原则

**零参数 API**：`limix_infer()` 无需任何参数，一切自动推断：

- **推理算法**：根据 `limix_task` 自动选择 k-NN 变体
- **搜索向量**：自动取表中第一个 EMBEDDINGS 列的值
- **PREDICT 列名**：自动检测当前表的 PREDICT 列
- **模型名**：通过 WITH 参数 `limix_model` 设置，默认 `'limix-2m'`
- **任务类别**：通过 WITH 参数 `limix_task` 设置，默认 `'classification'`
- **检索数量**：通过 WITH 参数 `limix_topn` 设置，默认 `5`

### 2.2 函数签名

```sql
limix_infer() RETURNS text
```

### 2.3 表级 WITH 参数

所有 limix 配置通过 CREATE TABLE 的 WITH 子句设置：

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `limix_model` | text | `'limix-2m'` | 本地推理模型名称（预留，当前版本使用 k-NN） |
| `limix_task` | text | `'classification'` | 任务类别：`classification`、`regression`、`extraction`、`anomaly` |
| `limix_topn` | integer | `5` | 检索的相似行数量（k-NN 的 k 值） |

### 2.4 自动推断机制

| 推断项 | 推断方式 | 说明 |
|--------|---------|------|
| 当前表名 | GUC: `jolix_predict.current_table` | 由 PREDICT 触发器自动设置 |
| EMBEDDINGS 列名 | 查询 `pg_attribute` 中 `attembeddings=true` 的列 | 取第一个 EMBEDDINGS 列 |
| PREDICT 列名 | 查询 `pg_attribute` 中 `attpredict=true` 的列 | 取第一个 PREDICT 列 |
| 搜索向量 | 从当前行 EMBEDDINGS 列取值 | 触发器中向量已计算 |
| 推理算法 | 根据 `limix_task` 自动选择 | 见 1.4 节 |

### 2.5 limix_task 与推理算法

| limix_task 值 | 推理算法 | 输出格式 | 说明 |
|---------------|---------|---------|------|
| `classification` | 加权多数投票 | 类别名称文本 | 距离越近权重越大，取票数最多的类别 |
| `regression` | 加权平均 | 数值文本 | 距离越近权重越大，取加权平均值 |
| `extraction` | 最近邻复制 | 最近邻的 PREDICT 值 | 直接取最相似行的预测结果 |
| `anomaly` | 距离阈值判定 | `normal` 或 `anomaly` | 平均距离超过阈值则异常 |

当 `limix_task` 未设置（NULL 或空字符串）时，默认使用 `classification`。

### 2.6 使用示例

```sql
-- 1. 创建带 EMBEDDINGS + PREDICT 的表（最简用法）
CREATE TABLE customer_segments (
    id serial PRIMARY KEY,
    age int,
    income float,
    category text,
    demographic EMBEDDINGS AS (ft_transformer_embedding(age, income, category)),
    segment text PREDICT AS (limix_infer())
) WITH (predict_timing = immediate, vector_len = 384);

-- 2. 先插入一些有 segment 值的历史数据（作为样本）
INSERT INTO customer_segments (age, income, category, segment) VALUES
    (30, 50000, 'A', 'Premium'),
    (25, 30000, 'B', 'Standard'),
    (40, 75000, 'C', 'VIP'),
    (35, 55000, 'A', 'Premium'),
    (22, 28000, 'B', 'Basic');

-- 3. 插入新数据（不提供 segment），自动推理
INSERT INTO customer_segments (age, income, category) VALUES (32, 48000, 'A');
-- segment 列自动推理为 'Premium'（基于 k-NN 加权投票）
```

### 2.7 更多场景示例

```sql
-- 异常检测（指定 limix_task）
CREATE TABLE anomaly_detection (
    id serial PRIMARY KEY,
    metric_name text,
    value float,
    metric_vec EMBEDDINGS AS (ft_transformer_embedding(metric_name, value)),
    is_anomaly text PREDICT AS (limix_infer())
) WITH (predict_timing = immediate, vector_len = 384, limix_task = 'anomaly');

-- 回归预测
CREATE TABLE price_predictions (
    id serial PRIMARY KEY,
    area float,
    rooms int,
    location text,
    feature_vec EMBEDDINGS AS (ft_transformer_embedding(area, rooms, location)),
    price float PREDICT AS (limix_infer()::float)
) WITH (predict_timing = immediate, vector_len = 384, limix_task = 'regression');

-- 优先级判定（自定义 topn）
CREATE TABLE ticket_priorities (
    id serial PRIMARY KEY,
    product text,
    severity text,
    description text,
    ticket_vec EMBEDDINGS AS (ft_transformer_embedding(product, severity, description)),
    priority text PREDICT AS (limix_infer())
) WITH (predict_timing = immediate, vector_len = 384, limix_topn = 10);
```

## 3. 架构设计

### 3.1 系统架构图

```
INSERT/UPDATE 语句
    │
    ▼
BEFORE 触发器 (predict_trigger)
    │
    ├── 1. 计算 EMBEDDINGS 列（向量值已就绪）
    │
    └── 2. 计算 PREDICT 列
            │
            ▼
        limix_infer() 函数调用（零参数，本地推理）
            │
            ├── a. 获取当前表名（GUC: jolix_predict.current_table）
            │
            ├── b. 自动检测 EMBEDDINGS 列名（pg_attribute, attembeddings=true）
            │
            ├── c. 自动检测 PREDICT 列名（pg_attribute, attpredict=true）
            │
            ├── d. 从当前行 EMBEDDINGS 列获取搜索向量
            │
            ├── e. 读取 WITH 参数：limix_model, limix_task, limix_topn
            │
            ├── f. 向量相似度搜索（SPI 查询）
            │   │
            │   │  SELECT predict_col, embeddings_col <=> search_vector AS distance
            │   │  FROM table_name
            │   │  WHERE predict_col IS NOT NULL
            │   │  ORDER BY distance
            │   │  LIMIT limix_topn
            │   │
            │   └── 返回 topn 行相似数据及其距离
            │
            ├── g. 本地 k-NN 推理（根据 limix_task 选择算法）
            │   │
            │   │  classification → 加权多数投票
            │   │  regression     → 加权平均
            │   │  anomaly        → 距离阈值判定
            │   │  extraction     → 最近邻复制
            │   │
            │   └── 返回推理结果
            │
            └── h. 返回推理结果 → 填入 PREDICT 列
```

### 3.2 触发器执行时序

```
INSERT INTO customer_segments (age, income, category) VALUES (32, 48000, 'A');
    │
    ├── Step 1: embeddings_trigger 计算 demographic 向量
    │   └── demographic = ft_transformer_embedding(32, 48000, 'A')
    │       → [0.12, -0.34, ..., 0.56]  (384维向量)
    │
    ├── Step 2: predict_trigger 计算 segment 预测值
    │   │
    │   └── 评估 PREDICT 表达式: limix_infer()
    │       │
    │       ├── 2a. 获取表名: customer_segments (from GUC)
    │       ├── 2b. 自动检测 EMBEDDINGS 列: demographic (attembeddings=true)
    │       ├── 2c. 自动检测 PREDICT 列: segment (attpredict=true)
    │       ├── 2d. 从当前行取搜索向量: demographic = [0.12, -0.34, ...]
    │       ├── 2e. 读取 WITH 参数: limix_task='classification', limix_topn=5
    │       ├── 2f. SPI 查询相似行:
    │       │   SELECT segment, demographic <=> '[0.12,...]' AS distance
    │       │   FROM customer_segments
    │       │   WHERE segment IS NOT NULL
    │       │   ORDER BY distance LIMIT 5
    │       │
    │       │   结果:
    │       │   segment='Premium', distance=0.05
    │       │   segment='Premium', distance=0.08
    │       │   segment='Standard', distance=0.15
    │       │   segment='VIP', distance=0.22
    │       │   segment='Premium', distance=0.25
    │       │
    │       ├── 2g. 加权多数投票（classification）:
    │       │   Premium: 1/0.05 + 1/0.08 + 1/0.25 = 20.0 + 12.5 + 4.0 = 36.5
    │       │   Standard: 1/0.15 = 6.67
    │       │   VIP: 1/0.22 = 4.55
    │       │   → Premium 得票最高
    │       │
    │       └── 2h. 返回 "Premium"
    │
    └── 最终存储: age=32, income=48000, category='A', segment='Premium'
```

### 3.3 与 LLM 推理的对比

```
llm_infer / llm_rag_infer:
    用户编写提示词 → 调用外部 LLM API → 等待响应 → 结果
    └── 需要网络 ──────────────────────┘  └── 有延迟和成本 ──┘

limix_infer:
    当前行向量 → 本地 k-NN 搜索 → 加权推理 → 结果
    └── 纯本地计算，零网络延迟 ──────────────────────┘
```

## 4. 详细设计

### 4.1 EMBEDDINGS 列名自动检测

`find_embeddings_column_name` 同时支持 EMBEDDING 和 EMBEDDINGS 两种语法：

- **EMBEDDING 列**（`col text EMBEDDING AS (func(col))`）：可见列是 text 类型，向量存储在隐藏的 `col_embedding` 列中
- **EMBEDDINGS 列**（`name EMBEDDINGS AS (func(col1, col2))`）：列本身就是 vector 类型

函数先查找标记为 `attembedding` 或 `attembeddings` 的可见列，然后返回对应的隐藏 `_embedding` 向量列名。

```c
static char *
find_embeddings_column_name(Oid relid)
{
    Relation    rel;
    TupleDesc   tupdesc;
    int         attnum;
    char       *colname = NULL;
    char       *visible_colname = NULL;

    rel = relation_open(relid, AccessShareLock);
    tupdesc = RelationGetDescr(rel);

    /* First pass: find the visible EMBEDDING/EMBEDDINGS column */
    for (attnum = 1; attnum <= tupdesc->natts; attnum++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

        if (attr->attisdropped)
            continue;
        if (attr->attembedding || attr->attembeddings)
        {
            visible_colname = pstrdup(NameStr(attr->attname));
            break;
        }
    }

    if (visible_colname != NULL)
    {
        /* Look for the hidden _embedding column that stores the vector */
        char   *hidden_colname = psprintf("%s_embedding", visible_colname);
        AttrNumber hidden_attnum;

        hidden_attnum = get_attnum(relid, hidden_colname);
        if (AttributeNumberIsValid(hidden_attnum))
            colname = hidden_colname;
        else
            pfree(hidden_colname);

        pfree(visible_colname);
    }

    relation_close(rel, AccessShareLock);
    return colname;
}
```

### 4.2 PREDICT 列名自动检测

```c
static char *
find_predict_column_name(Oid relid)
{
    Relation    rel;
    TupleDesc   tupdesc;
    int         attnum;
    char       *colname = NULL;

    rel = relation_open(relid, AccessShareLock);
    tupdesc = RelationGetDescr(rel);

    for (attnum = 1; attnum <= tupdesc->natts; attnum++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

        if (attr->attisdropped)
            continue;
        if (attr->attpredict)
        {
            colname = pstrdup(NameStr(attr->attname));
            break;
        }
    }

    relation_close(rel, AccessShareLock);
    return colname;
}
```

### 4.3 从当前行获取搜索向量

在触发器上下文中，EMBEDDINGS 列的值已在 newtuple 中计算完成。通过进程本地变量传递：

```c
/* 进程本地变量，用于在触发器和 limix_infer 之间传递数据 */
static Datum limix_current_vector = (Datum) 0;
static bool  limix_current_vector_isnull = true;
```

在 predict_trigger 中设置：

```c
/* 在 predict_trigger 中，表达式求值前 */
Datum vector_datum;
bool  isnull;
int   embeddings_attnum;

embeddings_attnum = find_embeddings_attnum(trigger_tuple_desc);

if (embeddings_attnum > 0)
{
    vector_datum = heap_getattr(newtuple, embeddings_attnum,
                                trigger_tuple_desc, &isnull);
    limix_current_vector = isnull ? (Datum) 0 : vector_datum;
    limix_current_vector_isnull = isnull;
}
else
{
    limix_current_vector_isnull = true;
}

/* 执行 PREDICT 表达式求值（调用 limix_infer()） */
predict_value = ExecEvalExprSwitchContext(pred_expr, econtext, &isnull);

/* 清理 */
limix_current_vector_isnull = true;
```

### 4.4 向量相似度搜索

通过 SPI 执行向量相似度搜索，同时获取距离值用于加权推理：

```c
typedef struct LimixNeighbor
{
    char   *predict_value;   /* PREDICT 列的值 */
    double  distance;        /* 与搜索向量的距离 */
} LimixNeighbor;

static LimixNeighbor *
do_limix_similarity_search(Oid table_oid, const char *embeddings_colname,
                           const char *predict_colname,
                           Datum search_vector_datum,
                           int topn, int *num_neighbors)
{
    int             ret;
    char           *relname;
    char           *vector_str;
    StringInfo      query_buf;
    LimixNeighbor  *neighbors;

    vector_str = vector_datum_to_string(search_vector_datum);
    relname = get_rel_name(table_oid);

    ret = SPI_connect();
    if (ret != SPI_OK_CONNECT)
        ereport(ERROR, ...);

    /* 构建相似度搜索查询，同时获取距离值 */
    query_buf = makeStringInfo();
    appendStringInfo(query_buf,
        "SELECT %s, %s <=> $1::vector AS distance "
        "FROM %s "
        "WHERE %s IS NOT NULL "
        "ORDER BY distance LIMIT %d",
        quote_identifier(predict_colname),
        quote_identifier(embeddings_colname),
        quote_identifier(relname),
        quote_identifier(predict_colname),
        topn);

    {
        Oid     argtypes[1] = {TEXTOID};
        Datum   values[1];
        char    nulls[1] = {' '};

        values[0] = CStringGetTextDatum(vector_str);

        ret = SPI_execute_with_args(query_buf->data,
                                    1, argtypes, values, nulls,
                                    true, topn);
    }

    if (ret == SPI_OK_SELECT && SPI_processed > 0)
    {
        int i;
        neighbors = (LimixNeighbor *) palloc(sizeof(LimixNeighbor) * SPI_processed);

        for (i = 0; i < (int) SPI_processed; i++)
        {
            HeapTuple   tuple = SPI_tuptable->vals[i];
            bool        isnull;

            /* 获取 PREDICT 列值 */
            neighbors[i].predict_value = SPI_getvalue(tuple, SPI_tuptable->tupdesc, 1);

            /* 获取距离值 */
            Datum distance_datum = SPI_getbinval(tuple, SPI_tuptable->tupdesc, 2, &isnull);
            if (isnull)
                neighbors[i].distance = DBL_MAX;
            else
                neighbors[i].distance = DatumGetFloat8(distance_datum);
        }

        *num_neighbors = (int) SPI_processed;
    }
    else
    {
        neighbors = NULL;
        *num_neighbors = 0;
    }

    SPI_finish();
    return neighbors;
}
```

### 4.5 分类推理：加权多数投票

```c
static char *
limix_classify(LimixNeighbor *neighbors, int num_neighbors)
{
    /* 使用距离的倒数作为权重，距离越近权重越大 */
    /* 权重 = 1 / (distance + epsilon)，epsilon 防止除零 */

    double  epsilon = 1e-6;
    char  **labels;
    double *weights;
    int     n_labels = 0;
    int     i, j;
    char   *best_label;
    double  best_weight = -1.0;

    labels = (char **) palloc(sizeof(char *) * num_neighbors);
    weights = (double *) palloc0(sizeof(double) * num_neighbors);

    for (i = 0; i < num_neighbors; i++)
    {
        double weight = 1.0 / (neighbors[i].distance + epsilon);
        bool   found = false;

        /* 查找是否已有该标签 */
        for (j = 0; j < n_labels; j++)
        {
            if (strcmp(labels[j], neighbors[i].predict_value) == 0)
            {
                weights[j] += weight;
                found = true;
                break;
            }
        }

        if (!found)
        {
            labels[n_labels] = neighbors[i].predict_value;
            weights[n_labels] = weight;
            n_labels++;
        }
    }

    /* 找出权重最大的标签 */
    best_label = labels[0];
    best_weight = weights[0];

    for (i = 1; i < n_labels; i++)
    {
        if (weights[i] > best_weight)
        {
            best_weight = weights[i];
            best_label = labels[i];
        }
    }

    pfree(labels);
    pfree(weights);

    return best_label;
}
```

### 4.6 回归推理：加权平均

```c
static char *
limix_regress(LimixNeighbor *neighbors, int num_neighbors)
{
    double  epsilon = 1e-6;
    double  weighted_sum = 0.0;
    double  weight_sum = 0.0;
    double  value;
    char   *result;
    int     i;

    for (i = 0; i < num_neighbors; i++)
    {
        double weight = 1.0 / (neighbors[i].distance + epsilon);

        value = atof(neighbors[i].predict_value);
        weighted_sum += weight * value;
        weight_sum += weight;
    }

    if (weight_sum == 0.0)
        return pstrdup("0");

    result = psprintf("%.6g", weighted_sum / weight_sum);
    return result;
}
```

### 4.7 异常检测：距离阈值判定

```c
static char *
limix_anomaly_detect(LimixNeighbor *neighbors, int num_neighbors)
{
    double  avg_distance = 0.0;
    int     i;

    if (num_neighbors == 0)
        return pstrdup("anomaly");

    for (i = 0; i < num_neighbors; i++)
        avg_distance += neighbors[i].distance;

    avg_distance /= num_neighbors;

    /*
     * 使用平均距离判定异常。
     * 阈值策略：使用最近邻距离的中位数 * 1.5 作为阈值。
     * 简化实现：如果平均距离 > 最近邻距离的 3 倍，判定为异常。
     */
    if (avg_distance > neighbors[0].distance * 3.0)
        return pstrdup("anomaly");
    else
        return pstrdup("normal");
}
```

### 4.8 信息提取：最近邻复制

```c
static char *
limix_extract(LimixNeighbor *neighbors, int num_neighbors)
{
    if (num_neighbors == 0)
        return NULL;

    /* 直接返回最近邻的 PREDICT 值 */
    return pstrdup(neighbors[0].predict_value);
}
```

### 4.9 表级 WITH 参数

在 `reloptions.c` 中新增三个表选项：

```c
static relopt_int intRelOpts[] =
{
    /* 现有选项... */
    {
        {
            "limix_topn",
            "Number of similar rows to retrieve for limix_infer function (k-NN k value).",
            RELOPT_KIND_HEAP,
            ShareUpdateExclusiveLock
        },
        5, 1, 100    /* 默认5，最小1，最大100 */
    },
};

static relopt_string stringRelOpts[] =
{
    /* 现有选项... */
    {
        {
            "limix_model",
            "Local inference model name for limix_infer function.",
            RELOPT_KIND_HEAP,
            ShareUpdateExclusiveLock
        },
        "limix-2m",   /* 默认值 */
        true,
    },
    {
        {
            "limix_task",
            "Task type for limix_infer: classification, regression, extraction, anomaly.",
            RELOPT_KIND_HEAP,
            ShareUpdateExclusiveLock
        },
        "classification",  /* 默认值 */
        true,
    },
};
```

在 `StdRdOptions` 中新增字段：

```c
typedef struct StdRdOptions
{
    StdRdOptPredictTiming predict_timing;
    int         vector_len;
    /* ... 现有字段 ... */
    int         limix_topn;         /* limix_infer 检索相似行数量 */
    int         limix_model_offset; /* limix_model 字符串偏移量 */
    int         limix_task_offset;  /* limix_task 字符串偏移量 */
} StdRdOptions;
```

### 4.10 主函数实现框架

```c
PG_FUNCTION_INFO_V1(limix_infer);

Datum
limix_infer(PG_FUNCTION_ARGS)
{
    char           *table_name;
    Oid             table_oid;
    char           *embeddings_colname;
    char           *predict_colname;
    char           *task;
    int             topn;
    Datum           search_vector_datum;
    LimixNeighbor  *neighbors;
    int             num_neighbors;
    char           *result;

    /* 1. 获取当前表名和 OID */
    if (jolix_predict_current_table == NULL ||
        strlen(jolix_predict_current_table) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("limix_infer: cannot determine current table"),
                 errhint("limix_infer must be used in a PREDICT column expression.")));

    table_name = pstrdup(jolix_predict_current_table);
    table_oid = RelnameGetRelid(table_name);

    if (!OidIsValid(table_oid))
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("limix_infer: table \"%s\" not found", table_name)));

    /* 2. 自动检测 EMBEDDINGS 列名 */
    embeddings_colname = find_embeddings_column_name(table_oid);
    if (embeddings_colname == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("table \"%s\" does not have an EMBEDDINGS column",
                        table_name),
                 errhint("limix_infer requires a table with an EMBEDDINGS column.")));

    /* 3. 自动检测 PREDICT 列名 */
    predict_colname = find_predict_column_name(table_oid);
    if (predict_colname == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("table \"%s\" does not have a PREDICT column",
                        table_name)));

    /* 4. 读取 WITH 参数 */
    task = get_limix_task(table_oid);    /* 默认 "classification" */
    topn = get_limix_topn(table_oid);    /* 默认 5 */

    /* 5. 获取搜索向量 */
    if (!limix_current_vector_isnull)
    {
        search_vector_datum = limix_current_vector;
    }
    else
    {
        search_vector_datum = get_search_vector_fallback(table_oid, embeddings_colname);
        if (search_vector_datum == (Datum) 0)
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("limix_infer: could not get search vector")));
    }

    /* 6. 向量相似度搜索 */
    neighbors = do_limix_similarity_search(
        table_oid, embeddings_colname, predict_colname,
        search_vector_datum, topn, &num_neighbors);

    if (num_neighbors == 0)
    {
        ereport(WARNING,
                (errmsg("limix_infer: no historical data found in table \"%s\"",
                        table_name)));
        PG_RETURN_NULL();
    }

    /* 7. 根据 task 选择推理算法 */
    if (strcmp(task, "classification") == 0)
    {
        result = limix_classify(neighbors, num_neighbors);
    }
    else if (strcmp(task, "regression") == 0)
    {
        result = limix_regress(neighbors, num_neighbors);
    }
    else if (strcmp(task, "anomaly") == 0)
    {
        result = limix_anomaly_detect(neighbors, num_neighbors);
    }
    else if (strcmp(task, "extraction") == 0)
    {
        result = limix_extract(neighbors, num_neighbors);
    }
    else
    {
        /* 默认使用分类 */
        result = limix_classify(neighbors, num_neighbors);
    }

    /* 8. 返回结果 */
    if (result == NULL)
        PG_RETURN_NULL();

    PG_RETURN_TEXT_P(cstring_to_text(result));
}
```

## 5. SQL 定义

### 5.1 jolix_predict--1.0.sql 新增

```sql
-- Limix 本地推理函数：基于向量相似度的 k-NN 推理（零参数，无需外部 LLM）
CREATE FUNCTION limix_infer() RETURNS text
AS 'jolix_predict', 'limix_infer'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION limix_infer() IS
'Limix local inference function (zero-parameter, no external LLM required): uses vector similarity search (k-NN) to find similar historical rows in the same table, then infers the prediction based on their PREDICT column values. All configuration is through table WITH parameters: limix_model (default: limix-2m), limix_task (default: classification), limix_topn (default: 5). Task types: classification (weighted majority vote), regression (weighted average), anomaly (distance threshold), extraction (nearest neighbor copy).';
```

## 6. 错误处理

### 6.1 错误场景

| 场景 | 错误码 | 错误信息 | 提示 |
|------|--------|---------|------|
| 无法确定当前表 | OBJECT_NOT_IN_PREREQUISITE_STATE | limix_infer: cannot determine current table | 必须在 PREDICT 列表达式中使用 |
| 表没有 EMBEDDINGS 列 | OBJECT_NOT_IN_PREREQUISITE_STATE | table "X" does not have an EMBEDDINGS column | 需要表有 EMBEDDINGS 列 |
| 表没有 PREDICT 列 | OBJECT_NOT_IN_PREREQUISITE_STATE | table "X" does not have a PREDICT column | 需要表有 PREDICT 列 |
| 无法获取搜索向量 | OBJECT_NOT_IN_PREREQUISITE_STATE | could not get search vector | 检查 EMBEDDINGS 列是否有值 |
| 无历史数据 | — | WARNING + 返回 NULL | 需要先插入有 PREDICT 值的数据 |

### 6.2 无历史数据时的处理

当表中没有已标记的数据（PREDICT 列全为 NULL）时：
1. 向量相似度搜索返回 0 行
2. 发出 WARNING 提示无历史数据
3. 返回 NULL（无法推理）

## 7. 性能考虑

### 7.1 向量索引利用

- 相似度搜索使用 EMBEDDINGS 列的向量索引（HNSW/IVFFlat）
- 查询 `ORDER BY embeddings_col <=> search_vector LIMIT topn` 可利用索引加速
- 无索引时退化为全表扫描，大数据量表建议创建向量索引

### 7.2 本地推理优势

- **零网络延迟**：无需调用外部 API，推理在本地完成
- **零成本**：不消耗 LLM API token
- **高并发**：不受 API 速率限制
- **确定性**：相同输入产生相同输出（分类/回归/最近邻）

### 7.3 SPI 查询优化

- 使用 `LIMIT topn` 限制返回行数
- `WHERE predict_col IS NOT NULL` 过滤未标记数据
- 只读查询（read-only SPI），不产生写开销
- 仅查询 PREDICT 列和距离值，减少数据传输

### 7.4 触发器时序保证

- EMBEDDINGS 列在 PREDICT 列之前计算（触发器内部处理顺序）
- 搜索向量通过进程本地变量从触发器传递给 limix_infer，无需额外查询

## 8. 扩展方向

### 8.1 短期扩展

- **距离权重函数**：支持不同的权重函数（高斯、逆距离、均匀）
- **相似度阈值**：增加 WITH 参数 `limix_similarity_threshold`，过滤过于不相似的邻居
- **指定 EMBEDDINGS 列**：增加 WITH 参数 `limix_embeddings_col`，支持多 EMBEDDINGS 列场景
- **异常检测阈值**：增加 WITH 参数 `limix_anomaly_threshold`，自定义异常判定阈值

### 8.2 长期扩展

- **本地 ML 模型**：`limix_model` 参数支持加载本地 ML 模型（如 ONNX、LightGBM）
- **增量学习**：新数据自动加入训练集，模型持续优化
- **特征重要性**：分析各特征列对推理结果的贡献度
- **置信度输出**：返回推理结果的置信度分数
- **批量推理**：支持一次推理多行，减少查询开销

## 9. 代码修改文件清单

| 文件路径 | 功能说明 |
|---------|----------|
| `contrib/jolix_predict/jolix_predict.c` | 新增 `limix_infer` 函数实现及 k-NN 推理算法 |
| `contrib/jolix_predict/jolix_predict--1.0.sql` | 新增 `limix_infer()` 函数 SQL 定义和注释 |
| `src/backend/access/common/reloptions.c` | 新增 `limix_topn`、`limix_model`、`limix_task` 表选项 |
| `src/include/utils/rel.h` | `StdRdOptions` 新增 limix 相关字段 |
| `src/backend/utils/adt/predict.c` | 触发器中传递 EMBEDDINGS 向量给 limix_infer |
| `doc/predict/limix_infer_design.md` | 本设计文档 |

## 10. 测试用例

### 10.1 基本功能测试（分类）

```sql
-- 创建测试表（零参数用法）
CREATE TABLE test_limix_segments (
    id serial PRIMARY KEY,
    age int,
    income float,
    category text,
    demographic EMBEDDINGS AS (ft_transformer_embedding(age, income, category)),
    segment text PREDICT AS (limix_infer())
) WITH (predict_timing = immediate, vector_len = 384);

-- 插入样本数据
INSERT INTO test_limix_segments (age, income, category, segment) VALUES
    (30, 50000, 'A', 'Premium'),
    (25, 30000, 'B', 'Standard'),
    (40, 75000, 'C', 'VIP'),
    (35, 55000, 'A', 'Premium'),
    (22, 28000, 'B', 'Basic');

-- 插入新数据（自动推理）
INSERT INTO test_limix_segments (age, income, category) VALUES (32, 48000, 'A');

-- 验证推理结果
SELECT id, age, income, category, segment FROM test_limix_segments WHERE id > 5;
-- segment 应为 'Premium'（加权多数投票）
```

### 10.2 异常检测

```sql
CREATE TABLE test_limix_anomaly (
    id serial PRIMARY KEY,
    metric_name text,
    value float,
    metric_vec EMBEDDINGS AS (ft_transformer_embedding(metric_name, value)),
    is_anomaly text PREDICT AS (limix_infer())
) WITH (predict_timing = immediate, vector_len = 384, limix_task = 'anomaly');

INSERT INTO test_limix_anomaly (metric_name, value, is_anomaly) VALUES
    ('cpu', 45.0, 'normal'),
    ('cpu', 52.0, 'normal'),
    ('memory', 60.0, 'normal');

INSERT INTO test_limix_anomaly (metric_name, value) VALUES ('cpu', 99.5);
-- is_anomaly 应为 'anomaly'（距离过远）
```

### 10.3 回归预测

```sql
CREATE TABLE test_limix_regression (
    id serial PRIMARY KEY,
    area float,
    rooms int,
    location text,
    feature_vec EMBEDDINGS AS (ft_transformer_embedding(area, rooms, location)),
    price float PREDICT AS (limix_infer()::float)
) WITH (predict_timing = immediate, vector_len = 384, limix_task = 'regression');

INSERT INTO test_limix_regression (area, rooms, location, price) VALUES
    (80.0, 2, 'downtown', 500000),
    (120.0, 3, 'suburb', 350000),
    (60.0, 1, 'downtown', 400000);

INSERT INTO test_limix_regression (area, rooms, location) VALUES (90.0, 2, 'downtown');
-- price 应接近 450000-500000（加权平均）
```

### 10.4 自定义 topn

```sql
CREATE TABLE test_limix_custom_topn (
    id serial PRIMARY KEY,
    product text,
    severity text,
    description text,
    ticket_vec EMBEDDINGS AS (ft_transformer_embedding(product, severity, description)),
    priority text PREDICT AS (limix_infer())
) WITH (predict_timing = immediate, vector_len = 384, limix_topn = 10);

INSERT INTO test_limix_custom_topn (product, severity, description, priority) VALUES
    ('Database', 'P1', 'Production database is down', 'critical'),
    ('API', 'P2', 'API response time increased', 'high'),
    ('UI', 'P3', 'Button color is wrong', 'low');

INSERT INTO test_limix_custom_topn (product, severity, description) VALUES
    ('Database', 'P2', 'Replication lag detected');
-- priority 应为 'high' 或 'critical'（与相似样本一致）
```

### 10.5 无历史数据测试

```sql
CREATE TABLE test_limix_empty (
    id serial PRIMARY KEY,
    feature1 text,
    feature2 int,
    feat_vec EMBEDDINGS AS (ft_transformer_embedding(feature1, feature2)),
    label text PREDICT AS (limix_infer())
) WITH (predict_timing = immediate, vector_len = 384);

INSERT INTO test_limix_empty (feature1, feature2) VALUES ('test', 100);
-- 应返回 WARNING 提示无历史数据，segment 为 NULL
```

### 10.6 信息提取（最近邻）

```sql
CREATE TABLE test_limix_extract (
    id serial PRIMARY KEY,
    source_text text,
    source_vec EMBEDDINGS AS (ft_transformer_embedding(source_text)),
    summary text PREDICT AS (limix_infer())
) WITH (predict_timing = immediate, vector_len = 384, limix_task = 'extraction');

INSERT INTO test_limix_extract (source_text, summary) VALUES
    ('PostgreSQL is a powerful open source database', 'database'),
    ('Python is a popular programming language', 'language');

INSERT INTO test_limix_extract (source_text) VALUES ('MySQL is an open source database');
-- summary 应为 'database'（最近邻复制）
```

---
**文档版本**: 4.0
**最后更新**: 2026-06-08
