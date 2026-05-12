# PostgreSQL PREDICT 功能完整使用指南

## 环境要求

- PostgreSQL 自定义版本（支持PREDICT功能）
- PL/Python3 扩展（如使用Python预测函数）
- pg_predict 扩展（如使用LLM推理功能）

## 快速开始

### 1. 创建预测函数

预测函数接收源列值作为参数，返回预测值：

```sql
CREATE OR REPLACE FUNCTION add_tax(price numeric) RETURNS numeric
AS $$ SELECT price * 1.1 $$ LANGUAGE SQL IMMUTABLE;
```

也可以使用 Python 实现更复杂的预测逻辑：

```sql
CREATE OR REPLACE FUNCTION ml_predict(feature float)
RETURNS float
AS $$
import pickle
import numpy as np

model = pickle.loads(open('/tmp/model.pkl', 'rb').read())
result = model.predict(np.array([[feature]]))[0]
return float(result)
$$ LANGUAGE plpython3u IMMUTABLE;
```

### 2. 创建表

使用 `PREDICT AS (expr) STORED` 语法创建预测列：

```sql
CREATE TABLE predictions (
    id SERIAL PRIMARY KEY,
    feature FLOAT,
    score FLOAT PREDICT AS (feature * 2.0 + 1.0) STORED
) WITH (predict_timing = immediate);
```

建表时自动创建：
- **隐藏列** `{column}_predict`：存储预测值，与PREDICT列同类型
- **隐藏列** `{column}_actual`：存储用户输入的实际值，与PREDICT列同类型
- **触发器** `pg_predict_{column}_{oid}`：BEFORE INSERT OR UPDATE，自动调用预测函数
- **复合索引** `{table}_{column}_predict_idx`：在PREDICT列和_predict列上创建B-tree索引

### 3. 插入数据

```sql
-- 插入时不指定PREDICT列，自动预测
INSERT INTO predictions (feature) VALUES (5.0);

-- 插入时指定PREDICT列，使用指定值
INSERT INTO predictions (feature, score) VALUES (3.0, 10.0);

-- 批量插入
INSERT INTO predictions (feature)
SELECT generate_series(1.0, 100.0, 1.0);
```

### 4. 查询数据

```sql
-- 普通查询，SELECT * 不显示隐藏列
SELECT * FROM predictions;

-- 查看预测值和实际值
SELECT id, feature, score, score_predict, score_actual FROM predictions;

-- 查看预测准确性
SELECT id, feature, score, score_actual,
       score - score_actual AS prediction_error
FROM predictions WHERE score_actual IS NOT NULL;
```

## 预测时机

### immediate 模式（即时预测）

插入/更新时立即计算预测表达式：

```sql
CREATE TABLE predictions (
    id SERIAL PRIMARY KEY,
    feature FLOAT,
    score FLOAT PREDICT AS (feature * 2.0 + 1.0) STORED
) WITH (predict_timing = immediate);
```

- INSERT 时 PREDICT 列为 NULL → 触发器立即计算预测表达式
- INSERT 时 PREDICT 列有值 → 使用用户提供的值
- UPDATE 时始终同步 `_actual` 列

### deferred 模式（延迟预测，默认）

插入时不预测，由后台工作进程异步处理：

```sql
CREATE TABLE predictions (
    id SERIAL PRIMARY KEY,
    feature FLOAT,
    score FLOAT PREDICT AS (feature * 2.0 + 1.0) STORED
) WITH (predict_timing = deferred);
```

- INSERT 时 PREDICT 列保持 NULL
- 后台 `async_predict` 工作进程定期扫描并填充预测值
- 适合预测函数耗时较长的场景（如LLM推理）

## 异步预测配置

### GUC 参数

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `async_predict_workers` | int | 2 | 后台工作进程数量 |
| `async_predict_naptime` | int | 60 | 扫描间隔（秒） |
| `async_predict_batch_size` | int | 100 | 每次扫描处理的最大行数 |
| `async_predict_enabled` | bool | true | 是否启用异步预测 |

```sql
-- 配置示例
SET async_predict_workers = 1;
SET async_predict_naptime = 30;
SET async_predict_batch_size = 500;
```

> **注意**：建议 `async_predict_workers = 1`，多个工作进程可能导致并发更新冲突。

## 多个 PREDICT 列

每个 PREDICT 列使用独立的预测表达式：

```sql
CREATE FUNCTION add_tax(numeric) RETURNS numeric
AS $$ SELECT $1 * 1.1 $$ LANGUAGE SQL IMMUTABLE;

CREATE FUNCTION double_price(numeric) RETURNS numeric
AS $$ SELECT $1 * 2 $$ LANGUAGE SQL IMMUTABLE;

CREATE TABLE multi_predict (
    id SERIAL PRIMARY KEY,
    price numeric,
    price_with_tax numeric PREDICT AS (add_tax(price)) STORED,
    price_doubled numeric PREDICT AS (double_price(price)) STORED
) WITH (predict_timing = immediate);
```

每个 PREDICT 列都会自动创建 `_predict`、`_actual` 隐藏列和触发器。

## WITH 参数完整参考

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `predict_timing` | enum | deferred | 预测时机：immediate 或 deferred |

## 隐藏列机制

| 列 | 类型 | 说明 |
|----|------|------|
| `{column}` | 基础类型 | PREDICT 列，存储预测结果（或用户提供的值） |
| `{column}_predict` | 基础类型 | 隐藏列，存储预测函数的输出 |
| `{column}_actual` | 基础类型 | 隐藏列，存储用户输入的实际值 |

- `SELECT *` 不显示隐藏列
- 显式引用隐藏列可以查询：`SELECT score_predict, score_actual FROM t`

## 工作原理

### 插入/更新时（immediate 模式）

1. BEFORE 触发器检测 PREDICT 列
2. 如果 PREDICT 列为 NULL，计算预测表达式
3. 将预测结果写入 PREDICT 列和 `_predict` 列
4. 将用户输入值写入 `_actual` 列

### 异步预测时（deferred 模式）

1. 后台工作进程定期扫描表
2. 查找 PREDICT 列为 NULL 的行
3. 通过 SPI 执行预测表达式
4. 通过 SPI 执行 UPDATE 更新 PREDICT 列和 `_predict` 列

### 查询时

- `SELECT *` 自动过滤隐藏列（`_predict`、`_actual`）
- 显式引用隐藏列可以查看预测值和实际值
- 无查询重写，直接返回存储的值

## 示例场景

### 评分预测

```sql
CREATE TABLE products (
    id SERIAL PRIMARY KEY,
    name TEXT,
    price FLOAT,
    review_count INT,
    predicted_rating FLOAT PREDICT AS (
        LEAST(5.0, GREATEST(1.0, 3.0 + (review_count::float / 100.0) - (price / 50.0)))
    ) STORED
) WITH (predict_timing = immediate);

INSERT INTO products (name, price, review_count) VALUES
    ('Widget A', 25.0, 200),
    ('Widget B', 50.0, 50);

SELECT id, name, predicted_rating, predicted_rating_actual FROM products;
```

### 分类预测

```sql
CREATE OR REPLACE FUNCTION predict_category(description text)
RETURNS text
AS $$
BEGIN
    description := lower(description);
    IF description LIKE '%database%' THEN RETURN 'tech';
    ELSIF description LIKE '%health%' THEN RETURN 'medical';
    ELSIF description LIKE '%finance%' THEN RETURN 'business';
    ELSE RETURN 'other';
    END IF;
END;
$$ LANGUAGE plpgsql IMMUTABLE;

CREATE TABLE articles (
    id SERIAL PRIMARY KEY,
    title TEXT,
    description TEXT,
    category TEXT PREDICT AS (predict_category(description)) STORED
) WITH (predict_timing = immediate);
```

### LLM 文本分类

使用 `pg_predict` 扩展的 `llm_infer` 函数实现 LLM 文本分类：

```sql
-- 1. 安装扩展
CREATE EXTENSION IF NOT EXISTS pg_predict;

-- 2. 配置 LLM API（必须使用 ALTER SYSTEM SET，因为 worker 是独立进程）
ALTER SYSTEM SET pg_predict.api_url = 'https://ark.cn-beijing.volces.com/api/v3/chat/completions';
ALTER SYSTEM SET pg_predict.api_key = '<your-api-key>';
ALTER SYSTEM SET pg_predict.model = '<your-model-name>';
SELECT pg_reload_conf();

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

-- 5. INSERT 提供 category - 保存为实际值
INSERT INTO test_llm_articles (content, category) VALUES ('The new movie broke box office records', 'entertainment');
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

### 延迟预测（ML模型推理）

```sql
CREATE TABLE ml_predictions (
    id SERIAL PRIMARY KEY,
    features vector(128),
    label INT PREDICT AS (ml_classify(features)) STORED
) WITH (predict_timing = deferred);

-- 批量插入，不立即预测
INSERT INTO ml_predictions (features)
SELECT '[0.1, 0.2, ...]'::vector FROM generate_series(1, 10000);

-- 后台工作进程自动处理
-- 可以通过查询检查预测进度
SELECT count(*) AS pending FROM ml_predictions WHERE label IS NULL;
```

## ALTER TABLE 操作

### 添加 PREDICT 列

```sql
ALTER TABLE my_table ADD COLUMN prediction FLOAT PREDICT AS (feature * 2.0) STORED;
```

自动创建 `_predict`、`_actual` 隐藏列和触发器。

### 修改预测时机

```sql
ALTER TABLE my_table SET (predict_timing = immediate);
ALTER TABLE my_table SET (predict_timing = deferred);
ALTER TABLE my_table RESET (predict_timing);
```

## 注意事项

1. **预测表达式**：使用 `PREDICT AS (expr) STORED` 语法指定，表达式引用同表其他列
2. **NULL 处理**：PREDICT 列为 NULL 时才触发预测，非 NULL 值直接使用
3. **触发器顺序**：predict_trigger 是 BEFORE 触发器，在其他 BEFORE 触发器之后执行
4. **异步工作进程**：建议 `async_predict_workers = 1` 避免并发冲突
5. **隐藏列**：`_predict` 和 `_actual` 列在 `SELECT *` 中不显示，需显式引用
6. **VOLATILE 函数**：predict 列允许使用 VOLATILE 函数（如 LLM 推理），不受 GENERATED 列的 IMMUTABLE 限制
7. **GUC 参数**：deferred 模式下，LLM 相关的 GUC 参数必须使用 `ALTER SYSTEM SET` 设置为全局级别

## 故障排除

### 问题：预测值始终为 NULL

**原因**：`predict_timing = deferred` 且异步工作进程未运行

**解决**：
```sql
-- 检查异步预测是否启用
SHOW async_predict_enabled;

-- 切换为即时预测
ALTER TABLE my_table SET (predict_timing = immediate);
```

### 问题：LLM 推理在 deferred 模式下不执行

**原因**：GUC 参数未设置为全局级别

**解决**：使用 `ALTER SYSTEM SET` 代替 `SET`
```sql
ALTER SYSTEM SET pg_predict.api_key = '<your-api-key>';
SELECT pg_reload_conf();
```

### 问题：ALTER TABLE ADD COLUMN PREDICT 未创建隐藏列

**原因**：早期版本 bug，已修复

**解决**：确保使用最新编译版本

## 总结

PostgreSQL PREDICT 功能提供了：

- ✅ 自动预测（触发器 + 异步工作进程）
- ✅ 灵活的预测时机（immediate/deferred）
- ✅ 内联表达式语法 `PREDICT AS (expr) STORED`
- ✅ 多个 predict 列使用不同表达式
- ✅ 隐藏列机制（_predict/_actual）
- ✅ ALTER TABLE 支持
- ✅ 异步预测后台处理
- ✅ 支持 VOLATILE 函数（LLM 推理）
- ✅ 简单易用的SQL接口

让预测像普通SQL操作一样简单！
