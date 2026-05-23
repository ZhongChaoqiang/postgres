-- ================================================
-- PostgreSQL PREDICT 全量功能测试脚本
-- 测试日期: 2026-05-23
-- ================================================

\echo '========================================'
\echo '1. 基础功能测试'
\echo '========================================'

\echo '--- 1.1 扩展安装验证 ---'
DROP EXTENSION IF EXISTS jolix_predict;
CREATE EXTENSION jolix_predict;
SELECT extname, extversion FROM pg_extension WHERE extname = 'jolix_predict';

\echo '--- 1.2 配置表验证 ---'
SELECT table_name FROM information_schema.tables
WHERE table_name IN ('jolix_llm_config', 'jolix_llm_history')
ORDER BY table_name;

\echo '--- 1.3 GUC 参数验证 ---'
SHOW jolix_predict.llm_api_url;
SHOW jolix_predict.llm_api_key;
SHOW jolix_predict.llm_model;
SHOW jolix_predict.llm_timeout;
SHOW jolix_predict.llm_history_table;
SHOW jolix_predict.current_table;
SHOW jolix_predict.history_retention_days;

\echo '--- 1.4 GUC 参数修改验证 ---'
SET jolix_predict.llm_timeout = 120;
SHOW jolix_predict.llm_timeout;
SET jolix_predict.llm_history_table = 'my_history';
SHOW jolix_predict.llm_history_table;
SET jolix_predict.llm_history_table = 'default';
SET jolix_predict.llm_timeout = 60;

\echo '--- 1.5 函数签名验证 ---'
SELECT proname, pronargs FROM pg_proc WHERE proname LIKE 'llm_%' ORDER BY proname, pronargs;
SELECT count(*) as deleted_func_count FROM pg_proc WHERE proname IN ('llm_predict_ext', 'llm_rag_predict_ext', 'set_predict_config', 'get_predict_config');
SELECT count(*) as config_func_count FROM pg_proc WHERE proname IN ('set_llm_config', 'get_llm_config');

\echo '========================================'
\echo '2. LLM 配置测试'
\echo '========================================'

\echo '--- 2.1 系统级配置 ---'
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

\echo '--- 2.2 查看系统级配置 ---'
SELECT api_url, model_name, temperature, max_tokens, rag_similarity, rag_topn FROM get_llm_config();

\echo '========================================'
\echo '3. LLM 推理测试'
\echo '========================================'

\echo '--- 3.1 基本推理（2参数）---'
SET jolix_predict.llm_timeout = 300;
SELECT llm_infer(
    'You are a helpful assistant. Reply in one short sentence.',
    'What is PostgreSQL?'
) AS two_param_result;

\echo '--- 3.2 验证两参数版本自动记录历史 ---'
SELECT count(*) AS history_count_default FROM jolix_llm_history WHERE table_name = 'default';
SELECT table_name, role, left(content, 60) AS content_preview
FROM jolix_llm_history WHERE table_name = 'default' ORDER BY created_at DESC LIMIT 4;

\echo '--- 3.3 带历史记录推理（3参数）---'
SELECT clear_predict_history();

SELECT llm_infer(
    'You are a helpful assistant. Reply concisely.',
    'What is PostgreSQL?',
    2
) AS three_param_result;

\echo '--- 3.4 带历史记录和表名推理（4参数）---'
SELECT llm_infer(
    'You are a helpful assistant. Reply concisely.',
    'Tell me more about its features.',
    2,
    'test_infer'
) AS four_param_result;

\echo '--- 3.5 验证4参数版本历史记录 ---'
SELECT table_name, role, left(content, 60) AS content_preview
FROM jolix_llm_history WHERE table_name = 'test_infer' ORDER BY created_at;

\echo '--- 3.6 llm_infer_with_history 函数 ---'
SELECT llm_infer_with_history(
    'You are a helpful assistant. Reply concisely.',
    'What are the main features?',
    2,
    'test_infer'
) AS with_history_result;

\echo '========================================'
\echo '4. PREDICT 列测试'
\echo '========================================'

\echo '--- 4.1 基本 PREDICT 列 ---'
DROP TABLE IF EXISTS qa_test CASCADE;
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

\echo '--- 4.2 PREDICT 列带历史记录 ---'
DROP TABLE IF EXISTS chat_test CASCADE;
CREATE TABLE chat_test (
    id serial PRIMARY KEY,
    question text,
    answer text PREDICT AS (llm_infer(
        'You are a helpful assistant. Reply concisely.',
        question,
        3,
        'chat_test'
    )) STORED
) WITH (predict_timing = immediate);

INSERT INTO chat_test (question) VALUES ('What is machine learning?');
INSERT INTO chat_test (question) VALUES ('Give me an example.');
SELECT question, answer FROM chat_test;

\echo '--- 4.3 LLM 文本分类（多列引用）---'
DROP TABLE IF EXISTS test_llm_articles CASCADE;
CREATE TABLE test_llm_articles (
    id serial PRIMARY KEY,
    title text,
    content text,
    category text PREDICT AS (llm_infer(
        'Classify the following text into exactly one category: technology, sports, politics, entertainment. Reply with only the category name, nothing else.',
        'Classify this text: ' || title || '. ' || content
    )) STORED
) WITH (predict_timing=immediate);

INSERT INTO test_llm_articles (title, content) VALUES ('AI Revolution', 'AI and machine learning are transforming software development');
INSERT INTO test_llm_articles (title, content, category) VALUES ('Box Office Hit', 'The new movie broke box office records', 'entertainment');
SELECT id, title, content, category FROM test_llm_articles;

\echo '--- 4.4 优先级分类 ---'
DROP TABLE IF EXISTS test_llm_tickets CASCADE;
CREATE TABLE test_llm_tickets (
    id serial PRIMARY KEY,
    product text,
    description text,
    priority text PREDICT AS (llm_infer(
        'Classify the priority. Reply with exactly one word: critical, high, medium, or low.',
        'Product: ' || product || '. Issue: ' || description
    )) STORED
) WITH (predict_timing=immediate);

INSERT INTO test_llm_tickets (product, description) VALUES ('Database', 'Production database is down, all users affected.');
INSERT INTO test_llm_tickets (product, description) VALUES ('Dashboard', 'Feature request: add dark mode to the dashboard.');
SELECT id, product, description, priority FROM test_llm_tickets;

\echo '--- 4.5 垃圾邮件检测 ---'
DROP TABLE IF EXISTS test_llm_emails CASCADE;
CREATE TABLE test_llm_emails (
    id serial PRIMARY KEY,
    sender text,
    subject text,
    body text,
    is_spam text PREDICT AS (llm_infer(
        'Determine if this email is spam. Reply with exactly one word: spam or not_spam.',
        'From: ' || sender || '. Subject: ' || subject || '. Body: ' || body
    )) STORED
) WITH (predict_timing=immediate);

INSERT INTO test_llm_emails (sender, subject, body) VALUES ('prize@scam.com', 'Congratulations! You won $1M', 'Click here to claim your prize now!');
INSERT INTO test_llm_emails (sender, subject, body) VALUES ('colleague@company.com', 'Meeting Tomorrow', 'Hi, just a reminder about our meeting at 3pm.');
SELECT id, sender, subject, is_spam FROM test_llm_emails;

\echo '========================================'
\echo '5. RAG 推理测试'
\echo '========================================'

\echo '--- 5.1 创建 RAG 表 ---'
DROP TABLE IF EXISTS rag_test CASCADE;
CREATE TABLE rag_test (
    id serial PRIMARY KEY,
    content text EMBEDDING AS (simple_embedding(content)) STORED,
    answer text PREDICT AS (llm_rag_infer(
        'Answer questions based on the provided context. Reply in one short sentence.',
        content
    )) STORED
) WITH (predict_timing = immediate, vector_len = 3);

\echo '--- 5.2 插入知识数据 ---'
INSERT INTO rag_test (content) VALUES ('PostgreSQL is a powerful open source database system');
INSERT INTO rag_test (content) VALUES ('Python is a popular programming language for data science');
INSERT INTO rag_test (content) VALUES ('Machine learning models can be trained on large datasets');
SELECT id, content, content_embedding FROM rag_test;

\echo '--- 5.3 RAG 推理 ---'
INSERT INTO rag_test (content) VALUES ('What is PostgreSQL?');
SELECT id, content, answer FROM rag_test WHERE content = 'What is PostgreSQL?';

\echo '========================================'
\echo '6. 历史记录测试'
\echo '========================================'

\echo '--- 6.1 自动记录验证 ---'
SELECT table_name, role, left(content, 60) AS content_preview
FROM jolix_llm_history ORDER BY created_at DESC LIMIT 10;

\echo '--- 6.2 手动记录 ---'
SELECT record_predict_history('manual_test', 'user', 'Test question');
SELECT record_predict_history('manual_test', 'assistant', 'Test answer');
SELECT table_name, role, content FROM jolix_llm_history WHERE table_name = 'manual_test';

\echo '--- 6.3 清除历史 ---'
SELECT clear_predict_history('manual_test');
SELECT count(*) AS remaining FROM jolix_llm_history WHERE table_name = 'manual_test';

\echo '--- 6.4 历史记录过期清理 ---'
SHOW jolix_predict.history_retention_days;
SELECT record_predict_history('test_cleanup', 'user', 'current question');
INSERT INTO jolix_llm_history (table_name, role, content, created_at)
VALUES ('test_cleanup', 'user', 'old question', now() - interval '8 days');
SELECT cleanup_predict_history();
SELECT table_name, role, content FROM jolix_llm_history WHERE table_name = 'test_cleanup';

\echo '--- 6.5 动态调整保留天数 ---'
SET jolix_predict.history_retention_days = 3;
INSERT INTO jolix_llm_history (table_name, role, content, created_at)
VALUES ('test_cleanup', 'user', 'mid question', now() - interval '4 days');
SELECT cleanup_predict_history();
SELECT table_name, role, content FROM jolix_llm_history WHERE table_name = 'test_cleanup';

SET jolix_predict.history_retention_days = 0;
SELECT cleanup_predict_history();

SET jolix_predict.history_retention_days = 7;

\echo '========================================'
\echo '7. Embedding 功能测试'
\echo '========================================'

\echo '--- 7.1 simple_embedding 函数 ---'
SELECT simple_embedding('hello world');

\echo '--- 7.2 EMBEDDING AS 语法 ---'
DROP TABLE IF EXISTS emb_test CASCADE;
CREATE TABLE emb_test (
    id serial PRIMARY KEY,
    content text EMBEDDING AS (simple_embedding(content)) STORED
) WITH (vector_len = 3);

INSERT INTO emb_test (content) VALUES ('Test embedding');
INSERT INTO emb_test (content) VALUES ('Another test');
SELECT id, content, content_embedding FROM emb_test;

\echo '========================================'
\echo '8. RAG + Embedding + Predict 集成测试'
\echo '========================================'

\echo '--- 8.1 完整集成测试 ---'
SELECT clear_predict_history();

DROP TABLE IF EXISTS rag_integration CASCADE;
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

\echo '--- 8.2 历史记录验证 ---'
SELECT table_name, role, left(content, 60) AS content_preview
FROM jolix_llm_history
WHERE table_name = 'rag_integration'
ORDER BY created_at;

\echo '========================================'
\echo '9. llm_history_table GUC 参数专项测试'
\echo '========================================'

\echo '--- 9.1 默认值验证 ---'
SHOW jolix_predict.llm_history_table;

\echo '--- 9.2 两参数版本自动记录到默认表 ---'
SELECT clear_predict_history();
SELECT llm_infer(
    'You are a helpful assistant. Reply in one word.',
    'What color is the sky?'
) AS sky_result;
SELECT count(*) AS default_history_count FROM jolix_llm_history WHERE table_name = 'default';

\echo '--- 9.3 修改 llm_history_table 后自动记录到新表 ---'
SET jolix_predict.llm_history_table = 'custom_history';
SELECT llm_infer(
    'You are a helpful assistant. Reply in one word.',
    'What color is grass?'
) AS grass_result;
SELECT count(*) AS custom_history_count FROM jolix_llm_history WHERE table_name = 'custom_history';

\echo '--- 9.4 禁用自动记录 ---'
SET jolix_predict.llm_history_table = '';
SELECT llm_infer(
    'You are a helpful assistant. Reply in one word.',
    'What is 2+2?'
) AS math_result;
SELECT count(*) AS no_record_count FROM jolix_llm_history WHERE table_name = '';

\echo '--- 9.5 恢复默认值 ---'
SET jolix_predict.llm_history_table = 'default';

\echo '========================================'
\echo '测试完成'
\echo '========================================'

SELECT clear_predict_history();
