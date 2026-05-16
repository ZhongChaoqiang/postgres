# PREDICT 功能测试报告

**测试日期**: 2026-05-16
**测试环境**: WSL Ubuntu, PostgreSQL 17 + jolix_predict + jolix_embedding
**LLM API**: https://ark.cn-beijing.volces.com/api/v3/chat/completions

## 测试结果概要

| 测试项 | 结果 |
|--------|------|
| 扩展安装与 GUC 参数 | ✅ 通过 |
| LLM 配置函数 | ✅ 通过 |
| llm_infer 基本推理 | ✅ 通过 |
| llm_infer 带历史记录推理 | ✅ 通过 |
| PREDICT 列推理 | ✅ 通过 |
| llm_rag_infer RAG 推理 | ✅ 通过 |
| 历史记录管理 | ✅ 通过 |
| Embedding 功能 | ✅ 通过 |
| RAG + Embedding + Predict 集成 | ✅ 通过 |

## 1. 基础功能测试

### 1.1 扩展安装验证

```sql
SELECT extname, extversion FROM pg_extension WHERE extname = 'jolix_predict';
```

**结果**: ✅ 返回 `jolix_predict | 1.0`

### 1.2 配置表验证

```sql
SELECT table_name FROM information_schema.tables
WHERE table_name IN ('jolix_predict_config', 'jolix_predict_history')
ORDER BY table_name;
```

**结果**: ✅ 返回 `jolix_predict_config` 和 `jolix_predict_history`

### 1.3 GUC 参数验证

```sql
SHOW jolix_predict.api_url;
SHOW jolix_predict.api_key;
SHOW jolix_predict.model;
SHOW jolix_predict.timeout;
```

**结果**: ✅ 所有参数均可访问

### 1.4 函数签名验证

```sql
-- 验证当前函数存在
SELECT proname FROM pg_proc WHERE proname LIKE 'llm_%' ORDER BY proname;
```

**结果**: ✅ 返回 `llm_infer`(3个重载)、`llm_rag_infer`

```sql
-- 验证已删除函数不存在
SELECT count(*) FROM pg_proc WHERE proname IN ('llm_predict_ext', 'llm_rag_predict_ext', 'set_predict_config', 'get_predict_config');
```

**结果**: ✅ 返回 0

```sql
-- 验证配置函数
SELECT count(*) FROM pg_proc WHERE proname IN ('set_llm_config', 'get_llm_config');
```

**结果**: ✅ 返回 4（系统级+表级各2个）

## 2. LLM 配置测试

### 2.1 系统级配置

```sql
SELECT set_llm_config(
    p_api_url := 'https://ark.cn-beijing.volces.com/api/v3/chat/completions',
    p_api_key := 'acc96ba1-d743-45d7-9b0e-a415bd96a046',
    p_model_name := 'ep-20251128103853-pp9jw',
    p_temperature := 0.7,
    p_max_tokens := 1024,
    p_system_prompt := 'You are a helpful assistant.',
    p_rag_similarity := 2.0,
    p_rag_topn := 3
);
```

**结果**: ✅ 配置成功

### 2.2 查看系统级配置

```sql
SELECT api_url, model_name, rag_similarity, rag_topn FROM get_llm_config();
```

**结果**: ✅ 返回配置的值（rag_similarity=2.0, rag_topn=3）

## 3. LLM 推理测试

### 3.1 基本推理（2参数）

```sql
SET jolix_predict.timeout = 300;

SELECT llm_infer(
    'You are a helpful assistant. Reply in one short sentence.',
    'What is PostgreSQL?'
);
```

**结果**: ✅ 返回 LLM 生成的回答："PostgreSQL is a free, open-source object-relational database management system..."

### 3.2 带历史记录推理（3参数）

```sql
SELECT clear_predict_history();

SELECT llm_infer(
    'You are a helpful assistant. Reply concisely.',
    'What is PostgreSQL?',
    2
);
```

**结果**: ✅ 返回 LLM 回答，历史记录自动保存

### 3.3 带历史记录和表名推理（4参数）

```sql
SELECT llm_infer(
    'You are a helpful assistant. Reply concisely.',
    'Tell me more about its features.',
    2,
    'test_infer'
);
```

**结果**: ✅ 返回 LLM 回答，能参考之前的对话上下文

## 4. PREDICT 列测试

### 4.1 基本 PREDICT 列

```sql
CREATE TABLE qa_test (
    id serial PRIMARY KEY,
    question text,
    answer text PREDICT AS (llm_infer(
        'Answer the question in one short sentence.',
        question
    )) STORED
) WITH (predict_timing = immediate);

INSERT INTO qa_test (question) VALUES ('What is Python?');
SELECT id, question, answer FROM qa_test;
```

**结果**: ✅ answer 列自动填充 LLM 回答

## 5. RAG 推理测试

### 5.1 创建 RAG 表（EMBEDDING AS 语法）

```sql
SELECT set_llm_config(
    p_api_url := 'https://ark.cn-beijing.volces.com/api/v3/chat/completions',
    p_api_key := 'acc96ba1-d743-45d7-9b0e-a415bd96a046',
    p_model_name := 'ep-20251128103853-pp9jw',
    p_rag_similarity := 2.0,
    p_rag_topn := 3
);

CREATE TABLE rag_test (
    id serial PRIMARY KEY,
    content text EMBEDDING AS (simple_embedding(content)) STORED,
    answer text PREDICT AS (llm_rag_infer(
        'Answer questions based on the provided context. Reply in one short sentence.',
        content
    )) STORED
) WITH (predict_timing = immediate, vector_len = 3);
```

**结果**: ✅ 表创建成功

### 5.2 插入知识数据

```sql
INSERT INTO rag_test (content) VALUES ('PostgreSQL is a powerful open source database system');
INSERT INTO rag_test (content) VALUES ('Python is a popular programming language for data science');
INSERT INTO rag_test (content) VALUES ('Machine learning models can be trained on large datasets');

SELECT id, content, content_embedding FROM rag_test;
```

**结果**: ✅ embedding 列自动填充向量数据

### 5.3 RAG 推理

```sql
INSERT INTO rag_test (content) VALUES ('What is PostgreSQL?');
SELECT id, content, answer FROM rag_test WHERE content = 'What is PostgreSQL?';
```

**结果**: ✅ answer 列基于 RAG 上下文生成回答，包含 PostgreSQL 相关信息

## 6. 历史记录测试

### 6.1 自动记录验证

```sql
SELECT table_name, role, substring(content, 1, 60) as content_preview
FROM jolix_predict_history
ORDER BY created_at DESC LIMIT 10;
```

**结果**: ✅ 每次推理后自动记录 user 和 assistant 两条记录

### 6.2 手动记录

```sql
SELECT record_predict_history('manual_test', 'user', 'Test question');
SELECT record_predict_history('manual_test', 'assistant', 'Test answer');

SELECT * FROM jolix_predict_history WHERE table_name = 'manual_test';
```

**结果**: ✅ 手动记录成功

### 6.3 清除历史

```sql
SELECT clear_predict_history('manual_test');
SELECT clear_predict_history();
```

**结果**: ✅ 清除成功，返回删除行数

## 7. Embedding 功能测试

### 7.1 simple_embedding 函数

```sql
SELECT simple_embedding('hello world');
```

**结果**: ✅ 返回 3 维向量

### 7.2 st_embedding 函数

```sql
SELECT substring(st_embedding('hello world')::text, 1, 50);
```

**结果**: ✅ 返回 384 维向量

### 7.3 EMBEDDING AS 语法（simple_embedding）

```sql
CREATE TABLE emb_test (
    id serial PRIMARY KEY,
    content text EMBEDDING AS (simple_embedding(content)) STORED
) WITH (vector_len = 3);

INSERT INTO emb_test (content) VALUES ('Test embedding');
INSERT INTO emb_test (content) VALUES ('Another test');

SELECT id, content, content_embedding FROM emb_test;
```

**结果**: ✅ EMBEDDING AS 语法正常工作，embedding 自动生成

### 7.4 EMBEDDING AS 语法（st_embedding）

```sql
CREATE TABLE emb_test2 (
    id serial PRIMARY KEY,
    content text EMBEDDING AS (st_embedding(content)) STORED
) WITH (vector_len = 384);

INSERT INTO emb_test2 (content) VALUES ('PostgreSQL is a database');
SELECT id, content, substring(content_embedding::text, 1, 50) FROM emb_test2;
```

**结果**: ✅ st_embedding 在 EMBEDDING AS 语法中正常工作

## 8. RAG + Embedding + Predict 集成测试

### 8.1 完整集成测试

```sql
SELECT set_llm_config(
    p_api_url := 'https://ark.cn-beijing.volces.com/api/v3/chat/completions',
    p_api_key := 'acc96ba1-d743-45d7-9b0e-a415bd96a046',
    p_model_name := 'ep-20251128103853-pp9jw',
    p_rag_similarity := 2.0,
    p_rag_topn := 3
);

SET jolix_predict.timeout = 300;
SELECT clear_predict_history();

CREATE TABLE rag_integration (
    id serial PRIMARY KEY,
    content text EMBEDDING AS (simple_embedding(content)) STORED,
    answer text PREDICT AS (llm_rag_infer(
        'Answer questions based on the provided context. Reply in one short sentence.',
        content
    )) STORED
) WITH (predict_timing = immediate, vector_len = 3);

INSERT INTO rag_integration (content) VALUES ('PostgreSQL is a powerful open source database system');
INSERT INTO rag_integration (content) VALUES ('Python is a popular programming language for data science');

INSERT INTO rag_integration (content) VALUES ('What is PostgreSQL?');
SELECT id, content, answer FROM rag_integration WHERE content = 'What is PostgreSQL?';
```

**结果**: ✅ 完整流程通过：
- EMBEDDING 列自动生成向量
- PREDICT 列自动触发 RAG 推理
- RAG 检索到相关上下文
- LLM 基于上下文生成正确回答
- 历史记录自动保存

### 8.2 历史记录验证

```sql
SELECT table_name, role, substring(content, 1, 60) as content_preview
FROM jolix_predict_history
WHERE table_name = 'rag_integration'
ORDER BY created_at;
```

**结果**: ✅ 6 条记录（3 条 user + 3 条 assistant），对应 3 次 INSERT 操作

## 已知限制

1. **st_embedding 在触发器中的稳定性**: `st_embedding` 函数调用 Python 模型，在触发器上下文中可能导致超时或崩溃。建议在触发器中使用 `simple_embedding` 或其他轻量级嵌入函数，`st_embedding` 适合在 INSERT 后手动 UPDATE 触发。
2. **vector_len 必须匹配**: 建表时 `vector_len` 必须与嵌入函数输出维度一致，否则 INSERT 会失败。
