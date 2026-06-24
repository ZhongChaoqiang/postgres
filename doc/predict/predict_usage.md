# Jolix Predict 使用文档

**最后更新**: 2026-06-20

## 1. 概述

Jolix Predict 是一个 PostgreSQL 扩展，提供 LLM（大语言模型）推理功能，支持直接在 SQL 中调用 LLM API、对话历史管理和 RAG（检索增强生成）。

## 2. 安装与配置

### 2.1 自动安装

Jolix Predict 扩展在数据库初始化时自动创建，无需手动执行 `CREATE EXTENSION`。

### 2.2 LLM 配置

使用 `set_llm_config()` 配置 LLM API 参数：

```sql
-- 系统级配置（对所有表生效）
SELECT set_llm_config(
    p_api_url := 'https://api.openai.com/v1/chat/completions',
    p_api_key := 'sk-xxx',
    p_model_name := 'gpt-4',
    p_temperature := 0.7,
    p_max_tokens := 1024,
    p_system_prompt := 'You are a helpful assistant.',
    p_prompt_template := '',
    p_history_count := 0,
    p_rag_similarity := 0.5,
    p_rag_topn := 5
);

-- 表级配置（对特定表生效，覆盖系统级配置）
SELECT set_llm_config(
    p_table_name := 'my_table',
    p_api_url := 'https://api.openai.com/v1/chat/completions',
    p_api_key := 'sk-xxx',
    p_model_name := 'gpt-4',
    p_system_prompt := 'You are a data analyst.',
    p_prompt_template := 'Analyze this data: {{content}}',
    p_history_count := 3,
    p_rag_similarity := 2.0,
    p_rag_topn := 3
);
```

### 2.3 查看配置

```sql
-- 查看系统级配置
SELECT * FROM get_llm_config();

-- 查看表级配置（含系统级回退）
SELECT * FROM get_llm_config('my_table');
```

### 2.4 GUC 参数

也可以通过 GUC 参数配置（优先级低于配置表）：

```sql
SET jolix_predict.llm_api_url = 'https://api.openai.com/v1/chat/completions';
SET jolix_predict.llm_api_key = 'sk-xxx';
SET jolix_predict.llm_model = 'gpt-4';
SET jolix_predict.llm_timeout = 300;
```

## 3. 函数说明

### 3.1 llm_infer 函数

调用 LLM API 进行推理，支持三种重载形式：

```sql
-- 基本调用（自动记录历史到默认表）
SELECT llm_infer(
    'You are a helpful assistant.',   -- system_prompt
    'What is PostgreSQL?'              -- user_input
);

-- 带历史记录
SELECT llm_infer(
    'You are a helpful assistant.',   -- system_prompt
    'Tell me more about it.',          -- user_input
    3                                  -- history_count（最近3轮对话）
);

-- 带历史记录和指定表名
SELECT llm_infer(
    'You are a helpful assistant.',   -- system_prompt
    'Tell me more about it.',          -- user_input
    3,                                 -- history_count
    'my_table'                         -- table_name（历史记录关联的表名）
);
```

**参数说明**：

| 参数 | 类型 | 必选 | 说明 |
|------|------|------|------|
| `system_prompt` | text | 是 | 系统提示词。传入 NULL 或空字符串时使用 `set_llm_config()` 配置的 `system_prompt` |
| `user_input` | text | 是 | 用户输入文本。传入 NULL 或空字符串时使用 `set_llm_config()` 配置的 `prompt_template` |
| `history_count` | integer | 否 | 历史对话轮数（默认0） |
| `table_name` | text | 否 | 历史记录关联的表名 |

**说明**：
- 两参数版本会自动将对话记录到 `jolix_predict.llm_history_table` 指定的默认表（默认为 `"default"`）
- 当 `system_prompt` 为 NULL 或空字符串时，自动使用 `set_llm_config()` 配置的 `system_prompt`
- 当 `user_input` 为 NULL 或空字符串时，自动使用 `set_llm_config()` 配置的 `prompt_template`
- 当 `history_count > 0` 时，从 `jolix_llm_history` 表读取最近的 N 轮 Q&A 对话作为上下文
- 推理完成后自动将 Q&A 记录到历史表
- 内容超过 4096 字符会自动截断
- 设置 `jolix_predict.llm_history_table = ''` 可禁用自动记录

### 3.2 limix_infer 函数

LimiX 本地推理函数（零参数），基于向量相似度（k-NN）检索历史样本数据进行推理。利用当前表的 EMBEDDING 向量列搜索相似行，根据相似行的 PREDICT 列值直接推理出当前行的预测结果。**无需调用外部 LLM API**，推理完全在本地完成。

```sql
-- 零参数调用，全自动本地推理
segment text PREDICT AS (limix_infer())
```

**函数签名**：

```sql
limix_infer() RETURNS text
```

**表级 WITH 参数**：

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `limix_model` | text | `'limix-2m'` | 本地推理模型名称（预留） |
| `limix_task` | text | `'classification'` | 任务类别：`classification`、`regression`、`extraction`、`anomaly` |
| `limix_topn` | integer | `5` | 检索的相似行数量（k-NN 的 k 值） |

**自动推断机制**：

| 推断项 | 推断方式 |
|--------|---------|
| 当前表名 | GUC: `jolix_predict.current_table`（由 PREDICT 触发器自动设置，支持 schema 限定名） |
| EMBEDDING 列名 | 自动检测 `pg_attribute` 中 `attembedding=true` 或 `attembeddings=true` 的第一个列，返回隐藏的 `_embedding` 向量列名 |
| PREDICT 列名 | 自动检测 `pg_attribute` 中 `attpredict=true` 的第一个列 |
| 搜索向量 | GUC: `jolix_predict.limix_current_vector`（由 PREDICT 触发器自动设置） |
| 推理算法 | 根据 `limix_task` 自动选择 k-NN 变体 |

**limix_task 与推理算法**：

| task 值 | 推理算法 | 输出格式 |
|---------|---------|---------|
| `classification` | 加权多数投票（距离越近权重越大） | 类别名称 |
| `regression` | 加权平均（距离越近权重越大） | 预测数值 |
| `extraction` | 最近邻复制 | 最近邻的 PREDICT 值 |
| `anomaly` | 距离阈值判定 | `normal` 或 `anomaly` |

**在 PREDICT 列中使用（EMBEDDING AS 语法）**：

limix_infer 支持两种向量列语法：`EMBEDDING AS`（单列嵌入，生成隐藏的 `_embedding` 列）和 `EMBEDDINGS AS`（多列组合向量）。

```sql
-- 分类任务：使用 EMBEDDING AS 语法（单列嵌入）
CREATE TABLE limix_test_class (
    name text EMBEDDING AS (st_embedding(name)),
    category text PREDICT AS (limix_infer())
) WITH (predict_timing=immediate, limix_task='classification', limix_topn=3, vector_len=384);

-- 插入训练数据
INSERT INTO limix_test_class (name, category) VALUES ('apple', 'fruit');
INSERT INTO limix_test_class (name, category) VALUES ('banana', 'fruit');
INSERT INTO limix_test_class (name, category) VALUES ('dog', 'animal');

-- 插入新数据（自动推理，本地 k-NN，无需 LLM）
INSERT INTO limix_test_class (name) VALUES ('grape');
-- category 列自动推理为 'fruit'（grape 与 apple/banana 向量最近）
```

```sql
-- 回归任务
CREATE TABLE limix_test_reg (
    feature text EMBEDDING AS (st_embedding(feature)),
    value text PREDICT AS (limix_infer())
) WITH (predict_timing=immediate, limix_task='regression', limix_topn=3, vector_len=384);

INSERT INTO limix_test_reg (feature, value) VALUES ('one', '1');
INSERT INTO limix_test_reg (feature, value) VALUES ('five', '5');
INSERT INTO limix_test_reg (feature, value) VALUES ('ten', '10');
INSERT INTO limix_test_reg (feature, value) VALUES ('twenty', '20');
INSERT INTO limix_test_reg (feature) VALUES ('fifteen');
-- value 自动推理为 12.6212（介于 ten=10 和 twenty=20 之间的加权平均）
```

```sql
-- 异常检测任务
CREATE TABLE limix_test_anom (
    sensor text EMBEDDING AS (st_embedding(sensor)),
    status text PREDICT AS (limix_infer())
) WITH (predict_timing=immediate, limix_task='anomaly', limix_topn=3, vector_len=384);
```

```sql
-- 提取任务（最近邻复制，topn=1）
CREATE TABLE limix_test_ext (
    source text EMBEDDING AS (st_embedding(source)),
    target text PREDICT AS (limix_infer())
) WITH (predict_timing=immediate, limix_task='extraction', limix_topn=1, vector_len=384);
```

**直接调用 limix_infer（不通过 PREDICT 列）**：

```sql
-- 手动设置 GUC 参数后直接调用
SET jolix_predict.current_table='limix_test_class';
SET jolix_predict.limix_current_vector='[0.5,0.3,0.2]';
SELECT limix_infer();
-- 返回基于 k-NN 的推理结果
```

**边界行为**：
- 空表（无训练数据）：返回 NULL，输出 WARNING
- 未设置 `limix_task`：默认使用 `classification`
- 向量列为 NULL：跳过该行，不参与 k-NN 检索

### 3.3 llm_rag_infer 函数

RAG（检索增强生成）推理函数，自动使用当前表（包含 EMBEDDING 列）作为 RAG 检索表。先从当前表中检索相关文档，再结合检索结果调用 LLM。RAG 参数（`rag_similarity`、`rag_topn`）通过 `set_llm_config()` 配置。

```sql
-- 配置 RAG 参数
SELECT set_llm_config(
    p_api_url := 'https://api.openai.com/v1/chat/completions',
    p_api_key := 'sk-xxx',
    p_model_name := 'gpt-4',
    p_rag_similarity := 2.0,
    p_rag_topn := 3
);

-- RAG 推理（不带历史）
SELECT llm_rag_infer(
    'Answer questions based on the provided context.',  -- system_prompt
    'What is PostgreSQL?'                                -- user_input
);

-- RAG 推理（带2轮历史）
SELECT llm_rag_infer(
    'Answer questions based on the provided context.',  -- system_prompt
    'What is PostgreSQL?',                                -- user_input
    2                                                     -- history_count（可选，默认0）
);
```

**参数说明**：

| 参数 | 类型 | 必选 | 说明 |
|------|------|------|------|
| `system_prompt` | text | 是 | 系统提示词。传入 NULL 或空字符串时使用 `set_llm_config()` 配置的 `system_prompt` |
| `user_input` | text | 是 | 用户输入文本。传入 NULL 或空字符串时使用 `set_llm_config()` 配置的 `prompt_template` |
| `history_count` | integer | 否 | 历史对话轮数（默认0） |

**RAG 配置参数**（通过 `set_llm_config()` 设置）：

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `p_rag_similarity` | float8 | 0.5 | 余弦距离阈值（0.0-2.0，越小越相似） |
| `p_rag_topn` | integer | 5 | 检索结果数量 |

**RAG 检索流程**：
1. 自动获取当前表名（从 `jolix_predict.current_table` GUC 参数）
2. 使用 EMBEDDING 列的嵌入函数将 `user_input` 转换为向量
3. 在当前表中进行向量相似度搜索
4. 将检索结果作为上下文与用户输入合并
5. 调用 LLM 生成回答
6. 自动记录 Q&A 到历史表

**前提条件**：
- 当前表必须有 EMBEDDING 列
- EMBEDDING 列必须使用 `EMBEDDING AS (函数名(列名))` 语法指定嵌入函数
- 必须配置 `vector_len` 与嵌入函数输出维度匹配

**在 PREDICT 列中使用**：

```sql
-- 配置 LLM 和 RAG 参数
SELECT set_llm_config(
    p_api_url := 'https://api.openai.com/v1/chat/completions',
    p_api_key := 'sk-xxx',
    p_model_name := 'gpt-4',
    p_rag_similarity := 2.0,
    p_rag_topn := 3
);

-- 创建同时包含 EMBEDDING 列和 PREDICT 列的表
CREATE TABLE rag_knowledge (
    id serial PRIMARY KEY,
    content text EMBEDDING AS (st_embedding(content)),
    answer text PREDICT AS (llm_rag_infer(
        'Answer questions based on the provided context. Reply in one short sentence.',
        content
    ))
) WITH (
    predict_timing = immediate,
    vector_len = 384
);

-- 插入知识数据（自动生成 embedding）
INSERT INTO rag_knowledge (content) VALUES ('PostgreSQL is a powerful open source database system');
INSERT INTO rag_knowledge (content) VALUES ('Python is a popular programming language');

-- 插入问题（自动触发 RAG 推理）
INSERT INTO rag_knowledge (content) VALUES ('What is PostgreSQL?');
-- answer 列会自动填充 LLM 基于 RAG 上下文的回答
```

### 3.5 set_llm_config 函数

设置 LLM 配置，支持系统级和表级两种形式。

**系统级配置**：

```sql
SELECT set_llm_config(
    p_api_url text,           -- API 地址（必选）
    p_api_key text,           -- API 密钥（默认 ''）
    p_model_name text,        -- 模型名称（默认 'gpt-3.5-turbo'）
    p_temperature float8,     -- 温度参数（默认 0.7）
    p_max_tokens integer,     -- 最大 token 数（默认 1024）
    p_system_prompt text,     -- 系统提示词（默认 ''）
    p_history_count integer,  -- 历史轮数（默认 0）
    p_rag_table regclass,     -- RAG 表（默认 NULL）
    p_rag_similarity float8,  -- RAG 相似度阈值（默认 0.5）
    p_rag_topn integer        -- RAG 检索数量（默认 5）
);
```

**表级配置**（额外支持 `p_prompt_template`）：

```sql
SELECT set_llm_config(
    p_table_name regclass,    -- 表名（必选）
    p_api_url text,           -- API 地址（必选）
    p_api_key text,           -- API 密钥
    p_model_name text,        -- 模型名称
    p_temperature float8,     -- 温度参数
    p_max_tokens integer,     -- 最大 token 数
    p_system_prompt text,     -- 系统提示词
    p_prompt_template text,   -- 提示词模板（支持 {{column_name}} 语法）
    p_history_count integer,  -- 历史轮数
    p_rag_table regclass,     -- RAG 表
    p_rag_similarity float8,  -- RAG 相似度阈值
    p_rag_topn integer        -- RAG 检索数量
);
```

### 3.6 get_llm_config 函数

查看 LLM 配置。

```sql
-- 查看系统级配置
SELECT * FROM get_llm_config();

-- 查看表级配置（无表级配置时回退到系统级）
SELECT * FROM get_llm_config('my_table');
```

### 3.7 历史记录管理函数

```sql
-- 手动记录历史
SELECT record_predict_history('my_table', 'user', 'What is PostgreSQL?');
SELECT record_predict_history('my_table', 'assistant', 'PostgreSQL is a database.');

-- 清除指定表的历史
SELECT clear_predict_history('my_table');

-- 清除所有历史
SELECT clear_predict_history();

-- 手动清理过期历史记录（根据 history_retention_days 设置）
SELECT cleanup_predict_history();

-- 查看历史记录
SELECT table_name, role, content, created_at
FROM jolix_llm_history
WHERE table_name = 'my_table'
ORDER BY created_at DESC
LIMIT 10;
```

### 3.8 历史记录过期清理

历史记录表 `jolix_llm_history` 会随着使用不断增长。为防止表过大，系统提供了自动过期清理功能：

**GUC 参数**：`jolix_predict.history_retention_days`（默认 7 天）

```sql
LOAD 'jolix_predict';

-- 查看当前保留天数
SHOW jolix_predict.history_retention_days;

-- 设置保留 30 天
SET jolix_predict.history_retention_days = 30;

-- 设置永不过期
SET jolix_predict.history_retention_days = 0;
```

**自动清理**：每次记录新的历史时，系统会自动检查并清理过期记录（间隔至少 1 小时执行一次，避免频繁清理影响性能）。

**手动清理**：也可以随时手动触发清理：

```sql
-- 清理过期记录，返回删除的行数
SELECT cleanup_predict_history();
```

## 4. PREDICT 列使用

### 4.1 基本用法

```sql
-- 创建带 PREDICT 列的表
CREATE TABLE qa_table (
    id serial PRIMARY KEY,
    question text,
    answer text PREDICT AS (llm_infer(
        'Answer the question concisely.',
        question
    ))
) WITH (predict_timing = immediate);

-- 插入数据时自动推理
INSERT INTO qa_table (question) VALUES ('What is PostgreSQL?');
-- answer 列自动填充 LLM 回答
```

### 4.2 带历史记录的 PREDICT 列

```sql
CREATE TABLE chat_table (
    id serial PRIMARY KEY,
    question text,
    answer text PREDICT AS (llm_infer(
        'You are a helpful assistant.',
        question,
        3,
        'chat_table'
    ))
) WITH (predict_timing = immediate);
```

### 4.3 RAG 增强的 PREDICT 列

```sql
SELECT set_llm_config(
    p_api_url := 'https://api.openai.com/v1/chat/completions',
    p_api_key := 'sk-xxx',
    p_model_name := 'gpt-4',
    p_rag_similarity := 2.0,
    p_rag_topn := 3
);

CREATE TABLE knowledge_qa (
    id serial PRIMARY KEY,
    content text EMBEDDING AS (st_embedding(content)),
    answer text PREDICT AS (llm_rag_infer(
        'Answer based on context. Reply concisely.',
        content,
        2
    ))
) WITH (
    predict_timing = immediate,
    vector_len = 384
);
```

### 4.4 LLM 文本分类

使用 `llm_infer` 函数实现 LLM 文本分类，自动对列内容进行分类标注。

**配置说明**：使用 `set_llm_config()` 配置 LLM API 参数，配置保存在 `jolix_llm_config` 表中，触发器中执行推理时会自动读取。

```sql
-- 1. 配置 LLM API
SELECT set_llm_config(
    p_api_url := 'https://ark.cn-beijing.volces.com/api/v3/chat/completions',
    p_api_key := '<your-api-key>',
    p_model_name := '<your-model-name>',
    p_temperature := 0.3,
    p_max_tokens := 64
);

-- 2. 创建表，PREDICT 表达式引用多个列
CREATE TABLE test_llm_articles (
    id serial PRIMARY KEY,
    title text,
    content text,
    category text PREDICT AS (llm_infer(
        'Classify the following text into exactly one category: technology, sports, politics, entertainment. Reply with only the category name, nothing else.',
        'Classify this text: ' || title || '. ' || content
    ))
) WITH (predict_timing=immediate);

-- 3. INSERT 不提供 category - LLM 自动分类
INSERT INTO test_llm_articles (title, content) VALUES ('AI Revolution', 'AI and machine learning are transforming software development');

-- 4. INSERT 提供 category - 保存为实际值
INSERT INTO test_llm_articles (title, content, category) VALUES ('Box Office Hit', 'The new movie broke box office records', 'entertainment');
```

**更多分类场景**：

```sql
-- 情感分析（引用单列）
CREATE TABLE reviews (
    id serial PRIMARY KEY,
    review_text text,
    sentiment text PREDICT AS (llm_infer(
        'Classify the sentiment. Reply with exactly one word: positive, negative, or neutral.',
        review_text
    ))
) WITH (predict_timing=immediate);

-- 优先级分类（引用多列：产品名 + 描述）
CREATE TABLE support_tickets (
    id serial PRIMARY KEY,
    product text,
    description text,
    priority text PREDICT AS (llm_infer(
        'Classify the priority. Reply with exactly one word: critical, high, medium, or low.',
        'Product: ' || product || '. Issue: ' || description
    ))
) WITH (predict_timing=immediate);

-- 垃圾邮件检测（引用多列：主题 + 正文）
CREATE TABLE emails (
    id serial PRIMARY KEY,
    sender text,
    subject text,
    body text,
    is_spam text PREDICT AS (llm_infer(
        'Determine if this email is spam. Reply with exactly one word: spam or not_spam.',
        'From: ' || sender || '. Subject: ' || subject || '. Body: ' || body
    ))
) WITH (predict_timing=immediate);
```

### 4.5 predict_timing 选项

| 值 | 说明 |
|------|------|
| `immediate` | 插入数据时立即执行推理 |
| `async` | 异步执行推理（后台进程处理） |

## 5. EMBEDDINGS 列使用

### 5.1 概述

EMBEDDINGS 功能允许将表中多个列的值组合为向量表示，支持基于 FT-Transformer 等模型的语义相似性查询。

### 5.2 列级语法：EMBEDDINGS AS

```sql
-- 基本用法
CREATE TABLE customers (
    id int PRIMARY KEY,
    age int,
    income float,
    category text,
    demographic EMBEDDINGS AS (ft_transformer_embedding(age, income, category))
);

-- 指定向量维度（需与模型输出维度匹配）
CREATE TABLE customers (
    id int PRIMARY KEY,
    age int,
    income float,
    category text,
    demographic EMBEDDINGS AS (ft_transformer_embedding(age, income, category))
) WITH (vector_len = 384);

-- 多组 EMBEDDINGS
CREATE TABLE products (
    id int PRIMARY KEY,
    name text,
    price float,
    brand text,
    basic_features EMBEDDINGS AS (ft_transformer_embedding(price, brand)),
    sales int,
    rating float,
    performance EMBEDDINGS AS (ft_transformer_embedding(sales, rating))
) WITH (vector_len = 384);
```

**语法说明**：
- `demographic`：向量化名称，同时作为隐藏向量列的列名
- `EMBEDDINGS AS`：关键字
- `ft_transformer_embedding(age, income, category)`：向量化表达式，引用同表其他列
- `STORED`：可选关键字（默认行为），表示物理存储

### 5.3 表级语法：CREATE EMBEDDINGS

```sql
-- 先建表
CREATE TABLE customers (
    id SERIAL PRIMARY KEY,
    age INTEGER,
    income FLOAT8,
    category TEXT
);

-- 添加向量化
CREATE EMBEDDINGS demographic ON customers
    USING ft_transformer_embedding (age, income, category)
    WITH (vector_len = 384);

-- 删除向量化
DROP EMBEDDINGS demographic ON customers;
```

### 5.4 向量相似性查询

```sql
-- 方式一：子查询（查找与某行最相似的行）
SELECT id, age, income, category,
       demographic <=> (SELECT demographic FROM customers WHERE id = 1) AS cosine_dist
FROM customers
WHERE id != 1
ORDER BY cosine_dist;

-- 方式二：函数调用（实时计算查询向量，需要显式类型转换）
SELECT id, age, income, category,
       demographic <=> ft_transformer_embedding(30::int, 50000.0::float8, 'premium'::text) AS query_dist
FROM customers
ORDER BY query_dist
LIMIT 3;
```

### 5.5 ft_transformer_embedding 函数

`ft_transformer_embedding` 接受可变参数（VARIADIC "any"），将多列值拼接后通过 SentenceTransformer 模型生成向量。

**默认模型**：`sentence-transformers/all-MiniLM-L6-v2`（输出 384 维向量）

**GUC 参数**：

| 参数 | 类型 | 默认值 | 作用域 | 说明 |
|------|------|--------|--------|------|
| `jolix_embedding.model_name` | string | `sentence-transformers/all-MiniLM-L6-v2` | USERSET | st_embedding 默认模型 |
| `jolix_embedding.model_path` | string | `/usr/local/pgsql/models` | SIGHUP | 模型缓存目录 |
| `jolix_embedding.ft_model_name` | string | `sentence-transformers/all-MiniLM-L6-v2` | USERSET | ft_transformer_embedding 默认模型 |
| `jolix_embedding.ft_vector_len` | integer | `384` | USERSET | 模型不可用时的降级向量维度 |

**切换模型**：

```sql
-- 切换为 768 维模型
SET jolix_embedding.ft_model_name = 'sentence-transformers/all-mpnet-base-v2';
-- 建表时需指定 vector_len = 768
```

**降级机制**：当模型加载失败时（如网络不可达），函数自动降级为基于列值哈希的确定性向量生成，不会报错。降级向量维度由 `jolix_embedding.ft_vector_len` 控制。

## 6. 配置表结构

### 6.1 jolix_llm_config

| 字段 | 类型 | 说明 |
|------|------|------|
| `id` | serial | 主键 |
| `scope` | text | 配置范围（`system` 或 `table`） |
| `relid` | oid | 表 OID（表级配置时使用） |
| `api_url` | text | API 地址 |
| `api_key` | text | API 密钥 |
| `model_name` | text | 模型名称 |
| `temperature` | float8 | 温度参数 |
| `max_tokens` | integer | 最大 token 数 |
| `system_prompt` | text | 系统提示词 |
| `prompt_template` | text | 提示词模板 |
| `history_count` | integer | 历史轮数 |
| `rag_table` | oid | RAG 表 OID |
| `rag_similarity` | float8 | RAG 相似度阈值 |
| `rag_topn` | integer | RAG 检索数量 |

### 6.2 jolix_llm_history

| 字段 | 类型 | 说明 |
|------|------|------|
| `id` | serial | 主键 |
| `table_name` | text | 关联的表名 |
| `role` | text | 角色（`user` 或 `assistant`） |
| `content` | text | 内容 |
| `created_at` | timestamptz | 创建时间 |

## 7. 表选项

| 选项 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `predict_timing` | enum | - | 推理时机：`immediate` 或 `async` |
| `vector_len` | integer | 384 | 向量维度（需与嵌入函数输出匹配） |
| `vector_index` | enum | hnsw | 向量索引类型（ivfflat/hnsw） |
| `vector_distance` | enum | vector_cosine_ops | 向量距离类型 |
| `limix_model` | text | `limix-2m` | limix_infer 本地推理模型名称（预留） |
| `limix_task` | text | `classification` | limix_infer 任务类别：`classification`/`regression`/`extraction`/`anomaly` |
| `limix_topn` | integer | `5` | limix_infer 检索的相似行数量（k-NN 的 k 值） |

## 8. GUC 参数

### 8.1 jolix_predict 参数

| 参数 | 类型 | 说明 |
|------|------|------|
| `jolix_predict.llm_api_url` | string | API 地址 |
| `jolix_predict.llm_api_key` | string | API 密钥 |
| `jolix_predict.llm_model` | string | 模型名称 |
| `jolix_predict.llm_timeout` | integer | 请求超时（秒） |
| `jolix_predict.llm_history_table` | string | 默认历史记录表名（默认default，空字符串禁用自动记录） |
| `jolix_predict.current_table` | string | 当前表名（内部使用，支持 schema 限定名） |
| `jolix_predict.limix_current_vector` | string | limix_infer 搜索向量（内部使用，由 PREDICT 触发器自动设置） |
| `jolix_predict.history_retention_days` | integer | 历史记录保留天数（默认7，0=永不过期） |

### 8.2 jolix_embedding 参数

| 参数 | 类型 | 默认值 | 作用域 | 说明 |
|------|------|--------|--------|------|
| `jolix_embedding.model_name` | string | `sentence-transformers/all-MiniLM-L6-v2` | USERSET | st_embedding 默认模型 |
| `jolix_embedding.model_path` | string | `/usr/local/pgsql/models` | SIGHUP | 模型缓存目录 |
| `jolix_embedding.ft_model_name` | string | `sentence-transformers/all-MiniLM-L6-v2` | USERSET | ft_transformer_embedding 默认模型 |
| `jolix_embedding.ft_vector_len` | integer | `384` | USERSET | 模型不可用时的降级向量维度 |

## 9. 常见问题

### Q: LLM 推理超时怎么办？

```sql
SET jolix_predict.llm_timeout = 300;  -- 设置为 300 秒
```

### Q: 如何查看历史记录？

```sql
SELECT * FROM jolix_llm_history WHERE table_name = 'my_table' ORDER BY created_at DESC;
```

### Q: llm_rag_infer 报错 "no RAG table available"？

确保：
1. 在 PREDICT 列表达式中使用 `llm_rag_infer`
2. 当前表有 EMBEDDING 列
3. EMBEDDING 列使用 `EMBEDDING AS (函数名(列名))` 语法

### Q: embedding 列为空？

确保建表时使用 `EMBEDDING AS` 语法指定嵌入函数：

```sql
CREATE TABLE my_table (
    content text EMBEDDING AS (st_embedding(content))
) WITH (vector_len = 384);
```

### Q: ft_transformer_embedding 向量维度不匹配？

`vector_len` 必须与模型输出维度匹配：
- `all-MiniLM-L6-v2`：384 维（默认）
- `all-mpnet-base-v2`：768 维

```sql
-- 使用默认模型（384维）
CREATE TABLE t1 (...,
    vec EMBEDDINGS AS (ft_transformer_embedding(col1, col2))
);  -- vector_len 默认 384

-- 使用 768 维模型
SET jolix_embedding.ft_model_name = 'sentence-transformers/all-mpnet-base-v2';
CREATE TABLE t2 (...,
    vec EMBEDDINGS AS (ft_transformer_embedding(col1, col2))
) WITH (vector_len = 768);
```

### Q: 如何从 Windows 客户端连接 WSL 中的 PostgreSQL？

1. 确保 `postgresql.conf` 中 `listen_addresses = '*'`
2. 确保 `pg_hba.conf` 中添加了 `host all all 0.0.0.0/0 md5`
3. 设置 postgres 用户密码：`ALTER USER postgres WITH PASSWORD 'PG';`
4. 获取 WSL IP：`wsl -d Ubuntu-22.04 -- bash -c "hostname -I"`
5. 使用该 IP 和端口 5432 连接
