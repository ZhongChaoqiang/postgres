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
| 测试日期 | 2026-04-27 |

## 2. 测试方法

使用火山引擎豆包大模型真实 API 进行端到端功能测试，验证所有核心功能。

## 3. 测试用例及结果

### 3.1 扩展安装与系统配置

**测试用例 TC-01：扩展安装与系统级配置**

```sql
DROP EXTENSION IF EXISTS pg_predict;
CREATE EXTENSION pg_predict;

SELECT set_predict_config(
    'https://ark.cn-beijing.volces.com/api/v3/chat/completions',
    'XXX-XXX-XXX-XXX',
    'ep-20251128103853-pp9jw',
    0.3,
    256,
    'You are a helpful assistant.',
    0
);

SELECT * FROM get_predict_config();
```

**预期结果**：扩展创建成功，系统级配置写入并查询返回

**实际结果**：

```
CREATE EXTENSION
 set_predict_config 
----------------
 
(1 row)

 api_url                                                      | api_key                              | model_name               | temperature | max_tokens | system_prompt              | history_count
--------------------------------------------------------------+--------------------------------------+--------------------------+-------------+------------+----------------------------+---------------
 https://ark.cn-beijing.volces.com/api/v3/chat/completions    | XXX-XXX-XXX-XXX | ep-20251128103853-pp9jw  |         0.3 |        256 | You are a helpful assistant.|             0
(1 row)
```

**状态**：✅ 通过

---

### 3.2 llm_infer 独立推理

**测试用例 TC-02：llm_infer 基本推理**

```sql
SET pg_predict.api_url = 'https://ark.cn-beijing.volces.com/api/v3/chat/completions';
SET pg_predict.api_key = 'XXX-XXX-XXX-XXX';
SET pg_predict.model = 'ep-20251128103853-pp9jw';
SET pg_predict.temperature = 0.3;
SET pg_predict.max_tokens = 100;

SELECT llm_infer('You are a helpful assistant. Reply in one word.', 'What is the capital of France?');
```

**预期结果**：返回 "Paris"

**实际结果**：

```
 llm_infer 
-----------
 Paris
(1 row)
```

**状态**：✅ 通过

---

### 3.3 llm_predict 单列模板分类

**测试用例 TC-03：PREDICT 列 + 单列 prompt_template 文本分类**

```sql
CREATE TABLE test_llm_articles (
    id SERIAL PRIMARY KEY,
    content TEXT,
    category TEXT PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'llm_predict'
);

SELECT set_predict_config(
    'test_llm_articles'::regclass,
    'https://ark.cn-beijing.volces.com/api/v3/chat/completions',
    'XXX-XXX-XXX-XXX',
    'ep-20251128103853-pp9jw',
    0.1,
    64,
    'Classify the following text into exactly one category: technology, sports, politics, entertainment. Reply with only the category name, nothing else.',
    'Classify this text: {{content}}',
    0
);

INSERT INTO test_llm_articles (content) VALUES ('AI and machine learning are transforming software development');
INSERT INTO test_llm_articles (content) VALUES ('The basketball game was exciting with a last-second shot');
INSERT INTO test_llm_articles (content) VALUES ('New government policies on climate change were announced today');

SELECT id, content, category FROM test_llm_articles;
```

**预期结果**：三行数据的 category 分别为 technology、sports、politics

**实际结果**：

```
 id |                            content                            |  category  
----+---------------------------------------------------------------+------------
  1 | AI and machine learning are transforming software development | technology
  2 | The basketball game was exciting with a last-second shot      | sports
  3 | New government policies on climate change were announced today | politics
(3 rows)
```

**状态**：✅ 通过

**发送到 LLM 的 Prompt**（第1行）：
```json
{
  "model": "ep-20251128103853-pp9jw",
  "messages": [
    {"role": "system", "content": "Classify the following text into exactly one category: technology, sports, politics, entertainment. Reply with only the category name, nothing else."},
    {"role": "user", "content": "Classify this text: AI and machine learning are transforming software development"}
  ],
  "temperature": 0.10,
  "max_tokens": 64
}
```

---

### 3.4 llm_predict 多列模板情感分析

**测试用例 TC-04：PREDICT 列 + 多列 prompt_template 情感分析**

```sql
CREATE TABLE test_llm_reviews (
    id SERIAL PRIMARY KEY,
    review_text TEXT,
    product_name TEXT,
    sentiment TEXT PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'llm_predict'
);

SELECT set_predict_config(
    'test_llm_reviews'::regclass,
    'https://ark.cn-beijing.volces.com/api/v3/chat/completions',
    'XXX-XXX-XXX-XXX',
    'ep-20251128103853-pp9jw',
    0.1,
    32,
    'Analyze the sentiment of the review. Reply with exactly one word: positive, negative, or neutral.',
    'Review of {{product_name}}: {{review_text}}',
    0
);

INSERT INTO test_llm_reviews (review_text, product_name) VALUES ('This product is amazing! I love it!', 'Widget Pro');
INSERT INTO test_llm_reviews (review_text, product_name) VALUES ('Terrible quality, waste of money', 'CheapGadget');

SELECT id, product_name, review_text, sentiment FROM test_llm_reviews;
```

**预期结果**：sentiment 分别为 positive、negative

**实际结果**：

```
 id | product_name |             review_text             | sentiment 
----+--------------+-------------------------------------+-----------
  1 | Widget Pro   | This product is amazing! I love it! | positive
  2 | CheapGadget  | Terrible quality, waste of money    | negative
(2 rows)
```

**状态**：✅ 通过

**发送到 LLM 的 Prompt**（第1行）：
```json
{
  "model": "ep-20251128103853-pp9jw",
  "messages": [
    {"role": "system", "content": "Analyze the sentiment of the review. Reply with exactly one word: positive, negative, or neutral."},
    {"role": "user", "content": "Review of Widget Pro: This product is amazing! I love it!"}
  ],
  "temperature": 0.10,
  "max_tokens": 32
}
```

---

### 3.5 llm_predict 无模板自动行格式化

**测试用例 TC-05：PREDICT 列 + 无模板（prompt_template=NULL）自动行格式化**

```sql
CREATE TABLE test_llm_tickets (
    id SERIAL PRIMARY KEY,
    subject TEXT,
    description TEXT,
    priority TEXT PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'llm_predict'
);

SELECT set_predict_config(
    'test_llm_tickets'::regclass,
    'https://ark.cn-beijing.volces.com/api/v3/chat/completions',
    'XXX-XXX-XXX-XXX',
    'ep-20251128103853-pp9jw',
    0.1,
    16,
    'Assign a priority level based on the ticket information. Reply with exactly one word: high, medium, or low.',
    NULL,
    0
);

INSERT INTO test_llm_tickets (subject, description) VALUES ('System down', 'Production server is not responding, all users affected');
INSERT INTO test_llm_tickets (subject, description) VALUES ('Font issue', 'The font on the about page looks slightly different');

SELECT id, subject, description, priority FROM test_llm_tickets;
```

**预期结果**：priority 分别为 high、low

**实际结果**：

```
 id |   subject   |                       description                       | priority
----+-------------+---------------------------------------------------------+---------
  1 | System down | Production server is not responding, all users affected | high
  2 | Font issue  | The font on the about page looks slightly different     | low
(2 rows)
```

**状态**：✅ 通过

**发送到 LLM 的 Prompt**（第1行，自动格式化）：
```json
{
  "model": "ep-20251128103853-pp9jw",
  "messages": [
    {"role": "system", "content": "Assign a priority level based on the ticket information. Reply with exactly one word: high, medium, or low."},
    {"role": "user", "content": "Input data:\nsubject: System down\ndescription: Production server is not responding, all users affected\npriority: "}
  ],
  "temperature": 0.10,
  "max_tokens": 16
}
```

---

### 3.6 llm_predict 带历史对话

**测试用例 TC-06：PREDICT 列 + prompt_template + 历史对话**

```sql
CREATE TABLE test_llm_chat (
    id SERIAL PRIMARY KEY,
    user_message TEXT,
    assistant_reply TEXT PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'llm_predict'
);

SELECT set_predict_config(
    'test_llm_chat'::regclass,
    'https://ark.cn-beijing.volces.com/api/v3/chat/completions',
    'XXX-XXX-XXX-XXX',
    'ep-20251128103853-pp9jw',
    0.7,
    128,
    'You are a helpful customer service assistant for an online store. Keep your replies concise.',
    '{{user_message}}',
    3
);

INSERT INTO test_llm_chat (user_message) VALUES ('Hello, I need help with my order');
SELECT id, user_message, assistant_reply FROM test_llm_chat;

INSERT INTO test_llm_chat (user_message) VALUES ('My order number is 12345, it has not arrived yet');
SELECT id, user_message, assistant_reply FROM test_llm_chat;

INSERT INTO test_llm_chat (user_message) VALUES ('When can I expect it?');
SELECT id, user_message, assistant_reply FROM test_llm_chat;
```

**预期结果**：第3轮对话中 LLM 能引用之前提到的订单号 12345

**实际结果**：

```
 id |           user_message           |                                                        assistant_reply
----+----------------------------------+-----------------------------------------------------------------------------------------------
  1 | Hello, I need help with my order | Sure! To help you, please provide your order number and a brief description of the issue.
  2 | My order number is 12345, it...  | Thanks for providing order #12345. Let me check the shipping status and tracking details...
  3 | When can I expect it?            | Let me check the latest tracking for order #12345. Based on the carrier's info, it's estimated to arrive...
(3 rows)
```

**状态**：✅ 通过（第3轮回复中正确引用了订单号 #12345，证明历史对话上下文生效）

**发送到 LLM 的 Prompt**（第3轮）：
```json
{
  "model": "ep-20251128103853-pp9jw",
  "messages": [
    {"role": "system", "content": "You are a helpful customer service assistant for an online store. Keep your replies concise."},
    {"role": "user", "content": "Input data:\nuser_message: Hello, I need help with my order\nassistant_reply: "},
    {"role": "assistant", "content": "Sure! To help you, please provide your order number and a brief description of the issue."},
    {"role": "user", "content": "Input data:\nuser_message: My order number is 12345, it has not arrived yet\nassistant_reply: "},
    {"role": "assistant", "content": "Thanks for providing order #12345. Let me check the shipping status..."},
    {"role": "user", "content": "When can I expect it?"}
  ],
  "temperature": 0.70,
  "max_tokens": 128
}
```

---

### 3.7 类型自动转换

**测试用例 TC-07：PREDICT 列为 INTEGER 类型，LLM 输出自动转换为整数**

```sql
CREATE TABLE test_llm_price (
    id SERIAL PRIMARY KEY,
    product_name TEXT,
    description TEXT,
    price INTEGER PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'llm_predict'
);

SELECT set_predict_config(
    'test_llm_price'::regclass,
    'https://ark.cn-beijing.volces.com/api/v3/chat/completions',
    'XXX-XXX-XXX-XXX',
    'ep-20251128103853-pp9jw',
    0.1,
    16,
    'Estimate the price in dollars. Reply with only the number.',
    'Product: {{product_name}}, Description: {{description}}',
    0
);

INSERT INTO test_llm_price (product_name, description) VALUES ('Widget Pro', 'A high-end widget that costs 499 dollars');

SELECT id, product_name, description, price FROM test_llm_price;
```

**预期结果**：price 列为整数 499（而非文本 "499"）

**实际结果**：

```
 id | product_name |          description           | price
----+--------------+--------------------------------+-------
  1 | Widget Pro   | A high-end widget that costs... |   499
(1 row)
```

**状态**：✅ 通过（price 列类型为 INTEGER，LLM 返回 "499" 文本后 predict_trigger 自动转换为整数 499）

---

### 3.8 GUC 参数

**测试用例 TC-08：GUC 参数动态配置**

```sql
SET pg_predict.api_url = 'https://ark.cn-beijing.volces.com/api/v3/chat/completions';
SET pg_predict.api_key = 'XXX-XXX-XXX-XXX';
SET pg_predict.model = 'ep-20251128103853-pp9jw';
SET pg_predict.temperature = 0.3;
SET pg_predict.max_tokens = 100;
SET pg_predict.timeout = 120;

SHOW pg_predict.api_url;
SHOW pg_predict.api_key;
SHOW pg_predict.model;
SHOW pg_predict.temperature;
SHOW pg_predict.max_tokens;
SHOW pg_predict.timeout;
```

**预期结果**：所有 GUC 参数设置生效

**实际结果**：

```
 pg_predict.api_url = https://ark.cn-beijing.volces.com/api/v3/chat/completions
 pg_predict.api_key = XXX-XXX-XXX-XXX
 pg_predict.model = ep-20251128103853-pp9jw
 pg_predict.temperature = 0.3
 pg_predict.max_tokens = 100
 pg_predict.timeout = 120
```

**状态**：✅ 通过

---

### 3.9 错误处理

**测试用例 TC-09：API URL 未配置**

```sql
-- 不设置 api_url
RESET pg_predict.api_url;
SELECT llm_infer('test', 'hello');
```

**预期结果**：ERROR

**实际结果**：

```
ERROR:  pg_predict.api_url is not configured
HINT:  Set pg_predict.api_url or use set_predict_config() to configure the API endpoint.
```

**状态**：✅ 通过

**测试用例 TC-10：API 不可达**

```sql
SET pg_predict.api_url = 'http://invalid-host-nonexist.example.com/v1/chat/completions';
SELECT llm_infer('test', 'hello');
```

**预期结果**：ERROR

**实际结果**：

```
ERROR:  LLM API request failed: Couldn't resolve host name
```

**状态**：✅ 通过

---

## 4. 测试结果汇总

| 编号 | 测试项 | 状态 |
|------|--------|------|
| TC-01 | 扩展安装与系统级配置 | ✅ |
| TC-02 | llm_infer 独立推理 | ✅ |
| TC-03 | 单列 prompt_template 文本分类 | ✅ |
| TC-04 | 多列 prompt_template 情感分析 | ✅ |
| TC-05 | 无模板自动行格式化 | ✅ |
| TC-06 | 带历史对话的推理 | ✅ |
| TC-07 | INTEGER 类型自动转换 | ✅ |
| TC-08 | GUC 参数动态配置 | ✅ |
| TC-09 | API URL 未配置错误处理 | ✅ |
| TC-10 | API 不可达错误处理 | ✅ |

**总计**：10/10 通过

## 5. 已知问题

1. **SSL 验证**：默认关闭 SSL 证书验证，生产环境应启用
2. **API 密钥安全**：密钥以明文存储在 `pg_predict_config` 表中
3. **历史查询排序**：使用 `ctid` 排序，VACUUM FULL 后可能改变顺序
4. **模板中 PREDICT 列**：模板中不能引用 PREDICT 列本身（其值尚未计算）

## 6. 性能考虑

- 每次 LLM 调用都是同步阻塞的，耗时取决于 API 响应速度
- 建议对大批量数据使用 `predict_timing = deferred` 配合异步推理
- 历史对话查询会增加 SPI 开销，建议 history_count 不超过 10
- 整行数据格式化可能产生较长的 prompt，注意 token 限制

## 7. 结论

pg_predict 扩展使用火山引擎豆包大模型真实 API 进行端到端测试，所有 10 项测试用例全部通过，包括：
- `{{column_name}}` 模板语法正确替换（单列、多列）
- 整行数据自动格式化为键值对（无模板模式）
- 历史对话正确构建，LLM 能引用上下文
- INTEGER 类型自动转换
- 错误处理正确
