-- ================================================
-- PostgreSQL PREDICT 功能完整测试脚本
-- ================================================

-- 1. 配置 LLM API
SELECT set_llm_config(
    p_api_url := 'https://ark.cn-beijing.volces.com/api/v3/chat/completions',
    p_api_key := 'your-api-key',
    p_model_name := 'your-model-endpoint',
    p_temperature := 0.7,
    p_max_tokens := 1024
);

-- 2. 查看配置
SELECT api_url, model_name, temperature, max_tokens FROM get_llm_config();

-- 3. 基本 LLM 推理
SELECT llm_infer(
    'You are a helpful assistant. Reply in one short sentence.',
    'What is PostgreSQL?'
);

-- 4. 带历史记录的推理
SELECT clear_predict_history();

SELECT llm_infer(
    'You are a helpful assistant. Reply concisely.',
    'What is PostgreSQL?',
    2,
    'test_history'
);

SELECT llm_infer(
    'You are a helpful assistant. Reply concisely.',
    'Tell me more about its features.',
    2,
    'test_history'
);

-- 5. 查看历史记录
SELECT table_name, role, substring(content, 1, 60) as content_preview
FROM jolix_llm_history
WHERE table_name = 'test_history'
ORDER BY created_at;

-- 6. PREDICT 列测试
CREATE TABLE qa_test (
    id serial PRIMARY KEY,
    question text,
    answer text PREDICT AS (llm_infer(
        'Answer the question in one short sentence.',
        question
    )) STORED
) WITH (predict_timing = immediate);

INSERT INTO qa_test (question) VALUES ('What is Python?');
SELECT question, answer FROM qa_test;

-- 7. PREDICT 列带历史记录
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

-- 8. 清理
SELECT clear_predict_history();
