-- ================================================================
-- PostgreSQL 18.3 综合回归测试脚本
-- 覆盖: PREDICT / EMBEDDING / EMBEDDINGS / RAG / st_embedding / ft_transformer_embedding / ldm_infer / 创建时序向量表 (CREATE TABLE ... WITH timeseries.*) / ts2v_moment / timeseries_vector_run
-- 日期: 2026-06-10（基础功能），2026-06-20（新增 ldm_infer），2026-07-15（新增时序向量表），2026-07-16（新增 ts2v_moment 和 timeseries_vector_run），2026-09-11（时序向量表改为标准 CREATE TABLE + reloptions 方案）
-- ================================================================
-- 使用方法:
--   psql -h localhost -p 5433 -U postgres -d postgres -f regression_test.sql
-- 前置条件:
--   1. 已安装 postgresql-18 deb 包（含 jolix_predict, jolix_embedding, vector 扩展）
--   2. 已全局安装 sentence-transformers: sudo -H pip3 install sentence-transformers
--   3. 已创建模型目录: /usr/local/pgsql/models (postgres用户可写)
--   4. 网络可访问 LLM API
--   5. 第11部分需要 pgvector 和 tsvector_funcs 扩展
-- ================================================================

\set ON_ERROR_STOP off

-- ================================================================
-- 第1部分: 扩展与配置验证
-- ================================================================
\echo '============================================================'
\echo '第1部分: 扩展与配置验证'
\echo '============================================================'

\echo '--- 1.1 扩展安装验证 ---'
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS tsvector_funcs;
SELECT extname, extversion FROM pg_extension
WHERE extname IN ('jolix_predict', 'jolix_embedding', 'vector', 'tsvector_funcs', 'plpgsql')
ORDER BY extname;

\echo '--- 1.2 GUC 参数验证（需先加载扩展）---'
LOAD 'jolix_predict';
LOAD 'jolix_embedding';
SHOW jolix_predict.llm_api_url;
SHOW jolix_predict.llm_model;
SHOW jolix_predict.llm_timeout;
SHOW jolix_predict.current_table;
SHOW jolix_embedding.model_name;
SHOW jolix_embedding.ft_model_name;
SHOW jolix_embedding.ft_vector_len;

\echo '--- 1.3 函数签名验证 ---'
SELECT proname, pronargs FROM pg_proc
WHERE proname IN ('llm_infer', 'llm_rag_infer', 'set_llm_config', 'get_llm_config',
                   'st_embedding', 'ft_transformer_embedding',
                   'record_predict_history', 'clear_predict_history', 'cleanup_predict_history',
                   'ts2v_moment', 'timeseries_vector_run')
ORDER BY proname, pronargs;

\echo '--- 1.4 配置 LLM API ---'
SELECT set_llm_config(
    p_api_url := 'https://ark.cn-beijing.volces.com/api/v3/chat/completions',
    p_api_key := 'acc96ba1-d743-45d7-9b0e-a415bd96a046',
    p_model_name := 'ep-20251128103853-pp9jw',
    p_temperature := 0.7,
    p_max_tokens := 256,
    p_system_prompt := 'You are a helpful assistant.',
    p_rag_similarity := 2.0,
    p_rag_topn := 3
);

SET jolix_predict.llm_timeout = 300;

\echo '--- 1.5 查看配置 ---'
SELECT api_url, model_name, temperature, max_tokens, rag_similarity, rag_topn FROM get_llm_config();

-- ================================================================
-- 第2部分: PREDICT 功能测试
-- ================================================================
\echo '============================================================'
\echo '第2部分: PREDICT 功能测试'
\echo '============================================================'

\echo '--- 2.1 llm_infer 基本推理（2参数）---'
SELECT llm_infer(
    'Reply in one word.',
    'What color is the sky?'
) AS sky_result;

\echo '--- 2.2 llm_infer 带历史记录推理（4参数）---'
SELECT llm_infer(
    'Reply concisely.',
    'What is PostgreSQL?',
    2,
    'regression_test'
) AS pg_result;

\echo '--- 2.3 PREDICT 列 - 基本用法 ---'
DROP TABLE IF EXISTS pred_basic CASCADE;
CREATE TABLE pred_basic (
    id serial PRIMARY KEY,
    question text,
    answer text PREDICT AS (llm_infer(
        'Answer in one short sentence.',
        question
    )) STORED
) WITH (predict_timing = immediate);

INSERT INTO pred_basic (question) VALUES ('What is Python?');
SELECT id, question, answer FROM pred_basic;

\echo '--- 2.4 PREDICT 列 - 多列引用 ---'
DROP TABLE IF EXISTS pred_multi CASCADE;
CREATE TABLE pred_multi (
    id serial PRIMARY KEY,
    product text,
    description text,
    priority text PREDICT AS (llm_infer(
        'Classify priority. Reply with exactly one word: critical, high, medium, or low.',
        'Product: ' || product || '. Issue: ' || description
    )) STORED
) WITH (predict_timing = immediate);

INSERT INTO pred_multi (product, description) VALUES ('Database', 'Production database is down');
INSERT INTO pred_multi (product, description) VALUES ('Dashboard', 'Feature request: dark mode');
SELECT id, product, description, priority FROM pred_multi;

\echo '--- 2.5 PREDICT 列 - 延迟推理（predict_timing = deferred，SELECT INFER 时按需推理）---'
DROP TABLE IF EXISTS pred_deferred CASCADE;
CREATE TABLE pred_deferred (
    id serial PRIMARY KEY,
    question text,
    answer text PREDICT AS (llm_infer(
        'Answer in one short sentence.',
        question
    )) STORED
) WITH (predict_timing = deferred);

\echo '--- 2.5.1 插入数据（answer 列应为 NULL，deferred 模式不自动推理）---'
INSERT INTO pred_deferred (question) VALUES ('What is Java?');
INSERT INTO pred_deferred (question) VALUES ('What is Linux?');

\echo '--- 2.5.2 默认 SELECT（跳过推理，answer 应为 NULL）---'
SELECT id, question, answer FROM pred_deferred;

\echo '--- 2.5.3 SELECT INFER（触发按需推理，answer 应有值）---'
SELECT INFER id, question, answer FROM pred_deferred;

\echo '--- 2.5.4 验证 predict_timing 选项值 ---'
SELECT c.relname, reloptions
FROM pg_class c
WHERE c.relname = 'pred_deferred';

\echo '--- 2.5.5 验证推理结果已持久化（再次默认查询应直接返回结果）---'
SELECT id, question, answer FROM pred_deferred;

\echo '--- 2.5.6 SELECT INFER ... WHERE 条件过滤未推理行 ---'
INSERT INTO pred_deferred (question) VALUES ('What is Python?');
SELECT INFER id, question FROM pred_deferred WHERE answer IS NULL;

\echo '--- 2.5.7 验证新行已推理 ---'
SELECT id, question, answer FROM pred_deferred;

\echo '--- 2.6 PREDICT 列 - deferred 模式下手动提供值 ---'
DROP TABLE IF EXISTS pred_deferred_manual CASCADE;
CREATE TABLE pred_deferred_manual (
    id serial PRIMARY KEY,
    question text,
    answer text PREDICT AS (llm_infer(
        'Answer in one word.',
        question
    )) STORED
) WITH (predict_timing = deferred);

INSERT INTO pred_deferred_manual (question, answer) VALUES ('What color is the sky?', 'blue');
SELECT id, question, answer FROM pred_deferred_manual;

\echo '--- 2.7 PREDICT 列 - deferred 模式 UPDATE 后 SELECT INFER 触发重新计算 ---'
UPDATE pred_deferred SET question = 'What is Golang?' WHERE id = 1;
SELECT INFER id, question, answer FROM pred_deferred WHERE id = 1;

-- ================================================================
-- 第3部分: EMBEDDING 功能测试（st_embedding）
-- ================================================================
\echo '============================================================'
\echo '第3部分: EMBEDDING 功能测试（st_embedding）'
\echo '============================================================'

\echo '--- 3.1 st_embedding 函数直接调用 ---'
SELECT vector_dims(st_embedding('Hello, world!')) AS dims;

\echo '--- 3.2 EMBEDDING 列 - 文本向量化 ---'
DROP TABLE IF EXISTS emb_text CASCADE;
CREATE TABLE emb_text (
    id serial PRIMARY KEY,
    content text EMBEDDING AS (st_embedding(content)) STORED
) WITH (vector_len = 384);

INSERT INTO emb_text (content) VALUES ('PostgreSQL is a powerful database');
INSERT INTO emb_text (content) VALUES ('Machine learning transforms industries');
INSERT INTO emb_text (content) VALUES ('Open source software drives innovation');

SELECT id, content, vector_dims(content_embedding) AS dims FROM emb_text ORDER BY id;

\echo '--- 3.3 EMBEDDING 列 - 语义搜索 ---'
SELECT id, content,
       round((content <=> 'database technology')::numeric, 4) AS distance
FROM emb_text ORDER BY distance;

\echo '--- 3.4 EMBEDDING 列 - UPDATE 自动更新向量 ---'
UPDATE emb_text SET content = 'PostgreSQL 18 adds AI features' WHERE id = 1;
SELECT id, content, vector_dims(content_embedding) AS dims FROM emb_text WHERE id = 1;

\echo '--- 3.5 EMBEDDING 列 - NULL 值处理 ---'
INSERT INTO emb_text (id, content) VALUES (100, NULL);
SELECT id, content, content_embedding IS NULL AS is_null FROM emb_text WHERE id = 100;

-- ================================================================
-- 第4部分: EMBEDDINGS 功能测试（ft_transformer_embedding）
-- ================================================================
\echo '============================================================'
\echo '第4部分: EMBEDDINGS 功能测试（ft_transformer_embedding）'
\echo '============================================================'

\echo '--- 4.1 ft_transformer_embedding 函数直接调用 ---'
SELECT vector_dims(ft_transformer_embedding(25, 50000.0, 'student'::text)) AS dims;

\echo '--- 4.2 EMBEDDINGS 列级语法（CREATE TABLE 中定义）---'
DROP TABLE IF EXISTS embd_column CASCADE;
CREATE TABLE embd_column (
    id int PRIMARY KEY,
    age int,
    income float8,
    category text,
    demographic vector EMBEDDINGS AS (ft_transformer_embedding(age, income, category))
) WITH (vector_len = 384);

INSERT INTO embd_column (id, age, income, category) VALUES (1, 25, 50000, 'student');
INSERT INTO embd_column (id, age, income, category) VALUES (2, 40, 120000, 'professional');
INSERT INTO embd_column (id, age, income, category) VALUES (3, 30, 75000, 'engineer');

SELECT id, age, income, category, vector_dims(demographic) AS dims FROM embd_column ORDER BY id;

\echo '--- 4.3 EMBEDDINGS 列级语法 - 相似性查询 ---'
SELECT id, age, income, category,
       round((demographic <=> (SELECT demographic FROM embd_column WHERE id = 1))::numeric, 4) AS distance
FROM embd_column WHERE id != 1 ORDER BY distance;

\echo '--- 4.4 CREATE EMBEDDINGS 语句（对已有表添加向量列）---'
DROP TABLE IF EXISTS embd_alter CASCADE;
CREATE TABLE embd_alter (
    id SERIAL PRIMARY KEY,
    price FLOAT8,
    brand TEXT
);

CREATE EMBEDDINGS product_vec ON embd_alter
    USING ft_transformer_embedding (price, brand)
    WITH (vector_len = 384);

INSERT INTO embd_alter (price, brand) VALUES (99.9, 'BrandA');
INSERT INTO embd_alter (price, brand) VALUES (199.9, 'BrandB');
INSERT INTO embd_alter (price, brand) VALUES (49.9, 'BrandC');

SELECT id, price, brand, vector_dims(product_vec) AS dims FROM embd_alter;

\echo '--- 4.5 CREATE EMBEDDINGS - 相似性查询 ---'
SELECT id, price, brand,
       round((product_vec <=> (SELECT product_vec FROM embd_alter WHERE id = 1))::numeric, 4) AS distance
FROM embd_alter WHERE id != 1 ORDER BY distance;

\echo '--- 4.6 UPDATE 自动重新计算向量 ---'
UPDATE embd_alter SET price = 149.9, brand = 'BrandA-Pro' WHERE id = 1;
SELECT id, price, brand, vector_dims(product_vec) AS dims FROM embd_alter WHERE id = 1;

\echo '--- 4.7 DROP EMBEDDINGS ---'
DROP EMBEDDINGS product_vec ON embd_alter;
SELECT count(*) = 0 AS col_dropped FROM pg_attribute
WHERE attrelid = 'embd_alter'::regclass AND attname = 'product_vec';

\echo '--- 4.8 CREATE EMBEDDINGS IF NOT EXISTS / DROP EMBEDDINGS IF EXISTS ---'
CREATE EMBEDDINGS test_vec ON embd_alter
    USING ft_transformer_embedding (price)
    WITH (vector_len = 384);

CREATE EMBEDDINGS IF NOT EXISTS test_vec ON embd_alter
    USING ft_transformer_embedding (price)
    WITH (vector_len = 384);

DROP EMBEDDINGS test_vec ON embd_alter;
DROP EMBEDDINGS IF EXISTS test_vec ON embd_alter;

\echo '--- 4.9 同表多 EMBEDDINGS ---'
DROP TABLE IF EXISTS embd_multi CASCADE;
CREATE TABLE embd_multi (
    id SERIAL PRIMARY KEY,
    age int,
    income float8,
    category text
);

CREATE EMBEDDINGS vec1 ON embd_multi
    USING ft_transformer_embedding (age)
    WITH (vector_len = 384);
CREATE EMBEDDINGS vec2 ON embd_multi
    USING ft_transformer_embedding (income)
    WITH (vector_len = 384);

INSERT INTO embd_multi (age, income, category) VALUES (30, 60000, 'standard');

SELECT id, age, income, vector_dims(vec1) AS dims1, vector_dims(vec2) AS dims2 FROM embd_multi;

DROP EMBEDDINGS vec1 ON embd_multi;
DROP EMBEDDINGS vec2 ON embd_multi;

\echo '--- 4.10 EMBEDDINGS 列级语法 - 不带类型名（第二替代分支）---'
DROP TABLE IF EXISTS embd_notype CASCADE;
CREATE TABLE embd_notype (
    id int PRIMARY KEY,
    age int,
    income float8,
    category text,
    demographic EMBEDDINGS AS (ft_transformer_embedding(age, income, category))
);

INSERT INTO embd_notype (id, age, income, category) VALUES (1, 25, 30000, 'basic');
SELECT id, age, income, category, vector_dims(demographic) AS dims FROM embd_notype;

\echo '--- 4.11 EMBEDDING AS 带 STORED（向后兼容）---'
DROP TABLE IF EXISTS embd_stored CASCADE;
CREATE TABLE embd_stored (
    id int PRIMARY KEY,
    info text,
    info_vec vector EMBEDDING AS (ft_transformer_embedding(info)) STORED
) WITH (vector_len = 384);

INSERT INTO embd_stored (id, info) VALUES (1, 'test stored keyword');
SELECT id, info, vector_dims(info_vec) AS dims FROM embd_stored;

\echo '--- 4.12 隐藏列属性验证 ---'
DROP TABLE IF EXISTS embd_meta CASCADE;
CREATE TABLE embd_meta (
    id int PRIMARY KEY,
    age int,
    income float8,
    category text,
    demographic vector EMBEDDINGS AS (ft_transformer_embedding(age, income, category))
) WITH (vector_len = 384);

INSERT INTO embd_meta (id, age, income, category) VALUES (1, 25, 50000, 'student');

SELECT attname, atthidden, attembeddings, attgenerated
FROM pg_attribute
WHERE attrelid = 'embd_meta'::regclass AND attname = 'demographic';

\echo '--- 4.13 触发器验证 ---'
SELECT tgname, tgenabled FROM pg_trigger
WHERE tgrelid = 'embd_meta'::regclass AND tgname LIKE 'embeddings_%';

\echo '--- 4.14 不同 vector_len 参数 ---'
DROP TABLE IF EXISTS embd_vlen CASCADE;
CREATE TABLE embd_vlen (
    id SERIAL PRIMARY KEY,
    age int,
    income float8
);

CREATE EMBEDDINGS vec768 ON embd_vlen
    USING ft_transformer_embedding (age)
    WITH (vector_len = 768);

INSERT INTO embd_vlen (age, income) VALUES (30, 50000);
SELECT id, vector_dims(vec768) AS dims FROM embd_vlen;

DROP EMBEDDINGS vec768 ON embd_vlen;
DROP TABLE embd_vlen;

\echo '--- 4.15 ft_transformer_embedding 函数调用形式相似性查询 ---'
SELECT id, age, income, category,
       demographic <=> ft_transformer_embedding(25::int, 50000.0::float8, 'student'::text) AS query_dist
FROM embd_meta
ORDER BY query_dist
LIMIT 3;

\echo '--- 4.16 跨表向量相似性查询 ---'
DROP TABLE IF EXISTS embd_cross CASCADE;
CREATE TABLE embd_cross (
    id SERIAL PRIMARY KEY,
    price FLOAT8,
    brand TEXT
);

CREATE EMBEDDINGS product_vec ON embd_cross
    USING ft_transformer_embedding (price, brand)
    WITH (vector_len = 384);

INSERT INTO embd_cross (price, brand) VALUES (99.9, 'BrandA');
INSERT INTO embd_cross (price, brand) VALUES (199.9, 'BrandB');

SELECT p.id, p.price, p.brand,
       p.product_vec <=> c.demographic AS cross_distance
FROM embd_cross p, embd_meta c
WHERE c.id = 1
ORDER BY cross_distance;

DROP EMBEDDINGS product_vec ON embd_cross;
DROP TABLE embd_cross;

\echo '--- 4.17 条件过滤 + EMBEDDINGS 向量搜索 ---'
INSERT INTO embd_meta (id, age, income, category) VALUES (2, 40, 120000, 'professional');
INSERT INTO embd_meta (id, age, income, category) VALUES (3, 30, 75000, 'engineer');

SELECT id, age, income, category,
       round((demographic <=> (SELECT demographic FROM embd_meta WHERE id = 2))::numeric, 4) AS distance
FROM embd_meta WHERE id != 2 AND category = 'student' OR (id != 2 AND income > 60000)
ORDER BY distance;

\echo '--- 4.18 st_embedding + ft_transformer 混合使用 ---'
DROP TABLE IF EXISTS embd_hybrid CASCADE;
CREATE TABLE embd_hybrid (
    id serial PRIMARY KEY,
    name text,
    age int,
    income float8,
    category text,
    name_vec text EMBEDDING AS (st_embedding(name)) STORED,
    feat_vec vector EMBEDDINGS AS (ft_transformer_embedding(age, income, category))
) WITH (vector_len = 384);

INSERT INTO embd_hybrid (name, age, income, category) VALUES ('Alice', 25, 50000, 'student');
INSERT INTO embd_hybrid (name, age, income, category) VALUES ('Bob', 40, 120000, 'professional');

SELECT id, name, vector_dims(name_vec_embedding) AS st_dims, vector_dims(feat_vec) AS ft_dims
FROM embd_hybrid ORDER BY id;

-- ================================================================
-- 第5部分: RAG 推理测试
-- ================================================================
\echo '============================================================'
\echo '第5部分: RAG 推理测试'
\echo '============================================================'

\echo '--- 5.1 RAG 表创建（EMBEDDING + PREDICT + RAG 三合一）---'
DROP TABLE IF EXISTS rag_demo CASCADE;
CREATE TABLE rag_demo (
    id serial PRIMARY KEY,
    product_name text,
    review_text text,
    sentiment text PREDICT AS (llm_infer(
        'Classify sentiment. Reply with exactly one word: positive, negative, or neutral.',
        'Product: ' || product_name || '. Review: ' || review_text
    )) STORED,
    review_emb text EMBEDDING AS (st_embedding(review_text)) STORED,
    rag_answer text PREDICT AS (llm_rag_infer(
        'Based on similar reviews, answer concisely in one sentence.',
        review_text
    )) STORED
) WITH (
    predict_timing = immediate,
    vector_len = 384
);

\echo '--- 5.2 插入知识数据 ---'
INSERT INTO rag_demo (product_name, review_text) VALUES
    ('iPhone 15 Pro', 'The camera quality is outstanding and the battery lasts all day.'),
    ('iPhone 15 Pro', 'The price is too high for what you get.'),
    ('MacBook Air M3', 'Incredibly fast and lightweight. Perfect for developers.'),
    ('AirPods Pro 2', 'Noise cancellation is top-notch. Sound quality improved.');

SELECT id, product_name, sentiment, vector_dims(review_emb_embedding) AS dims
FROM rag_demo ORDER BY id;

\echo '--- 5.3 RAG 推理 - 插入问题 ---'
INSERT INTO rag_demo (product_name, review_text) VALUES
    ('iPhone 15 Pro', 'How is the battery life of iPhone 15 Pro?');

SELECT id, product_name, review_text, rag_answer
FROM rag_demo WHERE rag_answer IS NOT NULL AND id = 5;

\echo '--- 5.4 语义搜索 + 情感过滤联合查询 ---'
SELECT id, product_name, left(review_text, 40) AS review_preview,
       sentiment,
       round((review_emb <=> 'great performance')::numeric, 4) AS relevance
FROM rag_demo
WHERE sentiment = 'positive'
ORDER BY review_emb <=> 'great performance'
LIMIT 3;

\echo '--- 5.5 RAG 上下文自动包含 _actual/_predict 隐藏列验证 ---'
-- 验证 llm_rag_infer 在构建 RAG 上下文时会显式查询并包含 PREDICT 列的
-- 隐藏伴生列（{col}_actual、{col}_predict），并在上下文头部添加说明。
-- 使用确定性 PREDICT 表达式 upper() 避免 LLM 调用，便于断言 _predict 值。
DROP TABLE IF EXISTS rag_ctx_test CASCADE;
CREATE TABLE rag_ctx_test (
    id serial PRIMARY KEY,
    content text EMBEDDING AS (st_embedding(content)) STORED,
    answer text PREDICT AS (upper(content)) STORED
) WITH (predict_timing = immediate, vector_len = 384);

INSERT INTO rag_ctx_test (content) VALUES
    ('PostgreSQL is an advanced open source database.'),
    ('The st_embedding function converts text to vectors.');

-- 5.5.1 验证隐藏伴生列存在
SELECT attname, atthidden, attpredict
FROM pg_attribute
WHERE attrelid = 'rag_ctx_test'::regclass
  AND attname IN ('answer', 'answer_actual', 'answer_predict')
ORDER BY attname;

-- 5.5.2 验证 _actual 列已填充（INSERT 时由触发器写入用户输入）
SELECT id, content, answer, answer_actual, answer_predict
FROM rag_ctx_test ORDER BY id;

-- 5.5.3 验证 _predict 列已填充（PREDICT 表达式 upper(content) 的结果）
-- answer_predict 应等于 upper(content)
SELECT id,
       content,
       answer,
       answer_actual,
       answer_predict,
       (answer_predict = upper(content)) AS predict_matches_upper
FROM rag_ctx_test ORDER BY id;

-- 5.5.4 验证 llm_rag_infer 调用成功（RAG 上下文构建包含 _actual/_predict 列）
-- 设置 dummy LLM URL，预期调用失败但 RAG 上下文构建应成功
SET jolix_predict.llm_api_url = 'http://127.0.0.1:19999';
SET jolix_predict.llm_model = 'test-model';
SET jolix_predict.current_table = 'rag_ctx_test';
SET jolix_predict.llm_timeout = 5;

-- 调用 llm_rag_infer，预期因 LLM URL 无效返回 NULL，但不应崩溃
-- 这验证了 do_rag_retrieval 能正确处理包含 _actual/_predict 列的查询
SELECT llm_rag_infer(
    'Answer based on context.',
    'What is PostgreSQL?'
) AS rag_result;

RESET jolix_predict.llm_api_url;
RESET jolix_predict.llm_model;
RESET jolix_predict.current_table;
RESET jolix_predict.llm_timeout;

DROP TABLE IF EXISTS rag_ctx_test CASCADE;

-- ================================================================
-- 第6部分: 向量距离操作符测试
-- ================================================================
\echo '============================================================'
\echo '第6部分: 向量距离操作符测试'
\echo '============================================================'

\echo '--- 6.1 余弦距离 <=> ---'
SELECT id, age, income, category,
       round((demographic <=> (SELECT demographic FROM embd_column WHERE id = 1))::numeric, 4) AS cosine_dist
FROM embd_column WHERE id != 1 ORDER BY cosine_dist;

\echo '--- 6.2 L2 距离 <-> ---'
SELECT id, age, income, category,
       round((demographic <-> (SELECT demographic FROM embd_column WHERE id = 1))::numeric, 4) AS l2_dist
FROM embd_column WHERE id != 1 ORDER BY l2_dist;

\echo '--- 6.3 内积距离 <#> ---'
SELECT id, age, income, category,
       (demographic <#> (SELECT demographic FROM embd_column WHERE id = 1))::numeric AS ip_dist
FROM embd_column WHERE id != 1 ORDER BY ip_dist;

\echo '--- 6.4 条件过滤 + 向量搜索 ---'
SELECT id, age, income, category,
       round((demographic <=> (SELECT demographic FROM embd_column WHERE id = 2))::numeric, 4) AS distance
FROM embd_column WHERE id != 2 AND category = 'student' OR (id != 2 AND income > 60000)
ORDER BY distance;

-- ================================================================
-- 第7部分: 历史记录测试
-- ================================================================
\echo '============================================================'
\echo '第7部分: 历史记录测试'
\echo '============================================================'

\echo '--- 7.1 自动记录验证 ---'
SELECT table_name, role, count(*) AS record_count
FROM jolix_llm_history
GROUP BY table_name, role
ORDER BY table_name, role;

\echo '--- 7.2 手动记录 ---'
SELECT record_predict_history('regression_manual', 'user', 'Test question');
SELECT record_predict_history('regression_manual', 'assistant', 'Test answer');
SELECT table_name, role, content FROM jolix_llm_history WHERE table_name = 'regression_manual';

\echo '--- 7.3 清除历史 ---'
SELECT clear_predict_history('regression_manual');
SELECT count(*) AS remaining FROM jolix_llm_history WHERE table_name = 'regression_manual';

\echo '--- 7.4 历史记录过期清理 ---'
SET jolix_predict.history_retention_days = 7;
INSERT INTO jolix_llm_history (table_name, role, content, created_at)
VALUES ('test_cleanup', 'user', 'old question', now() - interval '8 days');
INSERT INTO jolix_llm_history (table_name, role, content, created_at)
VALUES ('test_cleanup', 'user', 'recent question', now());
SELECT cleanup_predict_history();
SELECT table_name, role, content FROM jolix_llm_history WHERE table_name = 'test_cleanup';
SELECT clear_predict_history('test_cleanup');

-- ================================================================
-- 第8部分: GUC 参数动态修改测试
-- ================================================================
\echo '============================================================'
\echo '第8部分: GUC 参数动态修改测试'
\echo '============================================================'

\echo '--- 8.1 llm_timeout ---'
SET jolix_predict.llm_timeout = 120;
SHOW jolix_predict.llm_timeout;
SET jolix_predict.llm_timeout = 300;

\echo '--- 8.2 llm_history_table ---'
SET jolix_predict.llm_history_table = 'custom_history';
SHOW jolix_predict.llm_history_table;
SET jolix_predict.llm_history_table = 'default';

\echo '--- 8.3 current_table ---'
SET jolix_predict.current_table = 'rag_demo';
SHOW jolix_predict.current_table;
SET jolix_predict.current_table = '';

\echo '--- 8.4 embedding model_name ---'
SET jolix_embedding.model_name = 'sentence-transformers/all-MiniLM-L6-v2';
SHOW jolix_embedding.model_name;

-- ================================================================
-- 第9部分: 非public schema RAG测试
-- ================================================================
\echo '============================================================'
\echo '第9部分: 非public schema RAG测试'
\echo '============================================================'

\echo '--- 9.1 在自定义schema中创建RAG表 ---'
DROP SCHEMA IF EXISTS test_schema CASCADE;
CREATE SCHEMA test_schema;

SET search_path TO test_schema, public;

CREATE TABLE test_schema.rag_test (
    id serial PRIMARY KEY,
    content text EMBEDDING AS (st_embedding(content)) STORED,
    answer text PREDICT AS (llm_rag_infer(
        'Answer based on context. Reply in one short sentence.',
        content
    )) STORED
) WITH (predict_timing = immediate, vector_len = 384);

INSERT INTO test_schema.rag_test (content) VALUES ('PostgreSQL supports advanced indexing');
INSERT INTO test_schema.rag_test (content) VALUES ('Vector databases enable semantic search');
INSERT INTO test_schema.rag_test (content) VALUES ('What is PostgreSQL?');

SELECT id, content, answer FROM test_schema.rag_test WHERE id = 3;

SET search_path TO public;

-- ================================================================
-- 第10部分: ldm_infer 本地推理测试
-- ================================================================
\echo '============================================================'
\echo '第10部分: ldm_infer 本地推理测试'
\echo '============================================================'

\echo '--- 10.1 验证 ldm_infer 函数注册 ---'
SELECT proname FROM pg_proc WHERE proname = 'ldm_infer';

\echo '--- 10.2 设置嵌入模型 ---'
SET jolix_embedding.model_name = 'sentence-transformers/all-MiniLM-L6-v2';

\echo '--- 10.3 分类任务 (classification) ---'
DROP TABLE IF EXISTS ldm_test_class CASCADE;
CREATE TABLE ldm_test_class (
    name text EMBEDDING AS (st_embedding(name)),
    category text PREDICT AS (ldm_infer())
) WITH (predict_timing=immediate, ldm_task='classification', ldm_topn=3, vector_len=384);

INSERT INTO ldm_test_class (name, category) VALUES ('apple', 'fruit');
INSERT INTO ldm_test_class (name, category) VALUES ('banana', 'fruit');
INSERT INTO ldm_test_class (name, category) VALUES ('cherry', 'fruit');
INSERT INTO ldm_test_class (name, category) VALUES ('dog', 'animal');
INSERT INTO ldm_test_class (name, category) VALUES ('cat', 'animal');
INSERT INTO ldm_test_class (name) VALUES ('grape');
SELECT name, category FROM ldm_test_class WHERE name='grape';

\echo '--- 10.4 回归任务 (regression) ---'
DROP TABLE IF EXISTS ldm_test_reg CASCADE;
CREATE TABLE ldm_test_reg (
    feature text EMBEDDING AS (st_embedding(feature)),
    value text PREDICT AS (ldm_infer())
) WITH (predict_timing=immediate, ldm_task='regression', ldm_topn=3, vector_len=384);

INSERT INTO ldm_test_reg (feature, value) VALUES ('one', '1');
INSERT INTO ldm_test_reg (feature, value) VALUES ('five', '5');
INSERT INTO ldm_test_reg (feature, value) VALUES ('ten', '10');
INSERT INTO ldm_test_reg (feature, value) VALUES ('twenty', '20');
INSERT INTO ldm_test_reg (feature) VALUES ('fifteen');
SELECT feature, value FROM ldm_test_reg WHERE feature='fifteen';

\echo '--- 10.5 异常检测任务 (anomaly) ---'
DROP TABLE IF EXISTS ldm_test_anom CASCADE;
CREATE TABLE ldm_test_anom (
    sensor text EMBEDDING AS (st_embedding(sensor)),
    status text PREDICT AS (ldm_infer())
) WITH (predict_timing=immediate, ldm_task='anomaly', ldm_topn=3, vector_len=384);

INSERT INTO ldm_test_anom (sensor, status) VALUES ('apple', 'normal');
INSERT INTO ldm_test_anom (sensor, status) VALUES ('banana', 'normal');
INSERT INTO ldm_test_anom (sensor, status) VALUES ('cherry', 'normal');
INSERT INTO ldm_test_anom (sensor) VALUES ('dog');
SELECT sensor, status FROM ldm_test_anom WHERE sensor='dog';

\echo '--- 10.6 提取任务 (extraction) ---'
DROP TABLE IF EXISTS ldm_test_ext CASCADE;
CREATE TABLE ldm_test_ext (
    source text EMBEDDING AS (st_embedding(source)),
    target text PREDICT AS (ldm_infer())
) WITH (predict_timing=immediate, ldm_task='extraction', ldm_topn=1, vector_len=384);

INSERT INTO ldm_test_ext (source, target) VALUES ('apple', 'red');
INSERT INTO ldm_test_ext (source) VALUES ('apple');
SELECT target FROM ldm_test_ext WHERE source='apple' AND target IS NOT NULL LIMIT 1;

\echo '--- 10.7 边界条件：空表返回NULL ---'
DROP TABLE IF EXISTS ldm_test_empty CASCADE;
CREATE TABLE ldm_test_empty (
    name text EMBEDDING AS (st_embedding(name)),
    label text PREDICT AS (ldm_infer())
) WITH (predict_timing=immediate, ldm_task='classification', ldm_topn=3, vector_len=384);
INSERT INTO ldm_test_empty (name) VALUES ('test1');
SELECT label IS NULL FROM ldm_test_empty WHERE name='test1';

\echo '--- 10.8 边界条件：默认任务（未设置ldm_task）---'
DROP TABLE IF EXISTS ldm_test_default CASCADE;
CREATE TABLE ldm_test_default (
    name text EMBEDDING AS (st_embedding(name)),
    label text PREDICT AS (ldm_infer())
) WITH (predict_timing=immediate, ldm_topn=3, vector_len=384);
INSERT INTO ldm_test_default (name, label) VALUES ('apple', 'A');
INSERT INTO ldm_test_default (name, label) VALUES ('banana', 'A');
INSERT INTO ldm_test_default (name) VALUES ('grape');
SELECT label FROM ldm_test_default WHERE name='grape';

\echo '--- 10.9 WITH参数验证 ---'
SELECT reloptions FROM pg_class WHERE relname='ldm_test_reg';

-- ================================================================
-- 第11部分: 时序向量表 (reloptions 方案) + BGW 异步测试
-- ================================================================
\echo '============================================================'
\echo '第11部分: 时序向量表 + BGW 异步触发'
\echo '============================================================'

\echo '--- 11.1 确认 BGW 已加载 (需 shared_preload_libraries=tsvector_funcs) ---'
SELECT name, setting, unit FROM pg_settings
WHERE name = 'shared_preload_libraries';

\echo '--- 11.2 确认 BGW 进程在运行 ---'
SELECT pid, state, query
FROM pg_stat_activity
WHERE backend_type LIKE '%worker%'
  AND query LIKE '%tsvector_worker%';

\echo '--- 11.3 创建源表（hypertable 可选）---'
DROP TABLE IF EXISTS tsvec_src CASCADE;
CREATE TABLE tsvec_src (
    time  TIMESTAMPTZ NOT NULL,
    k     TEXT,
    v     DOUBLE PRECISION
);
-- 如果已装 TimescaleDB: SELECT create_hypertable('tsvec_src', 'time');

\echo '--- 11.4 创建向量表（手动定义列 + reloptions）---'
DROP TABLE IF EXISTS tsvec_vec CASCADE;
CREATE TABLE tsvec_vec (
    slice_start  TIMESTAMPTZ,
    slice_end    TIMESTAMPTZ,
    k            TEXT,
    embedding    vector(3),
    _row_count   BIGINT,
    _processed   BOOL DEFAULT FALSE,
    PRIMARY KEY (slice_start, k)
);

-- 注入 reloptions (UPDATE pg_class 是最稳妥的方式)
UPDATE pg_class
   SET reloptions = ARRAY[
        'timeseries.source=tsvec_src',
        'timeseries.bucket_interval=3600',
        'timeseries.vector_len=3',
        'timeseries.vector_column=embedding',
        'timeseries.carry_columns=k',
        'timeseries.value_column=v'
     ]
 WHERE relname = 'tsvec_vec'
   AND relnamespace = 'public'::regnamespace;

SELECT relname, reloptions FROM pg_class WHERE relname = 'tsvec_vec';

\echo '--- 11.5 验证 BGW 能发现 vec 表 ---'
SELECT c.oid, c.relname
FROM pg_class c, LATERAL unnest(c.reloptions) AS opt
WHERE opt LIKE 'timeseries.source=%';

\echo '--- 11.6 基线：手动跑一次 timeseries_vector_run ---'
-- 10:00, 10:15, 10:30 的 A 数据 + 10:45 的 B
-- FLOOR 语义: 10:00,10:15,10:30,10:45 全属于 10:00 bucket
INSERT INTO tsvec_src (time, k, v) VALUES
    ('2026-09-12 10:00:00+08', 'A', 1.0),
    ('2026-09-12 10:15:00+08', 'A', 2.0),
    ('2026-09-12 10:30:00+08', 'A', 3.0),
    ('2026-09-12 10:45:00+08', 'B', 4.0);

SELECT timeseries_vector_run('tsvec_vec'::regclass) AS run_rows;

-- Bug #1 回归: 10:30 必须在 10:00 bucket (不是 ROUND 到 11:00)
-- 所以 A 的 _row_count 应该是 3 (10:00, 10:15, 10:30)
SELECT slice_start, k, vector_dims(embedding) AS dims, _row_count
FROM tsvec_vec ORDER BY slice_start, k;

\echo '--- 11.6a Bug#1 FLOOR 验证: 10:30 必须在 10:00 bucket ---'
SELECT 
  'bucket FLOOR check' AS test,
  COUNT(*) FILTER (WHERE k = 'A' AND slice_start = '2026-09-12 10:00:00+08' AND _row_count = 3) AS pass,
  COUNT(*) FILTER (WHERE k = 'A' AND slice_start = '2026-09-12 11:00:00+08') AS wrong_bucket;
-- 预期 pass=1, wrong_bucket=0

SELECT
  'FLOOR vs ROUND' AS method,
  FLOOR(EXTRACT(EPOCH FROM TIMESTAMP '2026-09-12 10:30:00+08')/3600)::bigint*3600 AS floor_epoch,
  (EXTRACT(EPOCH FROM TIMESTAMP '2026-09-12 10:30:00+08')/3600)::bigint*3600 AS round_epoch;
-- 预期 floor_epoch = round_epoch-3600 (FLOOR 正确, ROUND 会多跳 1 小时)

\echo '--- 11.7 BGW 异步触发：插入新行，等 BGW 自动处理 ---'
INSERT INTO tsvec_src (time, k, v) VALUES
    ('2026-09-12 10:50:00+08', 'C', 5.0),
    ('2026-09-12 11:00:00+08', 'D', 6.0),
    ('2026-09-12 11:15:00+08', 'A', 7.0);

-- BGW poll 间隔 1s，等 5s 足够多轮
SELECT pg_sleep(5);

SELECT slice_start, k, vector_dims(embedding) AS dims, _row_count
FROM tsvec_vec ORDER BY slice_start, k;

\echo '--- 11.8 验证 BGW 日志（应有 "vec_oid=... -> N rows"）---'
-- 需在服务器上查看: tail -f /var/log/postgresql/postgresql-18-main.log | grep tsvector_worker
\echo '--- 11.9 幂等性：再次手动跑，ON CONFLICT DO UPDATE ---'
SELECT timeseries_vector_run('tsvec_vec'::regclass) AS run_again;
SELECT _row_count FROM tsvec_vec ORDER BY slice_start, k;

\echo '--- 11.10 ts2v_moment 函数测试 ---'
SELECT ts2v_moment(ARRAY[1.0, 2.0, 3.0]::float8[], 3) AS basic_vec;
SELECT vector_dims(ts2v_moment(ARRAY[1.0, 2.0, 3.0]::float8[], 4)) AS dims_4;
SELECT vector_dims(ts2v_moment(ARRAY[1.0, 2.0]::float8[])) AS default_dims;

\echo '--- 11.11 timeseries_vector_search 相似度搜索测试 ---'
-- 先确保目标向量表有数据
SELECT count(*) AS tsvec_count FROM tsvec_vec;

-- 测试1: 基本相似度搜索（指定 carry_filter）
\echo '--- 11.11.1 指定 carry_filter 搜索 ---'
SELECT slice_start_out, slice_end_out, round(cosine_distance::numeric, 6) AS cos_dist
FROM timeseries_vector_search('tsvec_vec', '2026-09-16 00:00:00+00', 'k = ''1''', 3);

-- 测试2: 不指定 carry_filter（跨 carry 值搜索）
\echo '--- 11.11.2 无 carry_filter 跨值搜索 ---'
SELECT slice_start_out, slice_end_out, round(cosine_distance::numeric, 6) AS cos_dist
FROM timeseries_vector_search('tsvec_vec', '2026-09-16 00:00:00+00', NULL, 5);

-- 测试3: 非对齐时间戳自动对齐（如 10:30:45 -> 10:00:00）
\echo '--- 11.11.3 非对齐时间戳自动对齐 ---'
SELECT slice_start_out, slice_end_out, round(cosine_distance::numeric, 6) AS cos_dist
FROM timeseries_vector_search('tsvec_vec', '2026-09-16 00:30:45+00', 'k = ''1''', 3);

-- 测试4: 搜索未计算向量的时间片（期望返回空集）
\echo '--- 11.11.4 未计算向量的时间片返回空 ---'
SELECT count(*) AS empty_result_count
FROM timeseries_vector_search('tsvec_vec', '2099-01-01 00:00:00+00', 'k = ''1''', 5);

-- 测试5: 验证返回的 slice 是自排除的（目标 slice 不应出现在结果中）
\echo '--- 11.11.5 目标 slice 自排除验证 ---'
SELECT count(*) AS self_excluded_count FROM (
    SELECT slice_start_out FROM timeseries_vector_search(
        'tsvec_vec', '2026-09-16 00:00:00+00', 'k = ''1''', 100
    )
) sub WHERE slice_start_out = '2026-09-16 00:00:00+00'::timestamptz;

-- 测试6: 不存在的表应报错
\echo '--- 11.11.6 不存在的表报错验证 ---'
DO $$
BEGIN
    PERFORM * FROM timeseries_vector_search('nonexistent_table', now(), NULL, 5);
    RAISE NOTICE 'FAIL: should have raised exception';
EXCEPTION WHEN OTHERS THEN
    RAISE NOTICE 'OK: got expected error: %', SQLERRM;
END $$;

\echo '--- 11.12 向量嵌套表 + ts2v_pool 测试 ---'
-- 先用 tsvec_vec 做一层 source，建 tsvec_vec2
\echo '--- 11.12.1 建二级向量表 tsvec_vec2（tsvec_vec → tsvec_vec2） ---'
DROP TABLE IF EXISTS tsvec_vec2 CASCADE;
CREATE TABLE tsvec_vec2 () WITH (
    timeseries.source              = 'tsvec_vec',
    timeseries.time_column         = 'slice_start',
    timeseries.bucket_interval     = 7200,
    timeseries.carry_columns       = 'k',
    timeseries.value_column        = 'embedding',
    timeseries.vectorize_function = 'ts2v_pool',
    timeseries.vector_len          = 3
);

\echo '--- 11.12.2 跑二级向量化 ---'
SELECT timeseries_vector_run('tsvec_vec2'::regclass) AS vec2_rows;

\echo '--- 11.12.3 验证 ts2v_pool avg 聚合与手动 avg 一致 ---'
SELECT
    count(*) AS vec2_count,
    (SELECT round((embedding <=> (SELECT avg(embedding) FROM tsvec_vec WHERE k='1'))::numeric, 6)
       FROM tsvec_vec2 WHERE k='1' ORDER BY slice_start LIMIT 1) AS match_err;
-- match_err 应为 0，验证 ts2v_pool 结果 = 手动 avg

\echo '--- 11.12.4 在二级向量表上跑相似度搜索 ---'
SELECT slice_start_out, round(cosine_distance::numeric, 6) AS dist
FROM timeseries_vector_search('tsvec_vec2', '2026-09-16 00:00:00+00', 'k = ''1''', 3);

\echo '--- 11.12.5 错误路径：未配 vectorize_function（应返回 0 行，不崩溃） ---'
DROP TABLE IF EXISTS tsvec_vec_bad CASCADE;
CREATE TABLE tsvec_vec_bad () WITH (
    timeseries.source          = 'tsvec_vec',
    timeseries.time_column     = 'slice_start',
    timeseries.bucket_interval = 7200,
    timeseries.value_column    = 'embedding',
    timeseries.vector_len      = 3
    -- 忘了配 vectorize_function
);
SELECT timeseries_vector_run('tsvec_vec_bad'::regclass) AS vec_bad_rows;
-- 期望 0 行，不崩溃

\echo '--- 11.12.6 错误路径：未配 time_column（默认 time，应返回 0 行） ---'
DROP TABLE IF EXISTS tsvec_vec_bad2 CASCADE;
CREATE TABLE tsvec_vec_bad2 () WITH (
    timeseries.source              = 'tsvec_vec',
    timeseries.bucket_interval     = 7200,
    timeseries.carry_columns       = 'k',
    timeseries.value_column        = 'embedding',
    timeseries.vectorize_function = 'ts2v_pool',
    timeseries.vector_len          = 3
    -- 忘了配 time_column
);
SELECT timeseries_vector_run('tsvec_vec_bad2'::regclass) AS vec_bad2_rows;
-- 期望 0 行，因为 tsvec_vec 没有 'time' 列

-- ================================================================
-- 第12部分: 清理
-- ================================================================
\echo '============================================================'
\echo '第12部分: 清理'
\echo '============================================================'

DROP TABLE IF EXISTS pred_basic CASCADE;
DROP TABLE IF EXISTS pred_multi CASCADE;
DROP TABLE IF EXISTS pred_deferred CASCADE;
DROP TABLE IF EXISTS pred_deferred_manual CASCADE;
DROP TABLE IF EXISTS emb_text CASCADE;
DROP TABLE IF EXISTS embd_column CASCADE;
DROP TABLE IF EXISTS embd_alter CASCADE;
DROP TABLE IF EXISTS embd_notype CASCADE;
DROP TABLE IF EXISTS embd_stored CASCADE;
DROP TABLE IF EXISTS embd_meta CASCADE;
DROP TABLE IF EXISTS embd_multi CASCADE;
DROP TABLE IF EXISTS embd_hybrid CASCADE;
DROP TABLE IF EXISTS rag_demo CASCADE;
DROP TABLE IF EXISTS ldm_test_class CASCADE;
DROP TABLE IF EXISTS ldm_test_reg CASCADE;
DROP TABLE IF EXISTS ldm_test_anom CASCADE;
DROP TABLE IF EXISTS ldm_test_ext CASCADE;
DROP TABLE IF EXISTS ldm_test_empty CASCADE;
DROP TABLE IF EXISTS ldm_test_default CASCADE;
DROP TABLE IF EXISTS tsvec_vec CASCADE;
DROP TABLE IF EXISTS tsvec_src CASCADE;
DROP TABLE IF EXISTS tsvec_vec2 CASCADE;
DROP TABLE IF EXISTS tsvec_vec_bad CASCADE;
DROP TABLE IF EXISTS tsvec_vec_bad2 CASCADE;
DROP SCHEMA IF EXISTS test_schema CASCADE;

SELECT clear_predict_history();

\echo '============================================================'
\echo '回归测试执行完毕！'
\echo '============================================================'
