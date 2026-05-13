# PREDICT 功能使用文档

## 环境要求

- PostgreSQL 自定义版本（支持PREDICT功能）
- jolix_predict 扩展（如使用LLM推理功能）
- PL/Python3 扩展（如使用Python预测函数）

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
- **触发器** `jolix_predict_{column}_{oid}`：BEFORE INSERT OR UPDATE，自动调用预测函数
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

## 内置 LLM 推理函数

jolix_predict 扩展提供了内置的 LLM 推理函数，无需手动创建预测函数即可使用大语言模型进行推理。

### LLM 配置

在使用 LLM 函数之前，需要先配置 API 连接信息。有两种配置方式：

#### 方式一：GUC 参数配置

```sql
-- 会话级别配置
SET jolix_predict.api_url = 'https://api.openai.com/v1/chat/completions';
SET jolix_predict.api_key = 'sk-xxx';
SET jolix_predict.model = 'gpt-3.5-turbo';

-- 全局级别配置（适用于后台工作进程）
ALTER SYSTEM SET jolix_predict.api_url = 'https://ark.cn-beijing.volces.com/api/v3/chat/completions';
ALTER SYSTEM SET jolix_predict.api_key = '<your-api-key>';
ALTER SYSTEM SET jolix_predict.model = '<your-model-name>';
SELECT pg_reload_conf();
```

#### 方式二：配置表配置

```sql
-- 系统级配置（所有表默认使用）
SELECT set_predict_config(
    p_api_url := 'https://api.openai.com/v1/chat/completions',
    p_api_key := 'sk-xxx',
    p_model_name := 'gpt-3.5-turbo',
    p_temperature := 0.7,
    p_max_tokens := 1024,
    p_system_prompt := 'You are a helpful assistant.'
);

-- 表级配置（覆盖系统级配置）
SELECT set_predict_config(
    p_table_name := 'articles',
    p_api_url := 'https://api.openai.com/v1/chat/completions',
    p_api_key := 'sk-xxx',
    p_model_name := 'gpt-4',
    p_system_prompt := 'Classify articles into categories.',
    p_prompt_template := 'Title: {{title}}\nContent: {{content}}\nCategory:',
    p_history_count := 3
);

-- 查看当前配置
SELECT * FROM get_predict_config();
SELECT * FROM get_predict_config('articles');
```

### GUC 参数完整参考

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `jolix_predict.api_url` | string | 空 | LLM API 地址 |
| `jolix_predict.api_key` | string | 空 | LLM API 密钥 |
| `jolix_predict.model` | string | gpt-3.5-turbo | LLM 模型名称 |
| `jolix_predict.temperature` | float8 | 0.7 | 生成温度（0.0-2.0） |
| `jolix_predict.max_tokens` | int | 1024 | 最大生成 token 数 |
| `jolix_predict.timeout` | int | 60 | API 请求超时时间（秒） |

### 配置表参数参考

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `p_api_url` | text | - | LLM API 地址 |
| `p_api_key` | text | 空 | LLM API 密钥 |
| `p_model_name` | text | gpt-3.5-turbo | 模型名称 |
| `p_temperature` | float8 | 0.7 | 生成温度 |
| `p_max_tokens` | integer | 1024 | 最大 token 数 |
| `p_system_prompt` | text | 空 | 系统提示词 |
| `p_prompt_template` | text | NULL | 提示词模板（仅表级配置） |
| `p_history_count` | integer | 0 | 历史对话轮数 |
| `p_rag_table` | regclass | NULL | RAG 检索表（必须有 EMBEDDING 列） |
| `p_rag_similarity` | float8 | 0.5 | RAG 最小相似度阈值 |
| `p_rag_topn` | integer | 5 | RAG 检索结果数量 |

### llm_infer 函数

独立的 LLM 推理函数，直接调用 LLM API 返回结果。支持从 GUC 参数或配置表读取配置（优先使用配置表中的系统级配置）。

```sql
-- 基本用法：system_prompt + user_input
SELECT llm_infer('You are a translator.', 'Translate to English: 你好世界');

-- 带历史对话：system_prompt + history_count + user_input
SELECT llm_infer('You are a chatbot.', 3, 'What is PostgreSQL?');
```

**参数说明**：

| 参数 | 类型 | 说明 |
|------|------|------|
| `system_prompt` | text | 系统提示词 |
| `history_count` | integer | 历史对话轮数（可选，默认0） |
| `user_input` | text | 用户输入文本 |

**配置读取优先级**：

1. **配置表系统级配置**（`set_predict_config` 设置的，优先级最高）
2. **GUC 参数**（`SET jolix_predict.*` 设置的，作为默认值）

**使用场景**：
- 直接在 SQL 中调用 LLM
- 作为 PREDICT AS 表达式的一部分
- 在存储过程中调用

**配置方式一：使用配置表（推荐）**：

```sql
-- 设置系统级配置，llm_infer 会自动读取
SELECT set_predict_config(
    p_api_url := 'https://api.openai.com/v1/chat/completions',
    p_api_key := 'sk-xxx',
    p_model_name := 'gpt-4',
    p_temperature := 0.5,
    p_max_tokens := 512,
    p_system_prompt := 'You are a helpful assistant.'
);

-- 直接调用，无需设置 GUC 参数
SELECT llm_infer('You are a translator.', 'Translate to English: 你好世界');
```

**配置方式二：使用 GUC 参数**：

```sql
SET jolix_predict.api_url = 'https://api.openai.com/v1/chat/completions';
SET jolix_predict.api_key = 'sk-xxx';
SET jolix_predict.model = 'gpt-3.5-turbo';

SELECT llm_infer('You are a translator.', 'Translate to English: 你好世界');
```

**在 PREDICT 列中使用**：

```sql
CREATE TABLE llm_articles (
    id serial PRIMARY KEY,
    content text,
    category text PREDICT AS (llm_infer(
        'Classify into: technology, sports, politics, entertainment. Reply only the category name.',
        'Classify: ' || content
    )) STORED
) WITH (predict_timing = immediate);
```

### llm_predict_ext 函数

默认的 PREDICT 列推理函数，自动将整行数据发送给 LLM。从 `jolix_predict_config` 表读取配置。

```sql
-- 配置表
SELECT set_predict_config(
    p_table_name := 'articles',
    p_api_url := 'https://api.openai.com/v1/chat/completions',
    p_api_key := 'sk-xxx',
    p_system_prompt := 'You are a text classifier. Classify the article into a category.',
    p_prompt_template := 'Title: {{title}}\nContent: {{content}}\nCategory:'
);

-- 创建表，使用 llm_predict_ext 作为预测函数
CREATE TABLE articles (
    id serial PRIMARY KEY,
    title text,
    content text,
    category text PREDICT AS (llm_predict_ext(articles)) STORED
) WITH (predict_timing = immediate);
```

**特点**：
- 自动将整行数据格式化为文本发送给 LLM
- 支持 `prompt_template`，使用 `{{column_name}}` 语法引用列值
- 支持 `history_count`，包含历史对话上下文
- 无模板时自动格式化为 `key: value` 格式
- 自动跳过 PREDICT 列和隐藏列

**prompt_template 语法**：

```
Title: {{title}}
Content: {{content}}
Please classify this article into a category:
```

`{{column_name}}` 会被替换为对应列的值。

### llm_rag_infer 函数

RAG（检索增强生成）推理函数，先从指定表中检索相关文档，再结合检索结果调用 LLM。支持从 GUC 参数或配置表读取配置（优先使用配置表中的系统级配置）。

```sql
-- RAG 推理
SELECT llm_rag_infer(
    'Answer questions based on the provided context.',  -- system_prompt
    0,                                                   -- history_count
    'What is PostgreSQL?',                               -- user_input
    'knowledge_base',                                    -- rag_table（必须有EMBEDDING列）
    0.5,                                                 -- rag_similarity（余弦距离阈值）
    5                                                    -- rag_topn（检索结果数）
);
```

**参数说明**：

| 参数 | 类型 | 说明 |
|------|------|------|
| `system_prompt` | text | 系统提示词 |
| `history_count` | integer | 历史对话轮数 |
| `user_input` | text | 用户输入文本 |
| `rag_table` | regclass | RAG 检索表（必须有 EMBEDDING 列） |
| `rag_similarity` | float8 | 余弦距离阈值（0.0-2.0，越小越相似） |
| `rag_topn` | integer | 检索结果数量 |

**RAG 检索流程**：
1. 使用 EMBEDDING 列的嵌入函数将 `user_input` 转换为向量
2. 在 `rag_table` 中进行向量相似度搜索
3. 将检索结果作为上下文与用户输入合并
4. 调用 LLM 生成回答

**前提条件**：`rag_table` 必须有 EMBEDDING 列。

### llm_rag_predict_ext 函数

RAG 增强的 PREDICT 列推理函数，结合行数据和 RAG 检索结果调用 LLM。

```sql
-- 配置 RAG
SELECT set_predict_config(
    p_table_name := 'qa_pairs',
    p_api_url := 'https://api.openai.com/v1/chat/completions',
    p_api_key := 'sk-xxx',
    p_system_prompt := 'Answer questions based on the provided context.',
    p_rag_table := 'knowledge_base',    -- RAG 检索表
    p_rag_similarity := 0.5,            -- 相似度阈值
    p_rag_topn := 5                     -- 检索结果数
);

-- 创建表
CREATE TABLE qa_pairs (
    id serial PRIMARY KEY,
    question text,
    answer text PREDICT AS (llm_rag_predict_ext(qa_pairs)) STORED
) WITH (predict_timing = immediate);
```

**特点**：
- 先从 RAG 表检索相关文档
- 将检索结果与行数据合并后发送给 LLM
- 如果未配置 `rag_table`，退化为 `llm_predict_ext` 行为

### 内置函数对比

| 函数 | 用途 | 配置来源 | RAG 支持 | 行数据 |
|------|------|---------|---------|--------|
| `llm_infer` | 独立推理 | 配置表系统级 > GUC 参数 | 否 | 否 |
| `llm_predict_ext` | PREDICT 列推理 | 配置表表级 > 系统级 > GUC | 否 | 自动格式化 |
| `llm_rag_infer` | RAG 独立推理 | 配置表系统级 > GUC 参数 | 是（参数指定） | 否 |
| `llm_rag_predict_ext` | RAG PREDICT 列推理 | 配置表表级 > 系统级 > GUC | 是（配置表指定） | 自动格式化 |

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

使用 `jolix_predict` 扩展的 `llm_infer` 函数实现 LLM 文本分类：

```sql
-- 1. 配置 LLM API（必须使用 ALTER SYSTEM SET，因为 worker 是独立进程）
ALTER SYSTEM SET jolix_predict.api_url = 'https://ark.cn-beijing.volces.com/api/v3/chat/completions';
ALTER SYSTEM SET jolix_predict.api_key = '<your-api-key>';
ALTER SYSTEM SET jolix_predict.model = '<your-model-name>';
SELECT pg_reload_conf();

-- 2. 创建表，直接使用 llm_infer 函数
CREATE TABLE test_llm_articles (
    id serial PRIMARY KEY,
    content text,
    category text PREDICT AS (llm_infer(
        'Classify the following text into exactly one category: technology, sports, politics, entertainment. Reply with only the category name, nothing else.',
        'Classify this text: ' || content
    )) STORED
) WITH (predict_timing=immediate);

-- 3. INSERT 不提供 category - LLM 自动分类
INSERT INTO test_llm_articles (content) VALUES ('AI and machine learning are transforming software development');

-- 4. INSERT 提供 category - 保存为实际值
INSERT INTO test_llm_articles (content, category) VALUES ('The new movie broke box office records', 'entertainment');
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
ALTER SYSTEM SET jolix_predict.api_key = '<your-api-key>';
SELECT pg_reload_conf();
```

---
**文档版本**: 1.0  
**最后更新**: 2026-05-13
