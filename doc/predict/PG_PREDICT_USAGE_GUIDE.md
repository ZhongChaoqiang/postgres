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
    0                                                 -- 历史对话条数
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

## 5. 使用 llm_predict 与 PREDICT 列集成

### 5.1 文本分类（单列模板）

```sql
CREATE TABLE articles (
    id SERIAL PRIMARY KEY,
    content TEXT,
    category TEXT PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'llm_predict'
);

SELECT set_predict_config(
    'articles'::regclass,
    'https://api.openai.com/v1/chat/completions',
    'sk-xxx',
    'gpt-3.5-turbo',
    0.3, 1024,
    'Classify into: technology, sports, politics, entertainment. Reply with only the category name.',
    'Classify this text: {{content}}',
    0
);

INSERT INTO articles (content) VALUES ('AI and machine learning are transforming software');
-- category 自动填充为 'technology'
```

### 5.2 情感分析（多列模板）

```sql
CREATE TABLE reviews (
    id SERIAL PRIMARY KEY,
    review_text TEXT,
    product_name TEXT,
    sentiment TEXT PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'llm_predict'
);

SELECT set_predict_config(
    'reviews'::regclass,
    'https://api.openai.com/v1/chat/completions',
    'sk-xxx',
    'gpt-3.5-turbo',
    0.1, 256,
    'Analyze sentiment. Reply with only: positive, negative, or neutral.',
    'Review of {{product_name}}: {{review_text}}',
    0
);

INSERT INTO reviews (review_text, product_name) VALUES ('This product is amazing!', 'Widget Pro');
-- sentiment 自动填充为 'positive'
```

### 5.3 数值预测（自动类型转换）

```sql
CREATE TABLE product_ratings (
    id SERIAL PRIMARY KEY,
    product_name TEXT,
    description TEXT,
    price INTEGER PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'llm_predict'
);

SELECT set_predict_config(
    'product_ratings'::regclass,
    'https://api.openai.com/v1/chat/completions',
    'sk-xxx',
    'gpt-3.5-turbo',
    0.1, 16,
    'Estimate the price in dollars. Reply with only the number.',
    'Product: {{product_name}}, Description: {{description}}',
    0
);

INSERT INTO product_ratings (product_name, description) VALUES ('Widget Pro', 'A high-end widget that costs 499 dollars');
-- price 自动填充为 499 (INTEGER 类型自动转换)
```

### 5.4 无模板（自动行格式化）

```sql
CREATE TABLE support_tickets (
    id SERIAL PRIMARY KEY,
    subject TEXT,
    description TEXT,
    priority TEXT PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'llm_predict'
);

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

### 5.5 带历史对话的推理

```sql
CREATE TABLE chat_logs (
    id SERIAL PRIMARY KEY,
    user_message TEXT,
    assistant_reply TEXT PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'llm_predict'
);

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

## 6. 兼容不同 LLM 服务

### 6.1 OpenAI

```sql
SELECT set_predict_config(
    'https://api.openai.com/v1/chat/completions',
    'sk-xxx', 'gpt-4', 0.7, 2048, '', 0
);
```

### 6.2 本地 Ollama

```sql
SELECT set_predict_config(
    'http://localhost:11434/v1/chat/completions',
    'ollama', 'llama3', 0.7, 2048, '', 0
);
```

### 6.3 本地 vLLM

```sql
SELECT set_predict_config(
    'http://localhost:8000/v1/chat/completions',
    'EMPTY', 'meta-llama/Meta-Llama-3-8B-Instruct', 0.7, 2048, '', 0
);
```

### 6.4 LM Studio

```sql
SELECT set_predict_config(
    'http://localhost:1234/v1/chat/completions',
    'lm-studio', 'local-model', 0.7, 2048, '', 0
);
```

## 7. GUC 参数参考

| 参数 | 类型 | 默认值 | 范围 | 说明 |
|------|------|--------|------|------|
| pg_predict.api_url | string | '' | - | API 地址 |
| pg_predict.api_key | string | '' | - | API 密钥 |
| pg_predict.model | string | 'gpt-3.5-turbo' | - | 模型名称 |
| pg_predict.temperature | real | 0.7 | 0.0-2.0 | 温度参数 |
| pg_predict.max_tokens | integer | 1024 | 1-32768 | 最大 token |
| pg_predict.timeout | integer | 60 | 1-600 | 超时秒数 |

## 8. 故障排查

### 8.1 扩展创建失败

```
ERROR: could not open extension control file
```
**解决**：确认 `pg_predict.control` 和 `pg_predict--1.0.sql` 已安装到 `$SHAREDIR/extension/`

### 8.2 函数加载失败

```
ERROR: could not load library "pg_predict.so": libcurl.so.4: cannot open shared object file
```
**解决**：安装 libcurl 运行时库 `sudo apt install libcurl4`

### 8.3 API 调用失败

```
ERROR: LLM API request failed: Couldn't resolve host name
```
**解决**：检查 API URL 是否正确，网络是否可达

### 8.4 PREDICT 列返回 NULL

**可能原因**：
- LLM API 调用失败（检查日志中的 WARNING）
- 配置未设置（检查 `pg_predict_config` 表）
- 输入列为 NULL

### 8.5 类型转换失败

```
ERROR: invalid input syntax for type integer: "five"
```
**解决**：优化系统提示词，确保 LLM 输出符合目标类型的格式要求

### 8.6 模板占位符未替换

**可能原因**：
- 列名拼写错误（区分大小写）
- 列是 PREDICT 列或隐藏列（不会出现在模板数据中）
