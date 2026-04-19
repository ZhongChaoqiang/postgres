# PostgreSQL PREDICT 功能完整使用指南

## 环境要求

- PostgreSQL 自定义版本（支持PREDICT功能）
- PL/Python3 扩展（如使用Python预测函数）

## 快速开始

### 1. 创建预测函数

预测函数接收整行记录作为参数，返回预测值：

```sql
CREATE OR REPLACE FUNCTION predict_score(row_data record)
RETURNS float
AS $$
DECLARE
    feature_value float;
BEGIN
    feature_value := row_data.feature;
    RETURN feature_value * 2.0 + 1.0;
END;
$$ LANGUAGE plpgsql IMMUTABLE;
```

也可以使用 Python 实现更复杂的预测逻辑：

```sql
CREATE OR REPLACE FUNCTION ml_predict(row_data record)
RETURNS float
AS $$
import pickle
import numpy as np

feature = row_data['feature']
model = pickle.loads(open('/tmp/model.pkl', 'rb').read())
result = model.predict(np.array([[feature]]))[0]
return float(result)
$$ LANGUAGE plpython3u IMMUTABLE;
```

### 2. 创建表

```sql
CREATE TABLE predictions (
    id SERIAL PRIMARY KEY,
    feature FLOAT,
    score FLOAT PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'predict_score'
);
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

插入/更新时立即调用预测函数：

```sql
CREATE TABLE predictions (
    id SERIAL PRIMARY KEY,
    feature FLOAT,
    score FLOAT PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'predict_score'
);
```

- INSERT 时 PREDICT 列为 NULL → 触发器立即调用预测函数
- INSERT 时 PREDICT 列有值 → 使用用户提供的值
- UPDATE 时始终同步 `_actual` 列

### deferred 模式（延迟预测，默认）

插入时不预测，由后台工作进程异步处理：

```sql
CREATE TABLE predictions (
    id SERIAL PRIMARY KEY,
    feature FLOAT,
    score FLOAT PREDICT
) WITH (
    predict_timing = deferred,
    predict_function = 'predict_score'
);
```

- INSERT 时 PREDICT 列保持 NULL
- 后台 `async_predict` 工作进程定期扫描并填充预测值
- 适合预测函数耗时较长的场景

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

## 预测函数格式

### 单一函数

所有 PREDICT 列使用同一个预测函数：

```sql
CREATE TABLE t (
    id SERIAL PRIMARY KEY,
    x FLOAT,
    y FLOAT PREDICT
) WITH (
    predict_function = 'my_predict'
);
```

### 列特定函数

不同 PREDICT 列使用不同预测函数：

```sql
CREATE TABLE t (
    id SERIAL PRIMARY KEY,
    x FLOAT,
    y FLOAT PREDICT,
    z FLOAT PREDICT
) WITH (
    predict_function = 'y:predict_y;z:predict_z'
);
```

格式：`列名:函数名`，用分号分隔。

## ALTER TABLE 操作

### 添加 PREDICT 列

```sql
ALTER TABLE my_table ADD COLUMN prediction FLOAT PREDICT;
```

自动创建 `_predict`、`_actual` 隐藏列和触发器。

### 设置预测函数

```sql
-- 方式1：专用语法
ALTER TABLE my_table SET PREDICT FUNCTION my_predict;

-- 方式2：通用SET语法
ALTER TABLE my_table SET (predict_function = 'my_predict');
```

### 修改预测时机

```sql
ALTER TABLE my_table SET (predict_timing = immediate);
ALTER TABLE my_table SET (predict_timing = deferred);
ALTER TABLE my_table RESET (predict_timing);
```

## 多个 PREDICT 列

```sql
CREATE TABLE multi_predict (
    id SERIAL PRIMARY KEY,
    feature1 FLOAT,
    feature2 FLOAT,
    prediction1 FLOAT PREDICT,
    prediction2 FLOAT PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'prediction1:predict_func1;prediction2:predict_func2'
);
```

每个 PREDICT 列都会自动创建 `_predict`、`_actual` 隐藏列和触发器。

## WITH 参数完整参考

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `predict_timing` | enum | deferred | 预测时机：immediate 或 deferred |
| `predict_function` | string | 无 | 预测函数名称，支持列特定格式 |

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
2. 如果 PREDICT 列为 NULL，调用预测函数
3. 将预测结果写入 PREDICT 列和 `_predict` 列
4. 将用户输入值写入 `_actual` 列

### 异步预测时（deferred 模式）

1. 后台工作进程定期扫描表
2. 查找 PREDICT 列为 NULL 的行
3. 调用预测函数
4. 通过 SPI 执行 UPDATE 更新 PREDICT 列和 `_predict` 列

### 查询时

- `SELECT *` 自动过滤隐藏列（`_predict`、`_actual`）
- 显式引用隐藏列可以查看预测值和实际值
- 无查询重写，直接返回存储的值

## 示例场景

### 评分预测

```sql
CREATE OR REPLACE FUNCTION predict_rating(row_data record)
RETURNS float
AS $$
DECLARE
    price float;
    reviews int;
BEGIN
    price := row_data.price;
    reviews := row_data.review_count;
    RETURN LEAST(5.0, GREATEST(1.0, 3.0 + (reviews::float / 100.0) - (price / 50.0)));
END;
$$ LANGUAGE plpgsql IMMUTABLE;

CREATE TABLE products (
    id SERIAL PRIMARY KEY,
    name TEXT,
    price FLOAT,
    review_count INT,
    predicted_rating FLOAT PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'predict_rating'
);

INSERT INTO products (name, price, review_count) VALUES
    ('Widget A', 25.0, 200),
    ('Widget B', 50.0, 50);

SELECT id, name, predicted_rating, predicted_rating_actual FROM products;
```

### 分类预测

```sql
CREATE OR REPLACE FUNCTION predict_category(row_data record)
RETURNS text
AS $$
DECLARE
    description text;
BEGIN
    description := lower(row_data.description);
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
    category TEXT PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'predict_category'
);
```

### 延迟预测（ML模型推理）

```sql
CREATE TABLE ml_predictions (
    id SERIAL PRIMARY KEY,
    features vector(128),
    label INT PREDICT
) WITH (
    predict_timing = deferred,
    predict_function = 'ml_classify'
);

-- 批量插入，不立即预测
INSERT INTO ml_predictions (features)
SELECT '[0.1, 0.2, ...]'::vector FROM generate_series(1, 10000);

-- 后台工作进程自动处理
-- 可以通过查询检查预测进度
SELECT count(*) AS pending FROM ml_predictions WHERE label IS NULL;
```

## 注意事项

1. **预测函数签名**：必须接受 `record` 参数，返回与 PREDICT 列相同类型
2. **NULL 处理**：PREDICT 列为 NULL 时才触发预测，非 NULL 值直接使用
3. **触发器顺序**：predict_trigger 是 BEFORE 触发器，在其他 BEFORE 触发器之后执行
4. **异步工作进程**：建议 `async_predict_workers = 1` 避免并发冲突
5. **隐藏列**：`_predict` 和 `_actual` 列在 `SELECT *` 中不显示，需显式引用

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

### 问题：预测函数调用失败

**原因**：函数签名不匹配

**解决**：确保函数接受 `record` 参数并返回正确类型
```sql
-- 正确的函数签名
CREATE FUNCTION my_predict(record) RETURNS float ...;
```

### 问题：ALTER TABLE ADD COLUMN PREDICT 未创建隐藏列

**原因**：早期版本 bug，已修复

**解决**：确保使用最新编译版本

## 总结

PostgreSQL PREDICT 功能提供了：

- ✅ 自动预测（触发器 + 异步工作进程）
- ✅ 灵活的预测时机（immediate/deferred）
- ✅ 列特定预测函数
- ✅ 隐藏列机制（_predict/_actual）
- ✅ ALTER TABLE 支持
- ✅ 异步预测后台处理
- ✅ 简单易用的SQL接口

让预测像普通SQL操作一样简单！
