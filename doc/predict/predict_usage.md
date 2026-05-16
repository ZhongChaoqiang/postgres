# Jolix Predict 使用文档

**最后更新**: 2026-05-16

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
SET jolix_predict.api_url = 'https://api.openai.com/v1/chat/completions';
SET jolix_predict.api_key = 'sk-xxx';
SET jolix_predict.model = 'gpt-4';
SET jolix_predict.timeout = 300;
```

## 3. 函数说明

### 3.1 llm_infer 函数

调用 LLM API 进行推理，支持三种重载形式：

```sql
-- 基本调用（无历史）
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
| `system_prompt` | text | 是 | 系统提示词 |
| `user_input` | text | 是 | 用户输入文本 |
| `history_count` | integer | 否 | 历史对话轮数（默认0） |
| `table_name` | text | 否 | 历史记录关联的表名 |

**说明**：
- 当 `history_count > 0` 时，从 `jolix_predict_history` 表读取最近的 N 轮 Q&A 对话作为上下文
- 推理完成后自动将 Q&A 记录到历史表
- 内容超过 4096 字符会自动截断

### 3.2 llm_rag_infer 函数

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
| `system_prompt` | text | 是 | 系统提示词 |
| `user_input` | text | 是 | 用户输入文本 |
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
- EMBEDDING 列必须使用 `EMBEDDING AS (函数名(列名)) STORED` 语法指定嵌入函数
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
    content text EMBEDDING AS (st_embedding(content)) STORED,
    answer text PREDICT AS (llm_rag_infer(
        'Answer questions based on the provided context. Reply in one short sentence.',
        content
    )) STORED
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

### 3.3 set_llm_config 函数

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

### 3.4 get_llm_config 函数

查看 LLM 配置。

```sql
-- 查看系统级配置
SELECT * FROM get_llm_config();

-- 查看表级配置（无表级配置时回退到系统级）
SELECT * FROM get_llm_config('my_table');
```

### 3.5 历史记录管理函数

```sql
-- 手动记录历史
SELECT record_predict_history('my_table', 'user', 'What is PostgreSQL?');
SELECT record_predict_history('my_table', 'assistant', 'PostgreSQL is a database.');

-- 清除指定表的历史
SELECT clear_predict_history('my_table');

-- 清除所有历史
SELECT clear_predict_history();

-- 查看历史记录
SELECT table_name, role, content, created_at
FROM jolix_predict_history
WHERE table_name = 'my_table'
ORDER BY created_at DESC
LIMIT 10;
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
    )) STORED
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
    )) STORED
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
    content text EMBEDDING AS (st_embedding(content)) STORED,
    answer text PREDICT AS (llm_rag_infer(
        'Answer based on context. Reply concisely.',
        content,
        2
    )) STORED
) WITH (
    predict_timing = immediate,
    vector_len = 384
);
```

### 4.4 predict_timing 选项

| 值 | 说明 |
|------|------|
| `immediate` | 插入数据时立即执行推理 |
| `async` | 异步执行推理（后台进程处理） |

## 5. 配置表结构

### 5.1 jolix_predict_config

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

### 5.2 jolix_predict_history

| 字段 | 类型 | 说明 |
|------|------|------|
| `id` | serial | 主键 |
| `table_name` | text | 关联的表名 |
| `role` | text | 角色（`user` 或 `assistant`） |
| `content` | text | 内容 |
| `created_at` | timestamptz | 创建时间 |

## 6. 表选项

| 选项 | 类型 | 说明 |
|------|------|------|
| `predict_timing` | enum | 推理时机：`immediate` 或 `async` |
| `vector_len` | integer | 向量维度（需与嵌入函数输出匹配） |

## 7. GUC 参数

| 参数 | 类型 | 说明 |
|------|------|------|
| `jolix_predict.api_url` | string | API 地址 |
| `jolix_predict.api_key` | string | API 密钥 |
| `jolix_predict.model` | string | 模型名称 |
| `jolix_predict.timeout` | integer | 请求超时（秒） |
| `jolix_predict.current_table` | string | 当前表名（内部使用） |

## 8. 常见问题

### Q: LLM 推理超时怎么办？

```sql
SET jolix_predict.timeout = 300;  -- 设置为 300 秒
```

### Q: 如何查看历史记录？

```sql
SELECT * FROM jolix_predict_history WHERE table_name = 'my_table' ORDER BY created_at DESC;
```

### Q: llm_rag_infer 报错 "no RAG table available"？

确保：
1. 在 PREDICT 列表达式中使用 `llm_rag_infer`
2. 当前表有 EMBEDDING 列
3. EMBEDDING 列使用 `EMBEDDING AS (函数名(列名)) STORED` 语法

### Q: embedding 列为空？

确保建表时使用 `EMBEDDING AS` 语法指定嵌入函数：

```sql
CREATE TABLE my_table (
    content text EMBEDDING AS (st_embedding(content)) STORED
) WITH (vector_len = 384);
```
