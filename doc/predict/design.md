# PREDICT AS 语法设计文档

## 1. 概述

本文档描述了 PostgreSQL 中 `PREDICT AS (expr) STORED` 语法的实现设计。该语法允许用户在列定义中内联指定预测函数表达式，类似于 `GENERATED ALWAYS AS (expr) STORED`，但使用 `PREDICT` 关键字标记为预测列。该语法替代了原有的 `predict_function` reloption 方式，支持 `predict_timing` 参数和多个 predict 列使用不同的函数。

与 GENERATED ALWAYS AS 的关键区别是：predict 列允许用户直接写入值。当用户提供了值时，该值保存到 predict 列本身和 `_actual` 伴随列；当用户没有提供值时，通过推理函数计算的结果保存到 predict 列本身和 `_predict` 伴随列。

## 2. 语法

### 2.1 新增语法

```sql
column_name data_type PREDICT AS (expression) STORED
```

### 2.2 示例

#### 基本用法

```sql
CREATE FUNCTION add_tax(numeric) RETURNS numeric
AS $$ SELECT $1 * 1.1 $$ LANGUAGE SQL IMMUTABLE;

CREATE TABLE products (
    id int PRIMARY KEY,
    price numeric,
    price_with_tax numeric PREDICT AS (add_tax(price)) STORED
) WITH (predict_timing=immediate);
```

#### INSERT 不提供 predict 列值（计算预测值）

```sql
INSERT INTO products (id, price) VALUES (1, 100);
-- 结果: price_with_tax = 110.0, price_with_tax_predict = 110.0, price_with_tax_actual = NULL
```

#### INSERT 提供 predict 列值（保存实际值）

```sql
INSERT INTO products (id, price, price_with_tax) VALUES (2, 200, 220);
-- 结果: price_with_tax = 220, price_with_tax_predict = NULL, price_with_tax_actual = 220
```

#### UPDATE 源列（重新计算预测值）

```sql
UPDATE products SET price = 150 WHERE id = 1;
-- 结果: price_with_tax = 165.0, price_with_tax_predict = 165.0, price_with_tax_actual = NULL
```

#### UPDATE predict 列（更新实际值）

```sql
UPDATE products SET price_with_tax = 275 WHERE id = 2;
-- 结果: price_with_tax = 275, price_with_tax_actual = 275
```

#### 多个 predict 列使用不同函数

```sql
CREATE FUNCTION add_tax(numeric) RETURNS numeric
AS $$ SELECT $1 * 1.1 $$ LANGUAGE SQL IMMUTABLE;

CREATE FUNCTION double_price(numeric) RETURNS numeric
AS $$ SELECT $1 * 2 $$ LANGUAGE SQL IMMUTABLE;

CREATE TABLE products (
    id int PRIMARY KEY,
    price numeric,
    price_with_tax numeric PREDICT AS (add_tax(price)) STORED,
    price_doubled numeric PREDICT AS (double_price(price)) STORED
) WITH (predict_timing=immediate);
```

#### 延迟预测模式

```sql
CREATE TABLE products (
    id int PRIMARY KEY,
    price numeric,
    price_with_tax numeric PREDICT AS (add_tax(price)) STORED
) WITH (predict_timing=deferred);
```

### 2.3 与 GENERATED ALWAYS AS 的对比

| 特性 | GENERATED ALWAYS AS (expr) STORED | PREDICT AS (expr) STORED |
|------|-----------------------------------|--------------------------|
| 语法 | `col type GENERATED ALWAYS AS (expr) STORED` | `col type PREDICT AS (expr) STORED` |
| attgenerated 值 | 's' | 'p' |
| attpredict 值 | false | true |
| INSERT 时自动计算 | 是 | 是（immediate模式，用户未提供值时） |
| UPDATE 时自动重算 | 是 | 是（immediate模式，用户未修改predict列时） |
| 允许直接写入 | 否 | 是（写入值保存到_actual列） |
| 表达式必须 IMMUTABLE | 是 | 是 |
| 不能引用其他生成列 | 是 | 是 |
| 伴随列 | 无 | _predict, _actual |
| 计算方式 | ExecComputeStoredGenerated | predict_trigger |
| 支持延迟计算 | 否 | 是（predict_timing=deferred） |
| 支持异步后台计算 | 否 | 是（async_predict worker） |

### 2.4 与原有 predict_function reloption 的关系

`PREDICT AS (expr) STORED` 语法替代了原有的 `predict_function` reloption 方式：

| 特性 | predict_function reloption | PREDICT AS (expr) STORED |
|------|---------------------------|--------------------------|
| 函数指定方式 | `WITH (predict_function='func_name')` | 内联表达式 `PREDICT AS (func(col)) STORED` |
| 多列支持 | `WITH (predict_function='col_a:func_a;col_b:func_b')` | 每列独立指定 |
| 表达式存储 | reloption 字符串 | pg_attrdef 系统目录 |
| 计算方式 | predict_trigger 通过函数OID调用 | predict_trigger 通过 build_column_default 计算表达式 |
| 允许直接写入 | 是 | 是 |

两种方式在触发器中通过 `attgenerated` 字段区分：
- `attgenerated == ATTRIBUTE_GENERATED_PREDICT ('p')`：使用内联表达式
- `attgenerated != 'p'` 但 `attpredict == true`：使用 predict_function reloption

## 3. 实现架构

### 3.1 修改文件清单

| 文件 | 修改内容 |
|------|----------|
| `src/include/catalog/pg_attribute.h` | 添加 `ATTRIBUTE_GENERATED_PREDICT` ('p') 常量 |
| `src/include/catalog/pg_attribute_d.h` | 添加 `ATTRIBUTE_GENERATED_PREDICT` ('p') 常量 |
| `src/include/nodes/parsenodes.h` | 添加 `CONSTR_PREDICT` 枚举值和 `generated_kind` 字段 |
| `src/include/access/tupdesc.h` | 添加 `has_generated_predict` 标志 |
| `src/backend/parser/gram.y` | 添加 `opt_predict_clause` 语法规则 |
| `src/backend/parser/parse_utilcmd.c` | 处理 `CONSTR_PREDICT` 约束，创建伴随列 |
| `src/backend/access/common/tupdesc.c` | 复制和比较 `has_generated_predict` |
| `src/backend/utils/cache/relcache.c` | 设置 `has_generated_predict` 标志 |
| `src/backend/executor/nodeModifyTable.c` | 跳过 predict 列的 ExecComputeStoredGenerated；允许 predict 列接受用户值 |
| `src/backend/executor/execMain.c` | 显示 predict 列类型 |
| `src/backend/commands/tablecmds.c` | 处理 predict 列建表/修改逻辑，创建触发器 |
| `src/backend/rewrite/rewriteHandler.c` | 允许 predict 列接受 INSERT/UPDATE 的用户值 |
| `src/backend/utils/adt/predict.c` | predict_trigger 支持内联表达式计算，区分用户值和预测值 |
| `src/backend/postmaster/async_predict.c` | async_predict worker 支持内联表达式 |
| `src/bin/pg_dump/pg_dump.c` | 导出 predict 列语法 |
| `src/bin/psql/describe.c` | \d 命令显示 predict 列信息 |

### 3.2 语法解析流程

```
SQL: CREATE TABLE t (a int, b numeric PREDICT AS (add_tax(a)) STORED) WITH (predict_timing=immediate)
                    │
                    ▼
┌──────────────────────────────────────────────────────┐
│ 1. gram.y 解析                                       │
│    PREDICT AS (add_tax(a)) STORED                    │
│    → Constraint {CONSTR_PREDICT, generated_kind='p'} │
│    通过 opt_predict_clause 规则在 columnDef 级别处理  │
└──────────────────────┬───────────────────────────────┘
                       ▼
┌──────────────────────────────────────────────────────┐
│ 2. parse_utilcmd.c 处理                              │
│    column->is_predict = true                         │
│    column->generated = 'p'                           │
│    column->raw_default = add_tax(a) 解析树           │
│    自动创建 _predict 和 _actual 伴随列               │
│    自动创建复合索引                                   │
└──────────────────────┬───────────────────────────────┘
                       ▼
┌──────────────────────────────────────────────────────┐
│ 3. tablecmds.c / heap.c 处理                         │
│    - BuildDescForRelation: att->attgenerated = 'p'   │
│    - heap_create_with_catalog: 写入 pg_attribute     │
│    - 生成表达式作为默认值存入 pg_attrdef              │
│    - 创建 _predict/_actual 伴随列                    │
│    - 创建 predict_trigger 触发器                     │
│    - 创建复合索引                                    │
└──────────────────────┬───────────────────────────────┘
                       ▼
┌──────────────────────────────────────────────────────┐
│ 4. INSERT/UPDATE 执行                                │
│    - rewriteHandler.c: 允许 predict 列接受用户值     │
│    - nodeModifyTable.c: 跳过 predict 列的自动计算    │
│    - predict_trigger: BEFORE INSERT/UPDATE 触发器    │
│      - 用户提供值: 保留到 predict 列，保存到 _actual │
│      - 用户未提供值:                                 │
│        - immediate: 计算表达式，保存到 predict 和    │
│          _predict 列                                 │
│        - deferred: 不计算，等待 async worker         │
└──────────────────────────────────────────────────────┘
```

### 3.3 关键设计决策

1. **语法位置**：`PREDICT AS (expr) STORED` 在 `columnDef` 规则中作为 `opt_predict_clause` 处理，位于 `ColQualList` 之后、`opt_embedding` 之前。这是因为 `PREDICT` 是 unreserved keyword，放在约束列表中会导致与裸 `PREDICT` 关键字的 shift/reduce 冲突。

2. **attgenerated 值**：使用 `'p'` 作为 predict 列的 `attgenerated` 值，与 STORED (`'s'`) 和 VIRTUAL (`'v'`) 区分。

3. **兼容性**：predict 列同时设置 `attpredict = true`，确保与现有 predict 基础设施（如 predict_trigger、async_predict worker）兼容。

4. **创建伴随列**：使用 `PREDICT AS (expr) STORED` 语法的列仍然创建 `_predict` 和 `_actual` 伴随列，与原有 predict_function 方式保持一致。`_predict` 列存储预测值，`_actual` 列存储用户提供的实际值（用于后续比较预测准确性）。

5. **predict 列可写**：与 GENERATED ALWAYS AS 列不同，predict 列允许用户直接写入值。这是通过在 `rewriteHandler.c` 和 `nodeModifyTable.c` 中对 `attgenerated == ATTRIBUTE_GENERATED_PREDICT` 的列跳过写入限制实现的。

6. **使用触发器计算**：predict 列不使用 `ExecComputeStoredGenerated`，而是通过 `predict_trigger` BEFORE INSERT/UPDATE 触发器计算。这样可以在触发器中根据 `predict_timing` 决定是否立即计算，并区分用户值和预测值。

7. **表达式计算方式**：在触发器中使用 `build_column_default` + `ExecPrepareExpr` + `ExecEvalExpr` 的方式计算内联表达式，而不是通过 SPI 执行 SQL 查询。这避免了 BEFORE INSERT 触发器中 ctid 无效的问题。

8. **UPDATE 时判断用户是否修改 predict 列**：在 BEFORE UPDATE 触发器中，通过比较新旧 tuple 中 predict 列的值来判断用户是否明确修改了 predict 列。如果新旧值相同，说明用户只是更新了其他列，predict 列应该重新计算；如果新旧值不同，说明用户明确修改了 predict 列，新值应保存到 `_actual`。

9. **跳过 ExecComputeStoredGenerated**：在 `nodeModifyTable.c` 的 `ExecInitGenerated` 中，predict 列被跳过（`continue`），因为计算由触发器负责。

## 4. 系统目录变更

### 4.1 pg_attribute

| 字段 | 新值 | 说明 |
|------|------|------|
| attgenerated | 'p' | ATTRIBUTE_GENERATED_PREDICT |
| attpredict | true | 兼容现有 predict 基础设施 |

### 4.2 pg_attrdef

predict 列的内联表达式存储在 `pg_attrdef` 系统目录中，与 GENERATED 列共享相同的存储机制。

### 4.3 TupleConstr

| 字段 | 说明 |
|------|------|
| has_generated_predict | true 表示关系包含 predict 列 |

## 5. 触发器行为

### 5.1 predict_trigger

`predict_trigger` 是一个 BEFORE INSERT OR UPDATE 触发器，处理所有 predict 列的计算：

1. **遍历所有 predict 列**：通过 `attpredict` 标志识别

2. **判断用户是否提供了值**：
   - INSERT：检查 predict 列是否为 NULL
   - UPDATE：比较新旧 tuple 中 predict 列的值，如果相同则视为未修改

3. **用户提供值（!isnull）**：
   - 保留 predict 列的用户值不变
   - 将用户值保存到 `_actual` 列

4. **用户未提供值（isnull）**：
   - 检查 `predict_timing`：
     - `immediate`：计算预测值，保存到 predict 列和 `_predict` 列
     - `deferred`：不计算，等待 async_predict worker

5. **计算方式**：
   - `attgenerated == 'p'`：使用 `build_column_default` 获取表达式，通过 `ExecEvalExpr` 计算
   - 其他：使用 `predict_function` reloption 指定的函数

### 5.2 async_predict worker

异步预测后台 worker 支持 `PREDICT AS (expr) STORED` 语法的延迟计算：

1. 检测 `attgenerated == ATTRIBUTE_GENERATED_PREDICT` 的列
2. 使用 `build_column_default` 获取表达式
3. 通过 `ExecPrepareExpr` + `ExecEvalExpr` 计算表达式值
4. 使用 SPI UPDATE 语句将计算结果写回表中

## 6. 测试验证

### 6.1 基础测试用例

| 测试场景 | 预期结果 | 状态 |
|----------|----------|------|
| INSERT 不提供 predict 值 (immediate) | predict 列自动计算，_predict 存储预测值 | ✅ 通过 |
| INSERT 提供 predict 值 | 用户值保存到 predict 列和 _actual 列 | ✅ 通过 |
| UPDATE 源列 (无 actual) | predict 列重新计算，_predict 更新 | ✅ 通过 |
| UPDATE 源列 (有 actual) | predict 列重新计算，_actual 保留 | ✅ 通过 |
| UPDATE predict 列直接修改 | 用户值保存到 predict 列和 _actual 列 | ✅ 通过 |
| 多个 predict 列不同函数 | 每列独立计算 | ✅ 通过 |
| deferred 模式 INSERT 不提供值 | predict 列为空，等待 worker | ✅ 通过 |
| deferred 模式 INSERT 提供值 | 用户值保存到 predict 列和 _actual 列 | ✅ 通过 |
| 复合索引 | 自动创建 | ✅ 通过 |

### 6.2 LLM 分类测试用例

使用 `PREDICT AS (expr) STORED` 语法配合 `pg_predict` 扩展实现 LLM 文本分类：

#### 测试步骤

1. 安装 `pg_predict` 扩展并配置 LLM API
2. 创建使用 `PREDICT AS` 语法的表，直接调用 `llm_infer` 函数
3. INSERT 数据验证 LLM 分类

#### 完整测试脚本

```sql
-- 1. 安装扩展
CREATE EXTENSION IF NOT EXISTS pg_predict;

-- 2. 配置 LLM API (GUC 参数)
SET pg_predict.api_url = 'https://ark.cn-beijing.volces.com/api/v3/chat/completions';
SET pg_predict.api_key = '<your-api-key>';
SET pg_predict.model = '<your-model-name>';

-- 3. 创建表，直接使用 llm_infer 函数
CREATE TABLE test_llm_articles (
    id serial PRIMARY KEY,
    content text,
    category text PREDICT AS (llm_infer(
        'Classify the following text into exactly one category: technology, sports, politics, entertainment. Reply with only the category name, nothing else.',
        'Classify this text: ' || content
    )) STORED
) WITH (predict_timing=immediate);

-- 4. INSERT 不提供 category - LLM 自动分类
INSERT INTO test_llm_articles (content) VALUES ('AI and machine learning are transforming software development');
INSERT INTO test_llm_articles (content) VALUES ('The basketball game was exciting with a last-second shot');
INSERT INTO test_llm_articles (content) VALUES ('New government policies on climate change were announced today');

-- 5. INSERT 提供 category - 保存为实际值
INSERT INTO test_llm_articles (content, category) VALUES ('The new movie broke box office records this weekend', 'entertainment');
```

也可以创建自定义包装函数来简化调用，支持 `{{column}}` 模板语法：

```sql
CREATE OR REPLACE FUNCTION classify_text(prompt text, question_template text, content_value text) RETURNS text
AS $$
    SELECT llm_infer(
        prompt,
        replace(question_template, '{{content}}', content_value)
    );
$$ LANGUAGE SQL VOLATILE;

CREATE TABLE test_llm_articles (
    id serial PRIMARY KEY,
    content text,
    category text PREDICT AS (classify_text(
        'Classify the following text into exactly one category: technology, sports, politics, entertainment. Reply with only the category name, nothing else.',
        'Classify this text: {{content}}',
        content
    )) STORED
) WITH (predict_timing=immediate);
```

#### 测试结果

| ID | Content | Category | category_predict | category_actual |
|----|---------|----------|------------------|-----------------|
| 1 | AI and machine learning are transforming software development | technology | technology | |
| 2 | The basketball game was exciting with a last-second shot | sports | sports | |
| 3 | New government policies on climate change were announced today | politics | politics | |
| 4 | The new movie broke box office records this weekend | entertainment | | entertainment |

| 测试场景 | 预期结果 | 状态 |
|----------|----------|------|
| LLM 自动分类 technology | category=technology, category_predict=technology | ✅ 通过 |
| LLM 自动分类 sports | category=sports, category_predict=sports | ✅ 通过 |
| LLM 自动分类 politics | category=politics, category_predict=politics | ✅ 通过 |
| 用户提供 category | category=entertainment, category_actual=entertainment | ✅ 通过 |

## 7. 关键代码修改说明

### 7.1 允许 predict 列使用 VOLATILE 表达式

在 `src/backend/catalog/heap.c` 中，predict 列跳过了 IMMUTABLE 检查：

```c
if (attgenerated != ATTRIBUTE_GENERATED_PREDICT)
{
    if (contain_mutable_functions_after_planning((Expr *) expr))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                 errmsg("generation expression is not immutable")));
}
```

这是因为 predict 列可能调用 LLM API 等外部服务，这些函数必须是 VOLATILE 的。

### 7.2 允许 predict 列接受用户写入

在 `src/backend/rewrite/rewriteHandler.c` 中，predict 列跳过了 generated 列的写入限制：

```c
if (att_tup->attgenerated && !apply_default &&
    att_tup->attgenerated != ATTRIBUTE_GENERATED_PREDICT)
{
    /* ... 报错：不能写入 generated 列 ... */
}
```

在 `src/backend/executor/nodeModifyTable.c` 中，predict 列跳过了类型检查限制：

```c
if (attr->attgenerated != ATTRIBUTE_GENERATED_PREDICT &&
    (!IsA(tle->expr, Const) || !((Const *) tle->expr)->constisnull))
    ereport(ERROR, ...);
```
