-- ============================================================
-- RAG 推理函数测试脚本
-- LLM: 火山引擎 Ark API (ep-20251128103853-pp9jw)
-- ============================================================

SET pg_predict.api_url = 'https://ark.cn-beijing.volces.com/api/v3/chat/completions';
SET pg_predict.api_key = 'acc96ba1-d743-45d7-9b0e-a415bd96a046';
SET pg_predict.model = 'ep-20251128103853-pp9jw';
SET pg_predict.temperature = 0.3;
SET pg_predict.max_tokens = 512;

-- ============================================================
-- 准备 RAG 知识库表
-- ============================================================
DROP TABLE IF EXISTS rag_examples CASCADE;
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

SELECT 'RAG-SETUP' AS test_id, COUNT(*) AS rag_rows FROM rag_examples;

-- ============================================================
-- TC-RAG1: llm_rag_infer RAG 推理
-- ============================================================
SELECT 'TC-RAG1' AS test_id, llm_rag_infer(
    '你是一个数据分析SQL生成助手。根据检索到的示例，生成对应的MySQL查询SQL。只输出SQL，不要包含任何解释。',
    0,
    '上周生产情况',
    'rag_examples'::regclass,
    0.5,
    3
) AS result;

-- ============================================================
-- TC-RAG2: llm_rag_predict PREDICT 列自动推理
-- ============================================================
DROP TABLE IF EXISTS production_qa CASCADE;
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
    0.3,
    512,
    '你是一个数据分析SQL生成助手。根据检索到的示例，生成对应的MySQL查询SQL。只输出SQL，不要包含任何解释。',
    '{{question}}',
    0,
    'rag_examples'::regclass,
    0.5,
    3
);

INSERT INTO production_qa (question) VALUES ('昨天生产情况');

SELECT 'TC-RAG2' AS test_id, id, question, sql_result FROM production_qa;

-- ============================================================
-- TC-RAG3: 类型转换 - INTEGER PREDICT + RAG
-- ============================================================
DROP TABLE IF EXISTS sentiment_test CASCADE;
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
    0.1,
    64,
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

SELECT 'TC-RAG3' AS test_id, id, review_text, sentiment_score, pg_typeof(sentiment_score) AS actual_type FROM sentiment_test;

-- ============================================================
-- TC-RAG4: 类型转换 - FLOAT8 PREDICT + RAG
-- ============================================================
DROP TABLE IF EXISTS score_test CASCADE;
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
    0.1,
    64,
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

SELECT 'TC-RAG4' AS test_id, id, student_name, math_score, pg_typeof(math_score) AS actual_type FROM score_test;

-- ============================================================
-- TC-RAG5: 类型转换 - BOOLEAN PREDICT + RAG
-- ============================================================
DROP TABLE IF EXISTS spam_test CASCADE;
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
    0.1,
    64,
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

SELECT 'TC-RAG5' AS test_id, id, email_content, is_spam, pg_typeof(is_spam) AS actual_type FROM spam_test;

-- ============================================================
-- TC-RAG6: 异常处理 - RAG 表无 EMBEDDING 列
-- ============================================================
DROP TABLE IF EXISTS no_embedding_table CASCADE;
CREATE TABLE no_embedding_table (
    id SERIAL PRIMARY KEY,
    name TEXT
);

DO $$
BEGIN
    PERFORM llm_rag_infer(
        'test',
        0,
        'test query',
        'no_embedding_table'::regclass,
        0.5,
        3
    );
    RAISE EXCEPTION 'TC-RAG6 FAILED: should have raised error';
EXCEPTION WHEN OTHERS THEN
    RAISE NOTICE 'TC-RAG6: Got expected error: %', SQLERRM;
END $$;

-- ============================================================
-- TC-RAG7: 内置函数验证 - llm_predict 和 llm_rag_predict 在 pg_catalog 中
-- ============================================================
SELECT 'TC-RAG7' AS test_id, proname, pronamespace::regnamespace, oid
FROM pg_proc WHERE proname IN ('llm_predict', 'llm_rag_predict') AND pronamespace = (SELECT oid FROM pg_namespace WHERE nspname = 'pg_catalog')
ORDER BY proname;
