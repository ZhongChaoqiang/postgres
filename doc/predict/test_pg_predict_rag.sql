-- ================================================
-- PostgreSQL RAG Predict 测试脚本
-- ================================================

-- 1. 配置 LLM API 和 RAG 参数
SELECT set_llm_config(
    p_api_url := 'https://ark.cn-beijing.volces.com/api/v3/chat/completions',
    p_api_key := 'your-api-key',
    p_model_name := 'your-model-endpoint',
    p_rag_similarity := 2.0,
    p_rag_topn := 3
);

-- 2. 查看配置
SELECT api_url, model_name, rag_similarity, rag_topn FROM get_llm_config();

-- 3. 创建 RAG 测试表（使用 simple_embedding 避免外部依赖）
CREATE TABLE rag_test (
    id serial PRIMARY KEY,
    content text EMBEDDING AS (simple_embedding(content)) STORED,
    answer text PREDICT AS (llm_rag_infer(
        'Answer questions based on the provided context. Reply in one short sentence.',
        content
    )) STORED
) WITH (predict_timing = immediate, vector_len = 3);

-- 4. 插入知识数据
INSERT INTO rag_test (content) VALUES ('PostgreSQL is a powerful open source database system');
INSERT INTO rag_test (content) VALUES ('Python is a popular programming language for data science');
INSERT INTO rag_test (content) VALUES ('Machine learning models can be trained on large datasets');

-- 5. 查看 embedding 数据
SELECT id, content, content_embedding FROM rag_test;

-- 6. 触发 RAG 推理
INSERT INTO rag_test (content) VALUES ('What is PostgreSQL?');
SELECT id, content, answer FROM rag_test WHERE content = 'What is PostgreSQL?';

-- 7. 查看历史记录
SELECT table_name, role, substring(content, 1, 80) as content_preview
FROM jolix_predict_history
WHERE table_name = 'rag_test'
ORDER BY created_at;

-- 8. 验证函数签名
SELECT proname FROM pg_proc WHERE proname LIKE 'llm_%' ORDER BY proname;
SELECT count(*) as config_func_count FROM pg_proc WHERE proname IN ('set_llm_config', 'get_llm_config');
SELECT count(*) as deleted_func_count FROM pg_proc WHERE proname IN ('llm_predict_ext', 'llm_rag_predict_ext', 'set_predict_config', 'get_predict_config');

-- 9. 清理
SELECT clear_predict_history();
