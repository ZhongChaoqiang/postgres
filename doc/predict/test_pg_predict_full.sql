-- ============================================================
-- pg_predict 完整测试脚本
-- 包含 PG_PREDICT_TEST_REPORT.md 中的 TC-01 到 TC-10
-- 以及 RAG 推理函数测试
-- LLM: 火山引擎 Ark API (ep-20251128103853-pp9jw)
-- ============================================================

-- ============================================================
-- TC-01: 扩展安装与系统级配置
-- ============================================================
DROP EXTENSION IF EXISTS pg_predict CASCADE;
CREATE EXTENSION pg_predict;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_embedding_st;

SELECT set_predict_config(
    'https://ark.cn-beijing.volces.com/api/v3/chat/completions',
    'acc96ba1-d743-45d7-9b0e-a415bd96a046',
    'ep-20251128103853-pp9jw',
    0.3,
    256,
    'You are a helpful assistant.',
    0
);

SELECT 'TC-01' AS test_id, api_url, model_name, temperature, max_tokens, system_prompt, history_count
FROM get_predict_config();

-- ============================================================
-- TC-02: llm_infer 独立推理
-- ============================================================
SET pg_predict.api_url = 'https://ark.cn-beijing.volces.com/api/v3/chat/completions';
SET pg_predict.api_key = 'acc96ba1-d743-45d7-9b0e-a415bd96a046';
SET pg_predict.model = 'ep-20251128103853-pp9jw';
SET pg_predict.temperature = 0.3;
SET pg_predict.max_tokens = 100;

SELECT 'TC-02' AS test_id, llm_infer('You are a helpful assistant. Reply in one word.', 'What is the capital of France?') AS result;

-- ============================================================
-- TC-03: PREDICT 列 + 单列 prompt_template 文本分类
-- ============================================================
DROP TABLE IF EXISTS test_llm_articles CASCADE;
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
    'acc96ba1-d743-45d7-9b0e-a415bd96a046',
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

SELECT 'TC-03' AS test_id, id, content, category FROM test_llm_articles;

-- ============================================================
-- TC-04: PREDICT 列 + 多列 prompt_template 情感分析
-- ============================================================
DROP TABLE IF EXISTS test_llm_reviews CASCADE;
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
    'acc96ba1-d743-45d7-9b0e-a415bd96a046',
    'ep-20251128103853-pp9jw',
    0.1,
    32,
    'Analyze the sentiment of the review. Reply with exactly one word: positive, negative, or neutral.',
    'Review of {{product_name}}: {{review_text}}',
    0
);

INSERT INTO test_llm_reviews (review_text, product_name) VALUES ('This product is amazing! I love it!', 'Widget Pro');
INSERT INTO test_llm_reviews (review_text, product_name) VALUES ('Terrible quality, waste of money', 'CheapGadget');

SELECT 'TC-04' AS test_id, id, product_name, review_text, sentiment FROM test_llm_reviews;

-- ============================================================
-- TC-05: PREDICT 列 + 无模板（prompt_template=NULL）自动行格式化
-- ============================================================
DROP TABLE IF EXISTS test_llm_tickets CASCADE;
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
    'acc96ba1-d743-45d7-9b0e-a415bd96a046',
    'ep-20251128103853-pp9jw',
    0.1,
    16,
    'Assign a priority level based on the ticket information. Reply with exactly one word: high, medium, or low.',
    NULL,
    0
);

INSERT INTO test_llm_tickets (subject, description) VALUES ('System down', 'Production server is not responding, all users affected');
INSERT INTO test_llm_tickets (subject, description) VALUES ('Font issue', 'The font on the about page looks slightly different');

SELECT 'TC-05' AS test_id, id, subject, description, priority FROM test_llm_tickets;

-- ============================================================
-- TC-06: PREDICT 列 + prompt_template + 历史对话
-- ============================================================
DROP TABLE IF EXISTS test_llm_chat CASCADE;
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
    'acc96ba1-d743-45d7-9b0e-a415bd96a046',
    'ep-20251128103853-pp9jw',
    0.7,
    128,
    'You are a helpful customer service assistant for an online store. Keep your replies concise.',
    '{{user_message}}',
    3
);

INSERT INTO test_llm_chat (user_message) VALUES ('Hello, I need help with my order');
SELECT 'TC-06a' AS test_id, id, user_message, assistant_reply FROM test_llm_chat;

INSERT INTO test_llm_chat (user_message) VALUES ('My order number is 12345, it has not arrived yet');
SELECT 'TC-06b' AS test_id, id, user_message, assistant_reply FROM test_llm_chat;

INSERT INTO test_llm_chat (user_message) VALUES ('When can I expect it?');
SELECT 'TC-06c' AS test_id, id, user_message, assistant_reply FROM test_llm_chat;

-- ============================================================
-- TC-07: INTEGER 类型自动转换
-- ============================================================
DROP TABLE IF EXISTS test_llm_price CASCADE;
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
    'acc96ba1-d743-45d7-9b0e-a415bd96a046',
    'ep-20251128103853-pp9jw',
    0.1,
    16,
    'Estimate the price in dollars. Reply with only the number.',
    'Product: {{product_name}}, Description: {{description}}',
    0
);

INSERT INTO test_llm_price (product_name, description) VALUES ('Widget Pro', 'A high-end widget that costs 499 dollars');

SELECT 'TC-07' AS test_id, id, product_name, price, pg_typeof(price) AS actual_type FROM test_llm_price;

-- ============================================================
-- TC-08: GUC 参数动态配置
-- ============================================================
SET pg_predict.api_url = 'https://ark.cn-beijing.volces.com/api/v3/chat/completions';
SET pg_predict.api_key = 'acc96ba1-d743-45d7-9b0e-a415bd96a046';
SET pg_predict.model = 'ep-20251128103853-pp9jw';
SET pg_predict.temperature = 0.3;
SET pg_predict.max_tokens = 100;

SELECT 'TC-08' AS test_id,
    current_setting('pg_predict.api_url') AS api_url,
    current_setting('pg_predict.model') AS model,
    current_setting('pg_predict.temperature') AS temperature,
    current_setting('pg_predict.max_tokens') AS max_tokens;

-- ============================================================
-- TC-09: API URL 未配置错误处理
-- ============================================================
RESET pg_predict.api_url;
DO $$
BEGIN
    PERFORM llm_infer('test', 'hello');
    RAISE EXCEPTION 'TC-09 FAILED: should have raised error';
EXCEPTION WHEN OTHERS THEN
    RAISE NOTICE 'TC-09: Got expected error: %', SQLERRM;
END $$;

-- ============================================================
-- TC-10: API 不可达错误处理
-- ============================================================
SET pg_predict.api_url = 'http://invalid-host-nonexist.example.com/v1/chat/completions';
SET pg_predict.api_key = 'test-key';
SET pg_predict.model = 'test-model';
DO $$
BEGIN
    PERFORM llm_infer('test', 'hello');
    RAISE EXCEPTION 'TC-10 FAILED: should have raised error';
EXCEPTION WHEN OTHERS THEN
    RAISE NOTICE 'TC-10: Got expected error: %', SQLERRM;
END $$;
