-- ================================================
-- LLM RAG Predict 示例脚本
-- ================================================

-- 1. 配置 LLM API 和 RAG 参数
SELECT set_llm_config(
    p_api_url := 'https://ark.cn-beijing.volces.com/api/v3/chat/completions',
    p_api_key := 'your-api-key',
    p_model_name := 'your-model-endpoint',
    p_rag_similarity := 2.0,
    p_rag_topn := 3
);

-- 2. 创建同时包含 EMBEDDING 列和 PREDICT 列的表
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

-- 3. 插入知识数据（自动生成 embedding）
INSERT INTO rag_knowledge (content) VALUES ('PostgreSQL is a powerful open source database system');
INSERT INTO rag_knowledge (content) VALUES ('Python is a popular programming language for data science');
INSERT INTO rag_knowledge (content) VALUES ('Machine learning models can be trained on large datasets');

-- 4. 插入问题（自动触发 RAG 推理）
INSERT INTO rag_knowledge (content) VALUES ('What is PostgreSQL?');

-- 5. 查看结果
SELECT id, content, answer FROM rag_knowledge WHERE content = 'What is PostgreSQL?';

-- 6. 查看历史记录
SELECT table_name, role, substring(content, 1, 80) as content_preview
FROM jolix_llm_history
WHERE table_name = 'rag_knowledge'
ORDER BY created_at;

-- 7. RAG 推理带历史记录
CREATE TABLE rag_chat (
    id serial PRIMARY KEY,
    content text EMBEDDING AS (st_embedding(content)) STORED,
    answer text PREDICT AS (llm_rag_infer(
        'Answer questions based on the provided context. Reply concisely.',
        content,
        2
    )) STORED
) WITH (
    predict_timing = immediate,
    vector_len = 384
);

INSERT INTO rag_chat (content) VALUES ('PostgreSQL supports advanced indexing including GiST, GIN, and BRIN');
INSERT INTO rag_chat (content) VALUES ('Python has libraries like NumPy, Pandas, and Scikit-learn');
INSERT INTO rag_chat (content) VALUES ('What indexing does PostgreSQL support?');

SELECT id, content, answer FROM rag_chat WHERE content = 'What indexing does PostgreSQL support?';
