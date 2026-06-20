# SELECT INFER 语句级推理开关设计文档

## 1. 需求背景

### 1.1 当前行为

当表使用 `predict_timing = deferred`（默认模式）时，当前实现对所有 SELECT 查询**无条件触发**按需推理：只要扫描到 predict 列为 NULL 的行，就立即执行推理表达式，将结果返回并持久化。

### 1.2 存在的问题

用户无法在单条 SELECT 语句中控制是否触发推理。某些场景下用户可能只想快速查看当前数据状态（NULL 或已有值），而不希望触发耗时的 LLM 推理。例如：

- 查看有多少行尚未推理（`SELECT count(*) FROM t WHERE sentiment IS NULL`）
- 快速浏览数据，不等待推理
- 批量导出原始数据

### 1.3 设计目标

参考 `SELECT DISTINCT` 语法模式，在 SELECT 语句中新增单个关键字 `INFER`，让用户显式指定是否触发按需推理。**默认行为改为跳过推理**，只有显式使用 `INFER` 关键字时才触发推理。

## 2. 语法设计

### 2.1 语法规则

```
SELECT [INFER] [opt_all_clause | distinct_clause] target_list ...
```

| 语法 | 含义 | 行为 |
|------|------|------|
| `SELECT INFER ...` | 立即推理 | 对 predict 列为 NULL 的行立即执行推理，返回推理结果并持久化 |
| `SELECT ...`（默认） | 跳过推理 | 不执行推理，直接返回当前值（NULL 或已有值） |

### 2.2 关键字选择

选择 **`INFER`**（推理）作为新关键字：

- 语义清晰：`INFER` 直接表达"推理"意图
- 单个单词，语法简洁：`SELECT INFER id, sentiment FROM t`
- 与表级 reloption `predict_timing` 的值（`deferred`/`immediate`）无名称冲突
- `infer` 极少被用作列名/表名，设为保留关键字影响极小

**关键字级别**：`RESERVED_KEYWORD`（保留关键字）

设为保留关键字的原因：
- `INFER` 需要放在 `SELECT` 之后、目标列表之前的位置，与 `DISTINCT`/`ALL` 同级
- 如果设为 `UNRESERVED_KEYWORD`，当用户有名为 `infer` 的列时，`SELECT infer FROM t` 会被解析为关键字而非列名，产生歧义
- 设为保留关键字后，`infer` 不能直接用作列名（需加引号 `"infer"`），但这种情况极少见

### 2.3 语法示例

```sql
-- 立即推理：对未推理的行执行推理并返回结果
SELECT INFER id, sentiment FROM deferred_reviews ORDER BY id;

-- 默认行为：跳过推理，直接返回当前值（NULL 表示尚未推理）
SELECT id, sentiment FROM deferred_reviews ORDER BY id;

-- 与 DISTINCT 组合使用
SELECT INFER DISTINCT sentiment FROM deferred_reviews;
SELECT INFER DISTINCT ON (product_name) id, sentiment FROM deferred_reviews;

-- 与 WHERE 组合
SELECT INFER id FROM deferred_reviews WHERE sentiment IS NULL;

-- 子查询中使用
SELECT * FROM (SELECT INFER id, sentiment FROM deferred_reviews) sub;

-- 查看未推理的行数（默认跳过推理，不触发推理）
SELECT count(*) FROM deferred_reviews WHERE sentiment IS NULL;
```

### 2.4 语法位置

参考 `distinct_clause` / `opt_all_clause` 的设计，`INFER` 放在 `SELECT` 关键字之后、`DISTINCT`/`ALL` 和目标列表之前：

```yacc
simple_select:
            SELECT opt_infer_clause opt_all_clause opt_target_list
            into_clause from_clause where_clause
            group_clause having_clause window_clause
                { ... }
            | SELECT opt_infer_clause distinct_clause target_list
            into_clause from_clause where_clause
            group_clause having_clause window_clause
                { ... }
            | values_clause
            | TABLE relation_expr
            | select_clause UNION set_quantifier select_clause
            | select_clause INTERSECT set_quantifier select_clause
            | select_clause EXCEPT set_quantifier select_clause
    ;

opt_infer_clause:
            INFER                { $$ = true; }
            | /*EMPTY*/          { $$ = false; }
    ;
```

## 3. 数据结构设计

### 3.1 SelectStmt 结构体扩展

文件：`src/include/nodes/parsenodes.h`

在 `SelectStmt` 的 leaf 字段区（`distinctClause` 附近）新增字段：

```c
typedef struct SelectStmt
{
    NodeTag     type;

    /* leaf fields */
    List       *distinctClause;
    IntoClause *intoClause;
    List       *targetList;
    List       *fromClause;
    Node       *whereClause;
    List       *groupClause;
    bool        groupDistinct;
    Node       *havingClause;
    List       *windowClause;

    /* NEW: INFER keyword specified in SELECT */
    bool        inferPredict;

    /* values list */
    List       *valuesLists;

    /* common fields */
    ...
} SelectStmt;
```

### 3.2 Query 结构体扩展

文件：`src/include/nodes/parsenodes.h`

在 `Query` 结构体中新增字段，供执行器读取：

```c
typedef struct Query
{
    ...
    List       *distinctClause;
    List       *sortClause;
    ...

    /* NEW: INFER keyword specified in SELECT */
    bool        inferPredict pg_node_attr(query_jumble_ignore);

    ...
} Query;
```

### 3.3 EState 结构体扩展

文件：`src/include/nodes/execnodes.h`

在 `EState` 中新增字段，供 `ExecScanExtended` 读取：

```c
typedef struct EState
{
    ...
    struct EPQState *es_epq_active;
    bool        es_use_parallel_mode;
    ...

    /* NEW: INFER keyword specified for current query */
    bool        es_infer_predict;

    ...
} EState;
```

## 4. 实现方案

### 4.1 语法解析层（gram.y）

#### 4.1.1 新增 INFER 关键字

在 `src/include/parser/kwlist.h` 中添加（保留关键字）：

```c
PG_KEYWORD("infer", INFER, RESERVED_KEYWORD, BARE_LABEL)
```

在 `src/backend/parser/gram.y` 的 token 声明区，`INFER` 会自动通过 `kwlist.h` 生成。

在 `reserved_keyword` 规则中添加：

```yacc
reserved_keyword:
          ...
        | INFER
        | ...
    ;
```

#### 4.1.2 新增 opt_infer_clause 规则

```yacc
opt_infer_clause:
            INFER                { $$ = true; }
            | /*EMPTY*/          { $$ = false; }
    ;
```

#### 4.1.3 修改 simple_select 规则

在两个 SELECT 产生式中插入 `opt_infer_clause`：

```yacc
simple_select:
            SELECT opt_infer_clause opt_all_clause opt_target_list
            into_clause from_clause where_clause
            group_clause having_clause window_clause
                {
                    SelectStmt *n = makeNode(SelectStmt);
                    n->inferPredict = $2;
                    n->targetList = $4;
                    n->intoClause = $5;
                    n->fromClause = $6;
                    n->whereClause = $7;
                    n->groupClause = ($8)->list;
                    n->groupDistinct = ($8)->distinct;
                    n->havingClause = $9;
                    n->windowClause = $10;
                    $$ = (Node *) n;
                }
            | SELECT opt_infer_clause distinct_clause target_list
            into_clause from_clause where_clause
            group_clause having_clause window_clause
                {
                    SelectStmt *n = makeNode(SelectStmt);
                    n->inferPredict = $2;
                    n->distinctClause = $3;
                    n->targetList = $4;
                    n->intoClause = $5;
                    n->fromClause = $6;
                    n->whereClause = $7;
                    n->groupClause = ($8)->list;
                    n->groupDistinct = ($8)->distinct;
                    n->havingClause = $9;
                    n->windowClause = $10;
                    $$ = (Node *) n;
                }
            | values_clause
            | TABLE relation_expr
            | select_clause UNION set_quantifier select_clause
            | select_clause INTERSECT set_quantifier select_clause
            | select_clause EXCEPT set_quantifier select_clause
    ;
```

### 4.2 语义分析层（analyze.c）

在 `transformSelectStmt` 函数中，将 `SelectStmt->inferPredict` 传递到 `Query` 节点：

```c
static Query *
transformSelectStmt(ParseState *pstate, SelectStmt *stmt)
{
    Query      *qry = makeNode(Query);
    ...

    /* NEW: 传递 INFER flag */
    qry->inferPredict = stmt->inferPredict;

    ...
    return qry;
}
```

### 4.3 执行器初始化层

#### 4.3.1 PlannedStmt 结构体扩展

文件：`src/include/nodes/plannodes.h`

```c
typedef struct PlannedStmt
{
    ...
    /* NEW: INFER keyword specified */
    bool        inferPredict;
    ...
} PlannedStmt;
```

在 `src/backend/optimizer/plan/planner.c` 的 `standard_planner` 函数中传递：

```c
PlannedStmt *
standard_planner(Query *parse, ...)
{
    ...
    result->inferPredict = parse->inferPredict;
    ...
}
```

#### 4.3.2 在 EState 初始化时设置 infer_predict

文件：`src/backend/executor/execMain.c`

在 `InitPlan` 函数中，从 `QueryDesc->plannedstmt` 传递到 `EState`：

```c
void
InitPlan(QueryDesc *queryDesc, int eflags)
{
    ...
    estate->es_infer_predict = queryDesc->plannedstmt->inferPredict;
    ...
}
```

### 4.4 执行器扫描层

#### 4.4.1 修改 ExecPredictOnDemand 签名

文件：`src/include/utils/predict.h`

```c
extern void ExecPredictOnDemand(struct RelationData *rel, TupleTableSlot *slot,
                                bool infer_predict);
```

#### 4.4.2 修改 ExecPredictOnDemand 实现

文件：`src/backend/utils/adt/predict.c`

```c
void
ExecPredictOnDemand(Relation rel, TupleTableSlot *slot, bool infer_predict)
{
    static bool in_predict_on_demand = false;
    ...

    /* Prevent recursive entry */
    if (in_predict_on_demand)
        return;

    in_predict_on_demand = true;

    ...

    /* Check predict_timing */
    relopts = (StdRdOptions *) rel->rd_options;
    predict_timing = STDRD_OPTION_PREDICT_TIMING_DEFERRED;
    if (relopts != NULL)
        predict_timing = relopts->predict_timing;

    /* Only do on-demand prediction for deferred mode */
    if (predict_timing != STDRD_OPTION_PREDICT_TIMING_DEFERRED)
    {
        in_predict_on_demand = false;
        return;
    }

    /* NEW: 默认跳过推理，只有显式指定 INFER 才推理 */
    if (!infer_predict)
    {
        in_predict_on_demand = false;
        return;
    }

    /* infer_predict == true，执行推理逻辑... */
    ...
}
```

#### 4.4.3 修改 ExecScanExtended 调用

文件：`src/include/executor/execScan.h`

从 `ScanState` 的 `PlanState` 链路获取 `EState`，读取 `es_infer_predict`：

```c
static pg_attribute_always_inline TupleTableSlot *
ExecScanExtended(ScanState *node,
                 ExecScanAccessMtd accessMtd,
                 ExecScanRecheckMtd recheckMtd,
                 EPQState *epqstate,
                 ExprState *qual,
                 ProjectionInfo *projInfo)
{
    ExprContext *econtext = node->ps.ps_ExprContext;
    TupleTableSlot *slot;
    EState     *estate = node->ps.state;
    bool        infer_predict = estate->es_infer_predict;

    ...

    if (!qual && !projInfo)
    {
        ResetExprContext(econtext);
        slot = ExecScanFetch(node, epqstate, accessMtd, recheckMtd);

        if (!TupIsNull(slot) && node->ss_currentRelation != NULL)
            ExecPredictOnDemand(node->ss_currentRelation, slot, infer_predict);

        return slot;
    }

    ...

    for (;;)
    {
        slot = ExecScanFetch(node, epqstate, accessMtd, recheckMtd);
        if (TupIsNull(slot))
            ...

        econtext->ecxt_scantuple = slot;

        if (node->ss_currentRelation != NULL)
            ExecPredictOnDemand(node->ss_currentRelation, slot, infer_predict);

        ...
    }
}
```

## 5. 行为矩阵

| 表级 `predict_timing` | 语句级关键字 | 行为 |
|----------------------|-------------|------|
| `immediate` | （任意/不写） | INSERT/UPDATE 时已推理，SELECT 时 predict 列已有值，无需按需推理 |
| `deferred` | 不写（默认） | **跳过推理**，返回 NULL 或当前值 |
| `deferred` | `INFER` | **立即推理**，对 NULL 的 predict 列执行推理，返回结果并持久化 |

### 5.1 行为变更说明

与之前实现的关键区别：

| 方面 | 之前行为 | 新行为 |
|------|---------|--------|
| 默认 SELECT | deferred 模式下自动按需推理 | **跳过推理**，返回当前值 |
| 触发推理 | 自动触发 | 需显式写 `SELECT INFER` |
| 查看未推理数据 | 需要特殊技巧 | 直接 `SELECT` 即可 |

## 6. 代码修改文件清单

| 文件路径 | 修改内容 |
|---------|----------|
| `src/include/parser/kwlist.h` | 新增 `INFER` 保留关键字声明 |
| `src/backend/parser/gram.y` | 新增 `INFER` 到 `reserved_keyword`，新增 `opt_infer_clause` 规则，修改 `simple_select` |
| `src/include/nodes/parsenodes.h` | `SelectStmt` 新增 `inferPredict` 字段，`Query` 新增 `inferPredict` 字段 |
| `src/include/nodes/plannodes.h` | `PlannedStmt` 新增 `inferPredict` 字段 |
| `src/include/nodes/execnodes.h` | `EState` 新增 `es_infer_predict` 字段 |
| `src/backend/parser/analyze.c` | `transformSelectStmt` 传递 `inferPredict` |
| `src/backend/optimizer/plan/planner.c` | `standard_planner` 传递 `inferPredict` |
| `src/backend/executor/execMain.c` | `InitPlan` 初始化 `es_infer_predict` |
| `src/include/utils/predict.h` | `ExecPredictOnDemand` 签名新增 `bool infer_predict` 参数 |
| `src/backend/utils/adt/predict.c` | `ExecPredictOnDemand` 实现新增 `infer_predict` 检查 |
| `src/include/executor/execScan.h` | `ExecScanExtended` 从 `EState` 读取 `es_infer_predict` 并传递 |

## 7. 测试计划

### 7.1 语法测试

```sql
-- 测试 INFER 语法
SELECT INFER id, sentiment FROM deferred_reviews;

-- 测试与 DISTINCT 组合
SELECT INFER DISTINCT sentiment FROM deferred_reviews;
SELECT INFER DISTINCT ON (product_name) id, sentiment FROM deferred_reviews;

-- 测试与 WHERE 组合
SELECT INFER id FROM deferred_reviews WHERE sentiment IS NULL;

-- 测试子查询
SELECT * FROM (SELECT INFER id, sentiment FROM deferred_reviews) sub;

-- 测试默认行为（不写 INFER，跳过推理）
SELECT id, sentiment FROM deferred_reviews;

-- 测试 UNION 中使用 INFER
SELECT INFER id, sentiment FROM deferred_reviews
UNION
SELECT INFER id, sentiment FROM other_reviews;
```

### 7.2 功能测试

```sql
-- 准备数据
CREATE TABLE infer_test (
    id serial PRIMARY KEY,
    content text,
    sentiment text PREDICT AS (upper(content)) STORED
) WITH (predict_timing = deferred);

INSERT INTO infer_test (content) VALUES ('hello'), ('world');

-- 测试默认行为（跳过推理）：sentiment 应为 NULL
SELECT id, sentiment FROM infer_test ORDER BY id;
-- 预期：sentiment 列为 NULL

-- 测试 INFER（立即推理）：sentiment 应为推理结果
SELECT INFER id, sentiment FROM infer_test ORDER BY id;
-- 预期：sentiment 列为 'HELLO', 'WORLD'

-- 验证持久化：再次默认查询应有值
SELECT id, sentiment FROM infer_test ORDER BY id;
-- 预期：sentiment 列为 'HELLO', 'WORLD'（已持久化）

-- 测试查看未推理行数
TRUNCATE infer_test;
INSERT INTO infer_test (content) VALUES ('foo'), ('bar');
SELECT count(*) FROM infer_test WHERE sentiment IS NULL;
-- 预期：2（未触发推理）
```

### 7.3 回归测试

在 `regression_test.sql` 中新增 `INFER` 测试用例，并更新现有 deferred 模式测试用例（默认行为变为跳过推理）。

## 8. 注意事项

1. **默认行为变更**：此修改改变了 deferred 模式下 SELECT 的默认行为——从自动按需推理变为跳过推理。现有的 `demo_predict_embedding_rag.sql` 和 `regression_test.sql` 中 deferred 模式的测试用例需要更新（在 SELECT 前加 `INFER`）
2. **子查询传播**：子查询中的 `INFER` 只影响该子查询的扫描，不传播到外层查询
3. **视图**：通过视图查询时，视图定义中的 SELECT 不携带 `INFER`，使用默认行为（跳过推理）
4. **INSERT ... SELECT**：SELECT 部分的 `INFER` 仅影响读取行为，不影响 INSERT
5. **UNION/INTERSECT/EXCEPT**：集合操作的每个分支可以独立指定 `INFER`
6. **CTE（WITH 子句）**：CTE 中的 SELECT 可以独立指定 `INFER`
7. **并行查询**：`es_infer_predict` 在并行 Worker 中需要正确传播
8. **保留关键字影响**：`INFER` 设为保留关键字后，不能直接用作列名/表名，需加引号 `"infer"`
