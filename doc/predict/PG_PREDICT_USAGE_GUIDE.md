# pg_predict 使用指南

## 1. 安装

### 1.1 编译要求

- PostgreSQL 编译时启用 libcurl：`./configure --with-libcurl`
- 系统安装 libcurl 开发库：`sudo apt install libcurl4-openssl-dev`

### 1.2 编译安装

```bash
cd contrib/pg_predict
make
make install
```

### 1.3 启用扩展

```sql
CREATE EXTENSION pg_predict;
```

## 2. 配置

### 2.1 系统级配置

使用 `set_predict_config()` 函数配置全局 LLM 参数：

```sql
SELECT set_predict_config(
    'https://api.openai.com/v1/chat/completions',  -- API 地址
    'sk-your-api-key',                               -- API 密钥
    'gpt-3.5-turbo',                                 -- 模型名称
    0.7,                                              -- 温度
    1024,                                             -- 最大 token
    'You are a helpful assistant',                    -- 系统提示词
    0,                                                -- 历史对话条数
    'knowledge_base'::regclass,                       -- RAG 检索表（可选）
    0.5,                                              -- RAG 相似度阈值（可选）
    5                                                 -- RAG Top-N（可选）
);
```

也可以使用 GUC 参数（适用于临时测试）：

```sql
SET pg_predict.api_url = 'https://api.openai.com/v1/chat/completions';
SET pg_predict.api_key = 'sk-your-api-key';
SET pg_predict.model = 'gpt-3.5-turbo';
SET pg_predict.temperature = 0.7;
SET pg_predict.max_tokens = 1024;
```

> **注意**：deferred 模式下，GUC 参数必须使用 `ALTER SYSTEM SET` 设置为全局级别。

### 2.2 表级配置

为特定表配置 LLM 参数，覆盖系统级配置：

```sql
SELECT set_predict_config(
    'my_table'::regclass,                            -- 表名
    'https://api.openai.com/v1/chat/completions',   -- API 地址
    'sk-your-api-key',                               -- API 密钥
    'gpt-3.5-turbo',                                 -- 模型
    0.7,                                              -- 温度
    1024,                                             -- 最大 token
    'Classify the text into categories',              -- 系统提示词
    'Classify this text: {{content}}',                -- prompt_template
    5                                                 -- 历史对话条数
);
```

### 2.3 查看配置

```sql
-- 查看系统级配置
SELECT * FROM get_predict_config();

-- 查看表级配置（自动回退到系统级）
SELECT * FROM get_predict_config('my_table'::regclass);

-- 直接查询配置表
SELECT * FROM pg_predict_config;
```

## 3. Prompt 模板

### 3.1 模板语法

pg_predict 使用 `{{column_name}}` 双花括号语法（参考 MindsDB），将行数据填充到提示词模板中：

- `{{column_name}}` 会被替换为当前行中对应列的值
- 列名区分大小写
- NULL 值替换为空字符串
- 非 PREDICT 列、非隐藏列都可作为占位符

### 3.2 有模板时的 Prompt

当配置了 `prompt_template` 时，模板中的 `{{column_name}}` 被替换为行数据：

```sql
-- 单列模板
prompt_template = 'Classify this text: {{content}}'
-- 行数据: content='AI is great'
-- 生成: "Classify this text: AI is great"

-- 多列模板
prompt_template = 'Review of {{product_name}}: {{review_text}}'
-- 行数据: product_name='Widget', review_text='Amazing!'
-- 生成: "Review of Widget: Amazing!"

-- 复杂模板
prompt_template = 'Product: {{name}}\nCategory: {{category}}\nPrice: ${{price}}\nDescription: {{desc}}\n\nPredict the rating:'
```

### 3.3 无模板时的自动 Prompt

当未配置 `prompt_template` 时，整行数据自动格式化为键值对文本：

```
Input data:
content: AI is transforming software
author: John
category: 
```

PREDICT 列名会出现在末尾（值为空），引导 LLM 填充预测值。

## 4. 使用 llm_infer 独立推理

```sql
-- 需要先配置 API 参数
SET pg_predict.api_url = 'https://api.openai.com/v1/chat/completions';
SET pg_predict.api_key = 'sk-xxx';

-- 简单推理
SELECT llm_infer('You are a helpful assistant', 'What is the capital of France?');

-- 批量推理
SELECT id, llm_infer('Translate to English', content) as translation
FROM documents
WHERE language = 'zh';
```

## 5. 使用 PREDICT AS 与 PREDICT 列集成

> **重要**：`predict_function` 参数已被移除，所有 predict 列必须使用 `PREDICT AS (expr) STORED` 语法。

### 5.1 文本分类（直接使用 llm_infer）

```sql
CREATE TABLE articles (
    id SERIAL PRIMARY KEY,
    content TEXT,
    category TEXT PREDICT AS (llm_infer(
        'Classify into: technology, sports, politics, entertainment. Reply with only the category name.',
        'Classify this text: ' || content
    )) STORED
) WITH (predict_timing = immediate);

INSERT INTO articles (content) VALUES ('AI and machine learning are transforming software');
-- category 自动填充为 'technology'
```

### 5.2 使用自定义包装函数

```sql
CREATE OR REPLACE FUNCTION classify_text(content TEXT) RETURNS TEXT
AS $$
    SELECT llm_infer(
        'Classify into: technology, sports, politics, entertainment. Reply with only the category name.',
        'Classify this text: ' || content
    );
$$ LANGUAGE SQL VOLATILE;

CREATE TABLE articles (
    id SERIAL PRIMARY KEY,
    content TEXT,
    category TEXT PREDICT AS (classify_text(content)) STORED
) WITH (predict_timing = immediate);
```

### 5.3 情感分析（多列模板）

```sql
CREATE OR REPLACE FUNCTION analyze_sentiment(review_text TEXT, product_name TEXT) RETURNS TEXT
AS $$
    SELECT llm_infer(
        'Analyze sentiment. Reply with only: positive, negative, or neutral.',
        'Review of ' || product_name || ': ' || review_text
    );
$$ LANGUAGE SQL VOLATILE;

CREATE TABLE reviews (
    id SERIAL PRIMARY KEY,
    review_text TEXT,
    product_name TEXT,
    sentiment TEXT PREDICT AS (analyze_sentiment(review_text, product_name)) STORED
) WITH (predict_timing = immediate);

INSERT INTO reviews (review_text, product_name) VALUES ('This product is amazing!', 'Widget Pro');
-- sentiment 自动填充为 'positive'
```

### 5.4 数值预测（自动类型转换）

```sql
CREATE OR REPLACE FUNCTION estimate_price(product_name TEXT, description TEXT) RETURNS INTEGER
AS $$
    SELECT llm_infer(
        'Estimate the price in dollars. Reply with only the number.',
        'Product: ' || product_name || ', Description: ' || description
    )::integer;
$$ LANGUAGE SQL VOLATILE;

CREATE TABLE product_ratings (
    id SERIAL PRIMARY KEY,
    product_name TEXT,
    description TEXT,
    price INTEGER PREDICT AS (estimate_price(product_name, description)) STORED
) WITH (predict_timing = immediate);

INSERT INTO product_ratings (product_name, description) VALUES ('Widget Pro', 'A high-end widget that costs 499 dollars');
-- price 自动填充为 499 (INTEGER 类型自动转换)
```

### 5.5 无模板（自动行格式化）

```sql
CREATE TABLE support_tickets (
    id SERIAL PRIMARY KEY,
    subject TEXT,
    description TEXT,
    priority TEXT PREDICT AS (llm_predict()) STORED
) WITH (predict_timing = immediate);

SELECT set_predict_config(
    'support_tickets'::regclass,
    'https://api.openai.com/v1/chat/completions',
    'sk-xxx',
    'gpt-3.5-turbo',
    0.1, 16,
    'Assign a priority level (high, medium, low) based on the ticket information.',
    NULL,   -- 不使用模板，自动格式化整行
    0
);

INSERT INTO support_tickets (subject, description) VALUES ('System down', 'Production server is not responding');
-- 自动生成的 prompt:
-- "Input data:
--  subject: System down
--  description: Production server is not responding
--  priority: "
```

### 5.6 带历史对话的推理

```sql
CREATE TABLE chat_logs (
    id SERIAL PRIMARY KEY,
    user_message TEXT,
    assistant_reply TEXT PREDICT AS (llm_predict()) STORED
) WITH (predict_timing = immediate);

SELECT set_predict_config(
    'chat_logs'::regclass,
    'https://api.openai.com/v1/chat/completions',
    'sk-xxx',
    'gpt-3.5-turbo',
    0.7, 1024,
    'You are a helpful customer service assistant.',
    '{{user_message}}',
    5    -- 包含最近5条历史对话
);

-- 每次插入时，LLM 会看到最近5条历史对话作为上下文
INSERT INTO chat_logs (user_message) VALUES ('I need help with my order');
INSERT INTO chat_logs (user_message) VALUES ('My order number is 12345');
```

## 6. 使用 llm_rag_infer RAG 增强推理

### 6.1 概述

`llm_rag_infer` 是 RAG（Retrieval-Augmented Generation）增强的 LLM 推理函数。它在调用 LLM 之前，先从指定的知识库表（必须有 EMBEDDING 列）中检索与用户输入最相关的内容，然后将检索结果作为上下文注入到 LLM 的提示词中。

### 6.2 函数签名

```sql
llm_rag_infer(
    system_prompt text,      -- 系统提示词
    history_count integer,   -- 历史对话条数
    user_input text,         -- 最新用户输入
    rag_table regclass,      -- RAG 检索表（必须有 EMBEDDING 列）
    rag_similarity float8,   -- 最小相似度阈值（余弦距离，0.0-2.0）
    rag_topn integer         -- 检索返回的最大行数
) returns text
```

### 6.3 准备知识库表

```sql
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_embedding_st;

CREATE TABLE knowledge_base (
    id SERIAL PRIMARY KEY,
    question TEXT,
    answer TEXT,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'sentence_transformers_embedding'
);

INSERT INTO knowledge_base (question, answer, content) VALUES
('What is PostgreSQL?', 'PostgreSQL is a powerful open source relational database system.', 'What is PostgreSQL?'),
('How to create a table?', 'Use CREATE TABLE statement to define a new table.', 'How to create a table?'),
('What is SQL?', 'SQL is a standard language for managing relational databases.', 'What is SQL?');
```

### 6.4 使用 llm_rag_infer

```sql
SET pg_predict.api_url = 'https://api.openai.com/v1/chat/completions';
SET pg_predict.api_key = 'sk-xxx';

SELECT llm_rag_infer(
    'You are a helpful database assistant. Answer based on the provided context.',
    0,
    'How do I create a new table?',
    'knowledge_base'::regclass,
    0.5,
    3
);
```

### 6.5 使用 PREDICT AS 与 RAG 推理集成

```sql
CREATE OR REPLACE FUNCTION rag_answer(question TEXT) RETURNS TEXT
AS $$
    SELECT llm_rag_infer(
        'Answer the question based on the provided context. If the context does not contain the answer, say you do not know.',
        0,
        question,
        'knowledge_base'::regclass,
        0.5,
        3
    );
$$ LANGUAGE SQL VOLATILE;

CREATE TABLE qa_table (
    id SERIAL PRIMARY KEY,
    question TEXT,
    answer TEXT PREDICT AS (rag_answer(question)) STORED
) WITH (predict_timing = immediate);

INSERT INTO qa_table (question) VALUES ('How to create a table?');
-- answer 自动填充，基于 RAG 检索的上下文
```

### 6.6 RAG 参数说明

| 参数 | 说明 | 推荐值 |
|------|------|--------|
| rag_table | 知识库表，必须有 EMBEDDING 列 | - |
| rag_similarity | 余弦距离阈值，越小越相似 | 0.3-0.7 |
| rag_topn | 检索返回的最大行数 | 3-10 |

## 7. 兼容不同 LLM 服务

### 7.1 OpenAI

```sql
SELECT set_predict_config(
    'https://api.openai.com/v1/chat/completions',
    'sk-xxx', 'gpt-4', 0.7, 2048, '', 0
);
```

### 7.2 本地 Ollama

```sql
SELECT set_predict_config(
    'http://localhost:11434/v1/chat/completions',
    'ollama', 'llama3', 0.7, 2048, '', 0
);
```

### 7.3 本地 vLLM

```sql
SELECT set_predict_config(
    'http://localhost:8000/v1/chat/completions',
    'EMPTY', 'meta-llama/Meta-Llama-3-8B-Instruct', 0.7, 2048, '', 0
);
```

### 7.4 LM Studio

```sql
SELECT set_predict_config(
    'http://localhost:1234/v1/chat/completions',
    'lm-studio', 'local-model', 0.7, 2048, '', 0
);
```

## 8. GUC 参数参考

| 参数 | 类型 | 默认值 | 范围 | 说明 |
|------|------|--------|------|------|
| pg_predict.api_url | string | '' | - | API 地址 |
| pg_predict.api_key | string | '' | - | API 密钥 |
| pg_predict.model | string | 'gpt-3.5-turbo' | - | 模型名称 |
| pg_predict.temperature | real | 0.7 | 0.0-2.0 | 温度参数 |
| pg_predict.max_tokens | integer | 1024 | 1-32768 | 最大 token |
| pg_predict.timeout | integer | 60 | 1-600 | 超时秒数 |

## 9. 故障排查

### 9.1 扩展创建失败

```
ERROR: could not open extension control file
```
**解决**：确认 `pg_predict.control` 和 `pg_predict--1.0.sql` 已安装到 `$SHAREDIR/extension/`

### 9.2 函数加载失败

```
ERROR: could not load library "pg_predict.so": libcurl.so.4: cannot open shared object file
```
**解决**：安装 libcurl 运行时库 `sudo apt install libcurl4`

### 9.3 API 调用失败

```
ERROR: LLM API request failed: Couldn't resolve host name
```
**解决**：检查 API URL 是否正确，网络是否可达

### 9.4 PREDICT 列返回 NULL

**可能原因**：
- LLM API 调用失败（检查日志中的 WARNING）
- 配置未设置（检查 `pg_predict_config` 表）
- 输入列为 NULL

### 9.5 类型转换失败

```
ERROR: invalid input syntax for type integer: "five"
```
**解决**：优化系统提示词，确保 LLM 输出符合目标类型的格式要求

### 9.6 模板占位符未替换

**可能原因**：
- 列名拼写错误（区分大小写）
- 列是 PREDICT 列或隐藏列（不会出现在模板数据中）

### 9.7 RAG 检索失败

```
ERROR: RAG table "xxx" does not have an EMBEDDING column
```
**解决**：确保 RAG 表有 EMBEDDING 列，使用 `content TEXT EMBEDDING` 语法创建

```
ERROR: different vector dimensions
```
**解决**：确保 RAG 表的 embedding 函数与查询使用的维度一致

### 9.8 PREDICT AS 中 llm_rag_predict 未使用 RAG

**可能原因**：
- `rag_table` 未在 `pg_predict_config` 中配置
- `rag_table` 配置为 NULL
- 此时 `llm_rag_predict` 退化为 `llm_predict` 行为
