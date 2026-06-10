-- ================================================================
-- PostgreSQL 18.3 综合回归测试脚本
-- 覆盖: PREDICT / EMBEDDING / EMBEDDINGS / RAG / st_embedding / ft_transformer_embedding
-- 日期: 2026-06-10
-- ================================================================
-- 使用方法:
--   psql -h localhost -p 5433 -U postgres -d postgres -f regression_test.sql
-- 前置条件:
--   1. 已安装 postgresql-18 deb 包（含 jolix_predict, jolix_embedding, vector 扩展）
--   2. 已全局安装 sentence-transformers: sudo -H pip3 install sentence-transformers
--   3. 已创建模型目录: /usr/local/pgsql/models (postgres用户可写)
--   4. 网络可访问 LLM API
-- ================================================================

\set ON_ERROR_STOP off

-- ================================================================
-- 第1部分: 扩展与配置验证
-- ================================================================
\echo '============================================================'
\echo '第1部分: 扩展与配置验证'
\echo '============================================================'

\echo '--- 1.1 扩展安装验证 ---'
SELECT extname, extversion FROM pg_extension
WHERE extname IN ('jolix_predict', 'jolix_embedding', 'vector', 'plpgsql')
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
                   'record_predict_history', 'clear_predict_history', 'cleanup_predict_history')
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

\echo '--- 2.5 PREDICT 列 - 延迟推理（predict_timing = deferred）---'
DROP TABLE IF EXISTS pred_deferred CASCADE;
CREATE TABLE pred_deferred (
    id serial PRIMARY KEY,
    question text,
    answer text PREDICT AS (llm_infer(
        'Answer in one short sentence.',
        question
    )) STORED
) WITH (predict_timing = deferred);

\echo '--- 2.5.1 插入数据（answer 列应为 NULL，等待 async worker 处理）---'
INSERT INTO pred_deferred (question) VALUES ('What is Java?');
INSERT INTO pred_deferred (question) VALUES ('What is Linux?');
SELECT id, question, answer FROM pred_deferred;

\echo '--- 2.5.2 验证 predict_timing 选项值 ---'
SELECT c.relname, reloptions
FROM pg_class c
WHERE c.relname = 'pred_deferred';

\echo '--- 2.5.3 设置 async worker 参数（缩短扫描间隔以便测试）---'
ALTER SYSTEM SET async_predict_naptime = 5;
ALTER SYSTEM SET async_predict_enabled = true;
SELECT pg_reload_conf();

\echo '--- 2.5.4 等待 async worker 处理（等待约 10 秒）---'
SELECT pg_sleep(10);

\echo '--- 2.5.5 检查 async worker 是否已处理 ---'
SELECT id, question, answer FROM pred_deferred;

\echo '--- 2.5.6 恢复 async worker 默认参数 ---'
ALTER SYSTEM RESET async_predict_naptime;
SELECT pg_reload_conf();

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

\echo '--- 2.7 PREDICT 列 - deferred 模式 UPDATE 触发重新计算 ---'
UPDATE pred_deferred SET question = 'What is Golang?' WHERE id = 1;
SELECT id, question, answer FROM pred_deferred WHERE id = 1;

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
-- 第10部分: 清理
-- ================================================================
\echo '============================================================'
\echo '第10部分: 清理'
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
DROP SCHEMA IF EXISTS test_schema CASCADE;

SELECT clear_predict_history();

\echo '============================================================'
\echo '回归测试执行完毕！'
\echo '============================================================'
