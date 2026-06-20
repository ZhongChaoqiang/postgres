# SELECT INFER 功能测试报告

## 1. 测试概述

### 1.1 测试目标

验证 `SELECT INFER` 关键字功能，在 `predict_timing = deferred` 模式下，用户可以通过 `INFER` 关键字显式控制是否在 SELECT 时触发按需推理。

### 1.2 测试环境

- 操作系统: Ubuntu 22.04 (WSL2)
- PostgreSQL 版本: 18.3
- 编译选项: `-Dcassert=true -Doptimization=g`
- 扩展: vector, jolix_predict

### 1.3 功能说明

| 语法 | 行为 |
|------|------|
| `SELECT INFER ...` | 立即推理：对 predict 列为 NULL 的行执行推理，返回结果并持久化 |
| `SELECT ...`（默认） | 跳过推理：直接返回当前值（NULL 或已有值） |

## 2. 测试用例与结果

### 2.1 基本功能测试

#### 测试 1: 默认 SELECT 跳过推理

```sql
CREATE TABLE infer_test (
    id serial PRIMARY KEY,
    content text,
    sentiment text PREDICT AS (upper(content)) STORED
) WITH (predict_timing = deferred);

INSERT INTO infer_test (content) VALUES ('hello'), ('world');
SELECT id, content, sentiment FROM infer_test ORDER BY id;
```

**预期**: sentiment 列为 NULL（跳过推理）
**实际**: sentiment 列为 NULL
**结果**: PASS

#### 测试 2: SELECT INFER 触发推理

```sql
SELECT INFER id, content, sentiment FROM infer_test ORDER BY id;
```

**预期**: sentiment 列为 'HELLO', 'WORLD'
**实际**: sentiment 列为 'HELLO', 'WORLD'
**结果**: PASS

#### 测试 3: 持久化验证

```sql
SELECT id, content, sentiment FROM infer_test ORDER BY id;
```

**预期**: sentiment 列有值（推理结果已持久化）
**实际**: sentiment 列为 'HELLO', 'WORLD'
**结果**: PASS

### 2.2 组合语法测试

#### 测试 4: SELECT INFER DISTINCT

```sql
SELECT INFER DISTINCT sentiment FROM infer_test ORDER BY sentiment;
```

**预期**: 返回去重后的 sentiment 值
**实际**: 返回 HELLO, WORLD
**结果**: PASS

#### 测试 5: SELECT INFER ... WHERE

```sql
TRUNCATE infer_test;
INSERT INTO infer_test (content) VALUES ('foo'), ('bar'), ('baz');
SELECT INFER id, content FROM infer_test WHERE sentiment IS NULL ORDER BY id;
```

**预期**: 返回 3 行（先 WHERE 过滤 NULL 行，再推理）
**实际**: 返回 3 行 (id=13,14,15)
**结果**: PASS

#### 测试 6: 查看未推理行数

```sql
TRUNCATE infer_test;
INSERT INTO infer_test (content) VALUES ('one'), ('two'), ('three');
SELECT count(*) AS null_count FROM infer_test WHERE sentiment IS NULL;
```

**预期**: 返回 3（默认跳过推理，不触发新推理）
**实际**: 返回 3
**结果**: PASS

### 2.3 LLM 推理测试

#### 测试 7: deferred 模式 LLM 情感分析

```sql
CREATE TABLE deferred_reviews (
    id serial PRIMARY KEY,
    product_name text,
    review_text text,
    sentiment text PREDICT AS (llm_infer(
        'Classify the sentiment...',
        'Product: ' || product_name || '. Review: ' || review_text
    )) STORED
) WITH (predict_timing = deferred);

INSERT INTO deferred_reviews (product_name, review_text) VALUES
    ('iPhone 15 Pro', 'The camera quality is outstanding...'),
    ('MacBook Air M3', 'The price is too high...'),
    ('AirPods Pro 2', 'Noise cancellation is top-notch.');

-- 默认 SELECT：sentiment 为 NULL
SELECT id, product_name, sentiment FROM deferred_reviews ORDER BY id;

-- SELECT INFER：触发 LLM 推理
SELECT INFER id, product_name, sentiment FROM deferred_reviews ORDER BY id;

-- 验证持久化
SELECT id, product_name, sentiment FROM deferred_reviews ORDER BY id;
```

**预期**: 默认 SELECT 返回 NULL，SELECT INFER 返回推理结果，再次 SELECT 返回持久化结果
**实际**: 符合预期
**结果**: PASS

## 3. 已知限制

### 3.1 子查询中的 INFER

子查询中的 `INFER` 关键字可能不生效，因为子查询在规划阶段可能被内联（flatten）到外层查询中，导致 INFER 信息丢失。

```sql
-- 子查询中的 INFER 可能不生效
SELECT * FROM (SELECT INFER id, sentiment FROM t) sub;
```

** workaround**: 直接在主查询中使用 INFER，或使用 CTE。

### 3.2 推理执行时机

推理在 WHERE 条件检查之后、投影之前执行。这意味着：
- `SELECT INFER ... WHERE sentiment IS NULL` → 先找到 NULL 行，再推理（正确行为）
- `SELECT INFER ... WHERE sentiment = 'positive'` → WHERE 看到的是推理前的值（NULL），不会匹配

如果需要按推理后的值过滤，请先执行 `SELECT INFER`，再执行带条件的 `SELECT`。

## 4. 测试结论

SELECT INFER 功能实现正确，所有核心测试用例通过。用户可以通过 `SELECT INFER` 显式触发按需推理，默认 SELECT 行为为跳过推理。

## 5. 修改文件清单

| 文件 | 修改内容 |
|------|---------|
| `src/include/parser/kwlist.h` | 新增 INFER 保留关键字 |
| `src/backend/parser/gram.y` | 新增 INFER token、opt_infer_clause 规则，修改 simple_select |
| `src/include/nodes/parsenodes.h` | SelectStmt 和 Query 新增 inferPredict 字段 |
| `src/include/nodes/plannodes.h` | PlannedStmt 新增 inferPredict 字段 |
| `src/include/nodes/execnodes.h` | EState 新增 es_infer_predict 字段 |
| `src/backend/parser/analyze.c` | transformSelectStmt 传递 inferPredict |
| `src/backend/optimizer/plan/planner.c` | standard_planner 传递 inferPredict |
| `src/backend/executor/execMain.c` | InitPlan 初始化 es_infer_predict |
| `src/include/utils/predict.h` | ExecPredictOnDemand 签名新增参数 |
| `src/backend/utils/adt/predict.c` | ExecPredictOnDemand 新增 infer_predict 检查 |
| `src/include/executor/execScan.h` | ExecScanExtended 读取并传递 es_infer_predict |
