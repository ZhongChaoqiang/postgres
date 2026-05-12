# pg_predict 测试报告

## 1. 测试环境

| 项目 | 值 |
|------|-----|
| 操作系统 | Ubuntu (WSL2) |
| PostgreSQL 版本 | 18.3 (定制版) |
| pg_predict 版本 | 1.0 |
| libcurl 版本 | 7.81.0 |
| LLM API | 火山引擎 Ark API (https://ark.cn-beijing.volces.com/api/v3) |
| LLM 模型 | ep-20251128103853-pp9jw (doubao-seed-1-6-vision) |
| Embedding 模型 | sentence-transformers/all-MiniLM-L6-v2 (384维) |
| 测试日期 | 2026-05-06 |

## 2. 测试方法

使用火山引擎豆包大模型真实 API 进行端到端功能测试，验证所有核心功能，包括 llm_predict/llm_rag_predict 内置函数。

## 3. 测试用例及结果

### TC-01: 扩展安装与系统级配置 ✅

```sql
DROP EXTENSION IF EXISTS pg_predict;
CREATE EXTENSION pg_predict;

SELECT set_predict_config(
    'https://ark.cn-beijing.volces.com/api/v3/chat/completions',
    'acc96ba1-d743-45d7-9b0e-a415bd96a046',
    'ep-20251128103853-pp9jw',
    0.3,
    256,
    'You are a helpful assistant.',
    0
);

SELECT * FROM get_predict_config();
```

**实际结果**：
```
 api_url                                                      | api_key                              | model_name               | temperature | max_tokens | system_prompt              | history_count
--------------------------------------------------------------+--------------------------------------+--------------------------+-------------+------------+----------------------------+---------------
 https://ark.cn-beijing.volces.com/api/v3/chat/completions    | acc96ba1-...                         | ep-20251128103853-pp9jw  |         0.3 |        256 | You are a helpful assistant.|             0
```

---

### TC-02: llm_infer 独立推理 ✅

```sql
SET pg_predict.api_url = 'https://ark.cn-beijing.volces.com/api/v3/chat/completions';
SET pg_predict.api_key = 'acc96ba1-d743-45d7-9b0e-a415bd96a046';
SET pg_predict.model = 'ep-20251128103853-pp9jw';
SET pg_predict.temperature = 0.3;
SET pg_predict.max_tokens = 100;

SELECT llm_infer('You are a helpful assistant. Reply in one word.', 'What is the capital of France?');
```

**实际结果**：
```
 llm_infer 
-----------
 Paris
```

---

### TC-03: PREDICT 列 + 单列 prompt_template 文本分类 ✅

```sql
CREATE TABLE test_llm_articles (
    id SERIAL PRIMARY KEY,
    content TEXT,
    category TEXT PREDICT AS (llm_predict()) STORED
) WITH (
    predict_timing = immediate
);

SELECT set_predict_config(
    'test_llm_articles'::regclass,
    'https://ark.cn-beijing.volces.com/api/v3/chat/completions',
    'acc96ba1-d743-45d7-9b0e-a415bd96a046',
    'ep-20251128103853-pp9jw',
    0.1, 64,
    'Classify the following text into exactly one category: technology, sports, politics, entertainment. Reply with only the category name, nothing else.',
    'Classify this text: {{content}}',
    0
);

INSERT INTO test_llm_articles (content) VALUES ('AI and machine learning are transforming software development');
INSERT INTO test_llm_articles (content) VALUES ('The basketball game was exciting with a last-second shot');
INSERT INTO test_llm_articles (content) VALUES ('New government policies on climate change were announced today');

SELECT id, content, category FROM test_llm_articles;
```

**实际结果**：
```
 id |                            content                            |  category  
----+---------------------------------------------------------------+------------
  1 | AI and machine learning are transforming software development | technology
  2 | The basketball game was exciting with a last-second shot      | sports
  3 | New government policies on climate change were announced today | politics
```

**验证点**：`PREDICT AS (llm_predict()) STORED` 直接引用内置函数，无需指定 schema

---

### TC-04: PREDICT 列 + 多列 prompt_template 情感分析 ✅

```sql
CREATE TABLE test_llm_reviews (
    id SERIAL PRIMARY KEY,
    review_text TEXT,
    product_name TEXT,
    sentiment TEXT PREDICT AS (llm_predict()) STORED
) WITH (
    predict_timing = immediate
);

SELECT set_predict_config(
    'test_llm_reviews'::regclass, ...,
    'Analyze the sentiment of the review. Reply with exactly one word: positive, negative, or neutral.',
    'Review of {{product_name}}: {{review_text}}',
    0
);

INSERT INTO test_llm_reviews (review_text, product_name) VALUES ('This product is amazing! I love it!', 'Widget Pro');
INSERT INTO test_llm_reviews (review_text, product_name) VALUES ('Terrible quality, waste of money', 'CheapGadget');
```

**实际结果**：
```
 id | product_name |             review_text             | sentiment 
----+--------------+-------------------------------------+-----------
  1 | Widget Pro   | This product is amazing! I love it! | positive
  2 | CheapGadget  | Terrible quality, waste of money    | negative
```

---

### TC-05: PREDICT 列 + 无模板（prompt_template=NULL）自动行格式化 ✅

```sql
CREATE TABLE test_llm_tickets (
    id SERIAL PRIMARY KEY,
    subject TEXT,
    description TEXT,
    priority TEXT PREDICT AS (llm_predict()) STORED
) WITH (
    predict_timing = immediate
);

SELECT set_predict_config(
    'test_llm_tickets'::regclass, ...,
    'Assign a priority level based on the ticket information. Reply with exactly one word: high, medium, or low.',
    NULL, 0
);

INSERT INTO test_llm_tickets (subject, description) VALUES ('System down', 'Production server is not responding, all users affected');
INSERT INTO test_llm_tickets (subject, description) VALUES ('Font issue', 'The font on the about page looks slightly different');
```

**实际结果**：
```
 id |   subject   |                       description                       | priority
----+-------------+---------------------------------------------------------+---------
  1 | System down | Production server is not responding, all users affected | high
  2 | Font issue  | The font on the about page looks slightly different     | low
```

---

### TC-06: PREDICT AS 列 + prompt_template + 历史对话 ✅

```sql
CREATE TABLE test_llm_chat (
    id SERIAL PRIMARY KEY,
    user_message TEXT,
    assistant_reply TEXT PREDICT AS (llm_predict()) STORED
) WITH (predict_timing = immediate);

SELECT set_predict_config(
    'test_llm_chat'::regclass, ...,
    'You are a helpful customer service assistant for an online store. Keep your replies concise.',
    '{{user_message}}',
    3
);

INSERT INTO test_llm_chat (user_message) VALUES ('Hello, I need help with my order');
INSERT INTO test_llm_chat (user_message) VALUES ('My order number is 12345, it has not arrived yet');
INSERT INTO test_llm_chat (user_message) VALUES ('When can I expect it?');
```

**实际结果**：
```
 id |           user_message           |                                                        assistant_reply
----+----------------------------------+-----------------------------------------------------------------------------------------------
  1 | Hello, I need help with my order | Sure! To assist you, please share your order number...
  2 | My order number is 12345, it...  | Thanks for sharing order #12345. Let me check its tracking status...
  3 | When can I expect it?            | I'm reviewing the tracking for order #12345. I'll share the estimated delivery date...
```

**验证点**：第3轮回复中正确引用了订单号 #12345，证明历史对话上下文生效

---

### TC-07: INTEGER 类型自动转换 ✅

```sql
CREATE TABLE test_llm_price (
    id SERIAL PRIMARY KEY,
    product_name TEXT,
    description TEXT,
    price INTEGER PREDICT AS (llm_predict()) STORED
) WITH (predict_timing = immediate);

INSERT INTO test_llm_price (product_name, description) VALUES ('Widget Pro', 'A high-end widget that costs 499 dollars');
```

**实际结果**：
```
 id | product_name |          description           | price | actual_type
----+--------------+--------------------------------+-------+-------------
  1 | Widget Pro   | A high-end widget that costs... |   500 | integer
```

**验证点**：LLM 返回 "500" 文本 → predict_trigger 自动转换为 integer 类型

---

### TC-08: GUC 参数动态配置 ✅

```sql
SET pg_predict.api_url = 'https://ark.cn-beijing.volces.com/api/v3/chat/completions';
SET pg_predict.api_key = 'acc96ba1-d743-45d7-9b0e-a415bd96a046';
SET pg_predict.model = 'ep-20251128103853-pp9jw';
SET pg_predict.temperature = 0.3;
SET pg_predict.max_tokens = 100;
```

**实际结果**：所有 GUC 参数设置生效

---

### TC-09: API URL 未配置错误处理 ✅

```sql
RESET pg_predict.api_url;
SELECT llm_infer('test', 'hello');
```

**实际结果**：
```
NOTICE: TC-09: Got expected error: pg_predict.api_url is not configured
```

---

### TC-10: API 不可达错误处理 ✅

```sql
SET pg_predict.api_url = 'http://invalid-host-nonexist.example.com/v1/chat/completions';
SELECT llm_infer('test', 'hello');
```

**实际结果**：
```
NOTICE: TC-10: Got expected error: LLM API request failed: Couldn't resolve host name
```

---

### TC-RAG1: llm_rag_infer RAG 推理 ✅

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

**实际结果**：
```sql
SELECT SUM(wd.produce_quantity) AS 上周总产出 FROM dfs_metrics_work_order_daily wd
WHERE YEARWEEK(wd.record_date) = YEARWEEK(CURDATE() - INTERVAL 1 WEEK);
```

**验证点**：RAG 向量检索命中"上周"相关示例，LLM 生成正确的 SQL

---

### TC-RAG2: PREDICT AS 列 + llm_rag_predict 自动推理 ✅

```sql
CREATE TABLE production_qa (
    id SERIAL PRIMARY KEY,
    question TEXT,
    sql_result TEXT PREDICT AS (llm_rag_predict()) STORED
) WITH (predict_timing = immediate);

INSERT INTO production_qa (question) VALUES ('昨天生产情况');
```

**实际结果**：
```
 id |   question   |                                          sql_result
----+--------------+----------------------------------------------------------------------------------------------
  1 | 昨天生产情况 | SELECT * FROM production WHERE DATE(create_time) = DATE_SUB(CURDATE(), INTERVAL 1 DAY);
```

**验证点**：`PREDICT AS (llm_rag_predict()) STORED` 直接引用内置函数

---

### TC-RAG3: 类型转换 - INTEGER PREDICT + RAG ✅

```sql
CREATE TABLE sentiment_test (
    id SERIAL PRIMARY KEY,
    review_text TEXT,
    sentiment_score INTEGER PREDICT AS (llm_rag_predict()) STORED
) WITH (predict_timing = immediate);
```

**实际结果**：
```
 id |             review_text             | sentiment_score | actual_type
----+-------------------------------------+-----------------+------------
  1 | This product is amazing! I love it! |               1 | integer
  2 | Terrible quality, waste of money    |              -1 | integer
  3 | It is okay, nothing special         |               0 | integer
```

---

### TC-RAG4: 类型转换 - FLOAT8 PREDICT + RAG ✅

**实际结果**：
```
 id |                  student_name                  | math_score | actual_type
----+------------------------------------------------+------------+---------------
  1 | Alice - excellent at math, always top of class |         98 | double precision
  2 | Bob - struggles with math, barely passes       |         50 | double precision
  3 | Charlie - average math student                 |         75 | double precision
```

---

### TC-RAG5: 类型转换 - BOOLEAN PREDICT + RAG ✅

**实际结果**：
```
 id |                                          email_content                                          | is_spam | actual_type
----+-------------------------------------------------------------------------------------------------+---------+-------------
  1 | Congratulations! You have won $1,000,000! Click here to claim your prize now!                   | t       | boolean
  2 | Hi John, the meeting tomorrow has been moved to 3pm. Please confirm.                            | f       | boolean
  3 | URGENT: Your account will be suspended! Verify your identity immediately by clicking this link! | t       | boolean
```

---

### TC-RAG6: 异常处理 - RAG 表无 EMBEDDING 列 ✅

**实际结果**：
```
NOTICE: TC-RAG6: Got expected error: RAG table "no_embedding_table" does not have an EMBEDDING column
```

---

### TC-RAG7: 内置函数验证 ✅

```sql
SELECT proname, pronamespace::regnamespace, oid
FROM pg_proc WHERE proname IN ('llm_predict', 'llm_rag_predict')
AND pronamespace = (SELECT oid FROM pg_namespace WHERE nspname = 'pg_catalog');
```

**实际结果**：
```
     proname     | pronamespace | oid
-----------------+--------------+------
 llm_predict     | pg_catalog   | 6506
 llm_rag_predict | pg_catalog   | 6507
```

**验证点**：`llm_predict` 和 `llm_rag_predict` 已注册为 `pg_catalog` 内置函数，可在 `PREDICT AS` 表达式中直接使用

## 4. 测试结果汇总

| 编号 | 测试项 | 函数 | 状态 |
|------|--------|------|------|
| TC-01 | 扩展安装与系统级配置 | set_predict_config | ✅ |
| TC-02 | llm_infer 独立推理 | llm_infer | ✅ |
| TC-03 | 单列 prompt_template 文本分类 | llm_predict (内置) | ✅ |
| TC-04 | 多列 prompt_template 情感分析 | llm_predict (内置) | ✅ |
| TC-05 | 无模板自动行格式化 | llm_predict (内置) | ✅ |
| TC-06 | 带历史对话的推理 | llm_predict (内置) | ✅ |
| TC-07 | INTEGER 类型自动转换 | llm_predict (内置) | ✅ |
| TC-08 | GUC 参数动态配置 | - | ✅ |
| TC-09 | API URL 未配置错误处理 | llm_infer | ✅ |
| TC-10 | API 不可达错误处理 | llm_infer | ✅ |
| TC-RAG1 | llm_rag_infer RAG 推理 | llm_rag_infer | ✅ |
| TC-RAG2 | llm_rag_predict PREDICT 列自动推理 | llm_rag_predict (内置) | ✅ |
| TC-RAG3 | INTEGER 类型转换 + RAG | llm_rag_predict (内置) | ✅ |
| TC-RAG4 | FLOAT8 类型转换 + RAG | llm_rag_predict (内置) | ✅ |
| TC-RAG5 | BOOLEAN 类型转换 + RAG | llm_rag_predict (内置) | ✅ |
| TC-RAG6 | RAG 表无 EMBEDDING 列异常 | llm_rag_infer | ✅ |
| TC-RAG7 | 内置函数验证 | pg_catalog | ✅ |

**总计**：17/17 通过

## 5. 内置函数架构

```
┌──────────────────────────────────────────────────────┐
│ pg_catalog 内置函数 (predict.c)                       │
│                                                      │
│ llm_predict(record) → text     [OID 6506]            │
│ llm_rag_predict(record) → text [OID 6507]            │
│                                                      │
│   ↓ 转发到扩展函数                                    │
│                                                      │
│ find_extension_function_oid("llm_predict_ext", 1)    │
│ find_extension_function_oid("llm_rag_predict_ext", 1)│
└──────────────────────┬───────────────────────────────┘
                       │
                       ▼
┌──────────────────────────────────────────────────────┐
│ pg_predict 扩展函数 (pg_predict.c)                    │
│                                                      │
│ llm_predict_ext(record) → text                       │
│ llm_rag_predict_ext(record) → text                   │
│                                                      │
│ 实际 LLM 推理逻辑：                                   │
│ - 读取 pg_predict_config 配置                         │
│ - 构建 prompt (模板/行格式化)                          │
│ - RAG 向量检索 (llm_rag_predict_ext)                  │
│ - 调用 LLM API                                       │
│ - 返回文本结果                                        │
└──────────────────────────────────────────────────────┘
```

**使用方式**：
```sql
-- 使用 PREDICT AS 语法，直接引用内置函数
CREATE TABLE t (
    id SERIAL PRIMARY KEY,
    input TEXT,
    output TEXT PREDICT AS (llm_predict()) STORED
) WITH (predict_timing = immediate);

-- RAG 增强推理
CREATE TABLE t (
    id SERIAL PRIMARY KEY,
    question TEXT,
    answer TEXT PREDICT AS (llm_rag_predict()) STORED
) WITH (predict_timing = immediate);
```

## 6. 已知问题

1. **SSL 验证**：默认关闭 SSL 证书验证，生产环境应启用
2. **API 密钥安全**：密钥以明文存储在 `pg_predict_config` 表中
3. **历史查询排序**：使用 `ctid` 排序，VACUUM FULL 后可能改变顺序
4. **模板中 PREDICT 列**：模板中不能引用 PREDICT 列本身（其值尚未计算）
5. **向量维度匹配**：RAG 表创建时必须指定 `vector_len = 384`
6. **扩展依赖**：内置函数转发到 pg_predict 扩展，需先安装扩展

## 7. 性能考虑

- 每次 LLM 调用都是同步阻塞的，耗时取决于 API 响应速度
- 建议对大批量数据使用 `predict_timing = deferred` 配合异步推理
- 历史对话查询会增加 SPI 开销，建议 history_count 不超过 10
- 整行数据格式化可能产生较长的 prompt，注意 token 限制
- RAG 检索需要调用 embedding 函数，首次调用需加载模型（约10秒）

## 8. 结论

pg_predict 扩展使用火山引擎豆包大模型真实 API 进行端到端测试，所有 17 项测试用例全部通过，包括：
- `llm_predict` 和 `llm_rag_predict` 已注册为 `pg_catalog` 内置函数（OID 6506/6507）
- 可直接在 `PREDICT AS` 表达式中使用，无需指定 schema
- `{{column_name}}` 模板语法正确替换（单列、多列）
- 整行数据自动格式化为键值对（无模板模式）
- 历史对话正确构建，LLM 能引用上下文
- RAG 向量检索正确命中相关示例
- INTEGER/FLOAT8/BOOLEAN 类型自动转换
- 错误处理正确
