# RAG 推理函数测试报告

## 1. 测试概述

| 项目 | 说明 |
|------|------|
| 测试日期 | 2026-05-06 |
| 测试环境 | WSL Ubuntu, PostgreSQL 17 + pg_predict + pg_embedding_st + pgvector |
| LLM 服务 | 火山引擎 Ark 平台（OpenAI 兼容 API） |
| LLM 模型 | ep-20251128103853-pp9jw |
| API 地址 | https://ark.cn-beijing.volces.com/api/v3/chat/completions |
| Embedding 模型 | sentence-transformers/all-MiniLM-L6-v2 (384维) |
| 测试范围 | llm_infer, llm_rag_infer, llm_rag_predict, 类型转换, 异常处理 |

## 2. 测试环境准备

### 2.1 安装扩展

```sql
CREATE EXTENSION pg_predict;
CREATE EXTENSION vector;
CREATE EXTENSION pg_embedding_st;
```

### 2.2 创建 RAG 知识库表

```sql
CREATE TABLE rag_examples (
    id SERIAL PRIMARY KEY,
    question TEXT,
    answer TEXT,
    tables TEXT,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'sentence_transformers_embedding',
    vector_len = 384
);

INSERT INTO rag_examples (question, answer, tables, content) VALUES
('本周的生产情况',
 'SELECT wd.record_date AS 统计日期, wd.produce_quantity AS 产出数量, wd.unqualified_quantity AS 不良数量 FROM dfs_metrics_work_order_daily wd WHERE WEEK(wd.record_date) = WEEK(CURDATE()) AND YEAR(wd.record_date) = YEAR(CURDATE());',
 'dfs_metrics_work_order_daily',
 '本周的生产情况'),
('上周三的生产情况是怎样的',
 'SELECT wd.work_order_number AS 工单号, wd.produce_quantity AS 产出数量 FROM dfs_metrics_work_order_daily wd WHERE DATE(wd.record_date) = DATE_SUB(CURDATE(), INTERVAL WEEKDAY(CURDATE()) + 3 DAY);',
 'dfs_metrics_work_order_daily',
 '上周三的生产情况是怎样的'),
('昨天生产情况',
 'SELECT wd.record_date AS 统计日期, wd.produce_quantity AS 产出数量 FROM dfs_metrics_work_order_daily wd WHERE DATE(wd.record_date) = DATE_SUB(CURDATE(), INTERVAL 1 DAY);',
 'dfs_metrics_work_order_daily',
 '昨天生产情况'),
('上周每天的生产情况分布',
 'SELECT DATE(wd.record_date) AS 统计日期, SUM(wd.produce_quantity) AS 产出数量 FROM dfs_metrics_work_order_daily wd WHERE YEARWEEK(wd.record_date) = YEARWEEK(CURDATE() - INTERVAL 1 WEEK) GROUP BY DATE(wd.record_date) ORDER BY 统计日期;',
 'dfs_metrics_work_order_daily',
 '上周每天的生产情况分布'),
('本月不良率最高的产线',
 'SELECT wd.line_name AS 产线名称, SUM(wd.unqualified_quantity)*100.0/NULLIF(SUM(wd.produce_quantity),0) AS 不良率 FROM dfs_metrics_work_order_daily wd WHERE MONTH(wd.record_date) = MONTH(CURDATE()) AND YEAR(wd.record_date) = YEAR(CURDATE()) GROUP BY wd.line_name ORDER BY 不良率 DESC LIMIT 1;',
 'dfs_metrics_work_order_daily',
 '本月不良率最高的产线');
```

### 2.3 配置 LLM API

```sql
SET pg_predict.api_url = 'https://ark.cn-beijing.volces.com/api/v3/chat/completions';
SET pg_predict.api_key = 'acc96ba1-d743-45d7-9b0e-a415bd96a046';
SET pg_predict.model = 'ep-20251128103853-pp9jw';
SET pg_predict.temperature = 0.3;
SET pg_predict.max_tokens = 512;
```

## 3. 测试用例与结果

### TC1: llm_infer 基础推理

| 项目 | 说明 |
|------|------|
| 测试目标 | 验证 llm_infer 基本两参数调用 |
| 测试函数 | `llm_infer(system_prompt, user_input)` |
| 预期结果 | LLM 返回正确回答 |

**测试 SQL**：
```sql
SELECT llm_infer(
    'You are a helpful assistant. Reply in one short sentence.',
    'What is the capital of France?'
);
```

**实际输出**：
```
The capital of France is Paris.
```

| 结果 | ✅ 通过 |
|------|--------|

---

### TC2: llm_infer 带历史对话参数

| 项目 | 说明 |
|------|------|
| 测试目标 | 验证 llm_infer 三参数调用（含 history_count） |
| 测试函数 | `llm_infer(system_prompt, history_count, user_input)` |
| 预期结果 | LLM 返回正确回答 |

**测试 SQL**：
```sql
SELECT llm_infer(
    'You are a helpful assistant. Keep answers very brief.',
    0,
    'What is the largest planet in the solar system?'
);
```

**实际输出**：
```
The largest planet in the solar system is **Jupiter** (a gas giant, with the greatest diameter and mass among all planets).
```

| 结果 | ✅ 通过 |
|------|--------|

---

### TC3: llm_rag_infer RAG 推理

| 项目 | 说明 |
|------|------|
| 测试目标 | 验证 RAG 向量检索 + LLM 推理的完整流程 |
| 测试函数 | `llm_rag_infer(system_prompt, history_count, user_input, rag_table, rag_similarity, rag_topn)` |
| 预期结果 | LLM 基于 RAG 检索到的相似示例，生成正确的 SQL |

**测试 SQL**：
```sql
SELECT llm_rag_infer(
    '你是一个数据分析SQL生成助手。根据检索到的示例，生成对应的MySQL查询SQL。只输出SQL，不要包含任何解释。',
    0,
    '上周生产情况',
    'rag_examples'::regclass,
    0.5,
    3
);
```

**RAG 检索过程**：
1. 调用 `sentence_transformers_embedding('上周生产情况')` 生成查询向量
2. 执行 `SELECT * FROM rag_examples ORDER BY content_embedding <=> $1::vector LIMIT 3`
3. 检索到最相似的3条示例（如"上周每天的生产情况分布"、"上周三的生产情况"等）
4. 格式化为上下文注入到用户输入之前

**实际输出**：
```sql
SELECT DATE(wd.record_date) AS 统计日期, SUM(wd.produce_quantity) AS 产出数量
FROM dfs_metrics_work_order_daily wd
WHERE YEARWEEK(wd.record_date) = YEARWEEK(CURDATE() - INTERVAL 1 WEEK)
GROUP BY DATE(wd.record_date)
ORDER BY 统计日期;
```

| 结果 | ✅ 通过 |
|------|--------|
| 验证点 | RAG 检索正确命中"上周"相关示例；LLM 生成的 SQL 使用了 `YEARWEEK` 过滤上周数据 |

---

### TC4: llm_rag_predict PREDICT 列自动推理

| 项目 | 说明 |
|------|------|
| 测试目标 | 验证通过 PREDICT 列 + predict_trigger 自动调用 llm_rag_predict |
| 测试函数 | `llm_rag_predict(input_row)` (由 predict_trigger 自动调用) |
| 预期结果 | INSERT 时 sql_result 列自动填充 LLM 生成的 SQL |

**测试 SQL**：
```sql
CREATE TABLE production_qa (
    id SERIAL PRIMARY KEY,
    question TEXT,
    sql_result TEXT PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'llm_rag_predict'
);

SELECT set_predict_config(
    'production_qa'::regclass,
    'https://ark.cn-beijing.volces.com/api/v3/chat/completions',
    'acc96ba1-d743-45d7-9b0e-a415bd96a046',
    'ep-20251128103853-pp9jw',
    0.3, 512,
    '你是一个数据分析SQL生成助手。根据检索到的示例，生成对应的MySQL查询SQL。只输出SQL，不要包含任何解释。',
    '{{question}}',
    0,
    'rag_examples'::regclass,
    0.5,
    3
);

INSERT INTO production_qa (question) VALUES ('昨天生产情况');
```

**实际输出**：
```
 id |   question   |                                          sql_result
----+--------------+----------------------------------------------------------------------------------------------
  1 | 昨天生产情况 | SELECT wd.record_date AS 统计日期, wd.produce_quantity AS 产出数量 FROM dfs_metrics_work_order_daily wd WHERE DATE(wd.record_date) = DATE_SUB(CURDATE(), INTERVAL 1 DAY);
```

| 结果 | ✅ 通过 |
|------|--------|
| 验证点 | INSERT 自动触发推理；sql_result 自动填充；prompt_template `{{question}}` 正确替换；RAG 检索命中"昨天生产情况"示例 |

---

### TC5: 类型转换 - INTEGER PREDICT

| 项目 | 说明 |
|------|------|
| 测试目标 | 验证 LLM 文本输出自动转换为 INTEGER 类型 |
| 测试函数 | `llm_rag_predict(input_row)` |
| 预期结果 | sentiment_score 列为 integer 类型，值域 {-1, 0, 1} |

**测试 SQL**：
```sql
CREATE TABLE sentiment_test (
    id SERIAL PRIMARY KEY,
    review_text TEXT,
    sentiment_score INTEGER PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'llm_rag_predict'
);

SELECT set_predict_config(
    'sentiment_test'::regclass,
    'https://ark.cn-beijing.volces.com/api/v3/chat/completions',
    'acc96ba1-d743-45d7-9b0e-a415bd96a046',
    'ep-20251128103853-pp9jw',
    0.1, 64,
    'You are a sentiment analysis assistant. Given a review, output ONLY a single integer: 1 for positive, 0 for neutral, -1 for negative. No other text.',
    'Review: {{review_text}}',
    0,
    'rag_examples'::regclass,
    0.5,
    3
);

INSERT INTO sentiment_test (review_text) VALUES ('This product is amazing! I love it!');
INSERT INTO sentiment_test (review_text) VALUES ('Terrible quality, waste of money');
INSERT INTO sentiment_test (review_text) VALUES ('It is okay, nothing special');
```

**实际输出**：
```
 id |             review_text             | sentiment_score | actual_type
----+-------------------------------------+-----------------+---------
  1 | This product is amazing! I love it! |               1 | integer
  2 | Terrible quality, waste of money    |              -1 | integer
  3 | It is okay, nothing special         |               0 | integer
```

| 结果 | ✅ 通过 |
|------|--------|
| 验证点 | LLM 输出 "1"/"-1"/"0" 文本 → predict_trigger 自动转为 integer 类型；pg_typeof 确认为 integer |

---

### TC6: 类型转换 - FLOAT8 (double precision) PREDICT

| 项目 | 说明 |
|------|------|
| 测试目标 | 验证 LLM 文本输出自动转换为 FLOAT8 类型 |
| 测试函数 | `llm_rag_predict(input_row)` |
| 预期结果 | math_score 列为 double precision 类型 |

**测试 SQL**：
```sql
CREATE TABLE score_test (
    id SERIAL PRIMARY KEY,
    student_name TEXT,
    math_score FLOAT8 PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'llm_rag_predict'
);

SELECT set_predict_config(
    'score_test'::regclass,
    'https://ark.cn-beijing.volces.com/api/v3/chat/completions',
    'acc96ba1-d743-45d7-9b0e-a415bd96a046',
    'ep-20251128103853-pp9jw',
    0.1, 64,
    'You are a score estimation assistant. Given a student description, output ONLY a single decimal number (0-100) representing the estimated math score. No other text.',
    'Student: {{student_name}}',
    0,
    'rag_examples'::regclass,
    0.5,
    3
);

INSERT INTO score_test (student_name) VALUES ('Alice - excellent at math, always top of class');
INSERT INTO score_test (student_name) VALUES ('Bob - struggles with math, barely passes');
INSERT INTO score_test (student_name) VALUES ('Charlie - average math student');
```

**实际输出**：
```
 id |                  student_name                  | math_score | actual_type
----+------------------------------------------------+------------+---------------
  1 | Alice - excellent at math, always top of class |         98 | double precision
  2 | Bob - struggles with math, barely passes       |         60 | double precision
  3 | Charlie - average math student                 |         75 | double precision
```

| 结果 | ✅ 通过 |
|------|--------|
| 验证点 | LLM 输出 "98"/"60"/"75" 文本 → predict_trigger 自动转为 double precision 类型；分数合理性正确（Alice>Charlie>Bob） |

---

### TC7: 类型转换 - BOOLEAN PREDICT

| 项目 | 说明 |
|------|------|
| 测试目标 | 验证 LLM 文本输出自动转换为 BOOLEAN 类型 |
| 测试函数 | `llm_rag_predict(input_row)` |
| 预期结果 | is_spam 列为 boolean 类型 |

**测试 SQL**：
```sql
CREATE TABLE spam_test (
    id SERIAL PRIMARY KEY,
    email_content TEXT,
    is_spam BOOLEAN PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'llm_rag_predict'
);

SELECT set_predict_config(
    'spam_test'::regclass,
    'https://ark.cn-beijing.volces.com/api/v3/chat/completions',
    'acc96ba1-d743-45d7-9b0e-a415bd96a046',
    'ep-20251128103853-pp9jw',
    0.1, 64,
    'You are a spam detection assistant. Given an email, output ONLY a single word: true if spam, false if not spam. No other text.',
    'Email: {{email_content}}',
    0,
    'rag_examples'::regclass,
    0.5,
    3
);

INSERT INTO spam_test (email_content) VALUES ('Congratulations! You have won $1,000,000! Click here to claim your prize now!');
INSERT INTO spam_test (email_content) VALUES ('Hi John, the meeting tomorrow has been moved to 3pm. Please confirm.');
INSERT INTO spam_test (email_content) VALUES ('URGENT: Your account will be suspended! Verify your identity immediately by clicking this link!');
```

**实际输出**：
```
 id |                                          email_content                                          | is_spam | actual_type
----+-------------------------------------------------------------------------------------------------+---------+-------------
  1 | Congratulations! You have won $1,000,000! Click here to claim your prize now!                   | t       | boolean
  2 | Hi John, the meeting tomorrow has been moved to 3pm. Please confirm.                            | f       | boolean
  3 | URGENT: Your account will be suspended! Verify your identity immediately by clicking this link! | t       | boolean
```

| 结果 | ✅ 通过 |
|------|--------|
| 验证点 | LLM 输出 "true"/"false" 文本 → predict_trigger 自动转为 boolean 类型；垃圾邮件判断正确 |

---

### TC8: 异常处理 - RAG 表无 EMBEDDING 列

| 项目 | 说明 |
|------|------|
| 测试目标 | 验证 RAG 表没有 EMBEDDING 列时的错误提示 |
| 测试函数 | `llm_rag_infer()` |
| 预期结果 | 抛出 ERROR，提示 RAG 表没有 EMBEDDING 列 |

**测试 SQL**：
```sql
CREATE TABLE no_embedding_table (
    id SERIAL PRIMARY KEY,
    name TEXT
);

SELECT llm_rag_infer(
    'test',
    0,
    'test query',
    'no_embedding_table'::regclass,
    0.5,
    3
);
```

**实际输出**：
```
ERROR:  RAG table "no_embedding_table" does not have an EMBEDDING column
HINT:  Create an EMBEDDING column on the table first.
```

| 结果 | ✅ 通过 |
|------|--------|
| 验证点 | 错误信息清晰，HINT 提供修复建议 |

---

### TC9: 异常处理 - API 未配置

| 项目 | 说明 |
|------|------|
| 测试目标 | 验证 API URL 未配置时的错误提示 |
| 测试函数 | `llm_rag_infer()` |
| 预期结果 | 抛出 ERROR，提示 API URL 未配置 |

**测试 SQL**：
```sql
RESET pg_predict.api_url;
RESET pg_predict.api_key;

SELECT llm_rag_infer(
    'test',
    0,
    'test query',
    'rag_examples'::regclass,
    0.5,
    3
);
```

**实际输出**：
```
ERROR:  pg_predict.api_url is not configured
HINT:  Set pg_predict.api_url or use set_predict_config() to configure the API endpoint.
```

| 结果 | ✅ 通过 |
|------|--------|
| 验证点 | 错误信息清晰，HINT 提供修复建议 |

## 4. 测试结果汇总

| 用例编号 | 测试内容 | 函数 | 结果 |
|---------|---------|------|------|
| TC1 | llm_infer 基础推理 | llm_infer | ✅ 通过 |
| TC2 | llm_infer 带历史对话参数 | llm_infer | ✅ 通过 |
| TC3 | llm_rag_infer RAG 推理 | llm_rag_infer | ✅ 通过 |
| TC4 | llm_rag_predict PREDICT 列自动推理 | llm_rag_predict | ✅ 通过 |
| TC5 | 类型转换 - INTEGER | llm_rag_predict | ✅ 通过 |
| TC6 | 类型转换 - FLOAT8 | llm_rag_predict | ✅ 通过 |
| TC7 | 类型转换 - BOOLEAN | llm_rag_predict | ✅ 通过 |
| TC8 | 异常处理 - RAG 表无 EMBEDDING 列 | llm_rag_infer | ✅ 通过 |
| TC9 | 异常处理 - API 未配置 | llm_rag_infer | ✅ 通过 |

**通过率：9/9 = 100%**

## 5. RAG 消息流程验证

参考 D:\1.txt 中的 RAG 消息流程，验证完整链路：

```
用户输入: "上周生产情况"
       │
       ▼
┌─────────────────────────────────────────┐
│ ① compute_embedding_for_text()          │
│    调用 sentence_transformers_embedding │
│    "上周生产情况" → 384维向量            │
└──────────────┬──────────────────────────┘
               │
               ▼
┌─────────────────────────────────────────┐
│ ② SPI 向量相似度搜索                     │
│    SELECT * FROM rag_examples            │
│    ORDER BY content_embedding            │
│    <=> $1::vector LIMIT 3               │
│                                         │
│    → 检索到:                             │
│      "上周每天的生产情况分布"             │
│      "上周三的生产情况"                   │
│      "昨天生产情况"                       │
└──────────────┬──────────────────────────┘
               │
               ▼
┌─────────────────────────────────────────┐
│ ③ 格式化上下文                           │
│    "Retrieved context:                   │
│     --- Result 1 ---                     │
│     question: 上周每天的生产情况分布       │
│     answer: SELECT ... FROM ...          │
│     tables: dfs_metrics_work_order_daily │
│     --- Result 2 ---                     │
│     ..."                                 │
└──────────────┬──────────────────────────┘
               │
               ▼
┌─────────────────────────────────────────┐
│ ④ 组合 + 发送 LLM                        │
│    messages = [                          │
│      {system: "你是SQL生成助手..."},      │
│      {user: "{上下文}\n\n上周生产情况"}    │
│    ]                                     │
└──────────────┬──────────────────────────┘
               │
               ▼
┌─────────────────────────────────────────┐
│ ⑤ LLM 返回 SQL                           │
│    "SELECT DATE(wd.record_date) AS       │
│     统计日期, SUM(...) AS 产出数量        │
│     FROM ... WHERE YEARWEEK(...)         │
│     GROUP BY ... ORDER BY ...;"          │
└──────────────┬──────────────────────────┘
               │
               ▼
┌─────────────────────────────────────────┐
│ ⑥ predict_trigger 类型转换               │
│    TEXT → TEXT (无转换)                   │
│    TEXT → INTEGER (TC5 验证)             │
│    TEXT → FLOAT8 (TC6 验证)              │
│    TEXT → BOOLEAN (TC7 验证)             │
└─────────────────────────────────────────┘
```

## 6. 已知限制

1. **向量维度匹配**：RAG 表创建时必须指定 `vector_len = 384`（与 sentence_transformers 默认模型一致），否则 INSERT 时会报维度不匹配错误
2. **embedding 函数依赖**：RAG 检索依赖 `pg_embedding_st` 扩展的 `sentence_transformers_embedding` 函数，首次调用需要加载 Python 模型（约10秒）
3. **SPI 不触发查询重写**：`do_rag_retrieval()` 通过 SPI 执行查询不会被查询重写器处理，因此必须直接使用 `_embedding` 隐藏列和 pgvector 的 `<=>` 操作符
4. **LLM 输出不确定性**：LLM 的输出具有随机性（temperature > 0 时），类型转换可能因 LLM 输出格式不规范而失败
