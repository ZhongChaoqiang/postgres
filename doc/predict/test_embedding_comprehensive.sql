\pset pager off
\set ON_ERROR_STOP off

\echo '============================================================'
\echo '  EMBEDDING 标签列功能综合测试'
\echo '============================================================'

-- 清理环境
DROP TABLE IF EXISTS test_emb_basic CASCADE;
DROP TABLE IF EXISTS test_emb_vector_len CASCADE;
DROP TABLE IF EXISTS test_emb_multi CASCADE;
DROP TABLE IF EXISTS test_emb_multi_func CASCADE;
DROP TABLE IF EXISTS test_emb_addcol CASCADE;
DROP TABLE IF EXISTS test_emb_update CASCADE;
DROP TABLE IF EXISTS test_emb_null CASCADE;
DROP TABLE IF EXISTS test_emb_hidden CASCADE;
DROP TABLE IF EXISTS test_emb_trigger_check CASCADE;
DROP TABLE IF EXISTS test_emb_index_check CASCADE;
DROP TABLE IF EXISTS test_emb_l2_search CASCADE;
DROP TABLE IF EXISTS test_emb_cosine_search CASCADE;
DROP TABLE IF EXISTS test_emb_ip_search CASCADE;
DROP TABLE IF EXISTS test_emb_where_rewrite CASCADE;
DROP TABLE IF EXISTS test_emb_orderby_rewrite CASCADE;
DROP TABLE IF EXISTS test_emb_reloptions CASCADE;
DROP TABLE IF EXISTS test_emb_predict_combo CASCADE;
DROP TABLE IF EXISTS test_emb_batch_insert CASCADE;
DROP TABLE IF EXISTS test_emb_attr_check CASCADE;
DROP TABLE IF EXISTS test_emb_col_func_relopt CASCADE;
DROP TABLE IF EXISTS test_emb_reverse_op CASCADE;
DROP TABLE IF EXISTS test_emb_no_func CASCADE;
DROP FUNCTION IF EXISTS my_embedding(text) CASCADE;
DROP FUNCTION IF EXISTS my_embedding_5d(text) CASCADE;
DROP FUNCTION IF EXISTS emb_func_a(text) CASCADE;
DROP FUNCTION IF EXISTS emb_func_b(text) CASCADE;

-- 创建通用嵌入函数 (返回3维向量)
CREATE OR REPLACE FUNCTION my_embedding(input_text text)
RETURNS vector AS $$
BEGIN
    RETURN '[0.1, 0.2, 0.3]'::vector;
END;
$$ LANGUAGE plpgsql IMMUTABLE;

-- 创建5维嵌入函数
CREATE OR REPLACE FUNCTION my_embedding_5d(input_text text)
RETURNS vector AS $$
BEGIN
    RETURN '[0.1, 0.2, 0.3, 0.4, 0.5]'::vector;
END;
$$ LANGUAGE plpgsql IMMUTABLE;

-- 创建列特定嵌入函数
CREATE OR REPLACE FUNCTION emb_func_a(input_text text)
RETURNS vector AS $$
BEGIN
    RETURN '[1.0, 0.0, 0.0]'::vector;
END;
$$ LANGUAGE plpgsql IMMUTABLE;

CREATE OR REPLACE FUNCTION emb_func_b(input_text text)
RETURNS vector AS $$
BEGIN
    RETURN '[0.0, 1.0, 0.0]'::vector;
END;
$$ LANGUAGE plpgsql IMMUTABLE;

-- ============================================================
-- 测试1: CREATE TABLE with EMBEDDING column - 基本DDL
-- ============================================================
\echo ''
\echo '===== 测试1: CREATE TABLE with EMBEDDING column - 基本DDL ====='

CREATE TABLE test_emb_basic (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

-- 检查所有列(包括隐藏列)
\echo '-- 检查所有列(包括隐藏列)'
SELECT attname, atttypid::regtype, attembedding, atthidden
FROM pg_attribute
WHERE attrelid = 'test_emb_basic'::regclass AND attnum > 0 AND NOT attisdropped
ORDER BY attnum;

-- 检查触发器是否自动创建
\echo '-- 检查自动创建的触发器(包括内部触发器)'
SELECT tgname, tgisinternal, proname
FROM pg_trigger t
JOIN pg_proc p ON t.tgfoid = p.oid
WHERE t.tgrelid = 'test_emb_basic'::regclass
ORDER BY tgname;

-- 检查索引是否自动创建
\echo '-- 检查自动创建的索引'
SELECT indexname, indexdef
FROM pg_indexes
WHERE tablename = 'test_emb_basic'
ORDER BY indexname;

DROP TABLE test_emb_basic CASCADE;

-- ============================================================
-- 测试2: vector_len 选项 - 自定义向量维度
-- ============================================================
\echo ''
\echo '===== 测试2: vector_len 选项 - 自定义向量维度 ====='

CREATE TABLE test_emb_vector_len (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding_5d',
    vector_len = 5
);

-- 检查 _embedding 列的类型是否为 vector(5)
\echo '-- 检查 _embedding 列的向量维度'
SELECT attname, atttypid::regtype, atttypmod
FROM pg_attribute
WHERE attrelid = 'test_emb_vector_len'::regclass AND attnum > 0 AND NOT attisdropped
ORDER BY attnum;

INSERT INTO test_emb_vector_len (content) VALUES ('hello');

\echo '-- 查询结果: content_embedding应为5维向量'
SELECT id, content, content_embedding FROM test_emb_vector_len;

DROP TABLE test_emb_vector_len CASCADE;

-- ============================================================
-- 测试3: 多个 EMBEDDING 列 - 单一嵌入函数
-- ============================================================
\echo ''
\echo '===== 测试3: 多个 EMBEDDING 列 - 单一嵌入函数 ====='

CREATE TABLE test_emb_multi (
    id SERIAL PRIMARY KEY,
    title TEXT EMBEDDING,
    body TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

INSERT INTO test_emb_multi (title, body) VALUES ('hello', 'world');

\echo '-- 查询结果: title_embedding和body_embedding都应为3维向量'
SELECT id, title, title_embedding, body, body_embedding FROM test_emb_multi;

DROP TABLE test_emb_multi CASCADE;

-- ============================================================
-- 测试4: 多个 EMBEDDING 列 - 列特定嵌入函数
-- ============================================================
\echo ''
\echo '===== 测试4: 多个 EMBEDDING 列 - 列特定嵌入函数 ====='

CREATE TABLE test_emb_multi_func (
    id SERIAL PRIMARY KEY,
    title TEXT EMBEDDING,
    body TEXT EMBEDDING
) WITH (
    embedding_function = 'title:emb_func_a;body:emb_func_b',
    vector_len = 3
);

INSERT INTO test_emb_multi_func (title, body) VALUES ('hello', 'world');

\echo '-- 查询结果: title_embedding=[1,0,0], body_embedding=[0,1,0]'
SELECT id, title, title_embedding, body, body_embedding FROM test_emb_multi_func;

DROP TABLE test_emb_multi_func CASCADE;

-- ============================================================
-- 测试5: ALTER TABLE ADD COLUMN with EMBEDDING
-- ============================================================
\echo ''
\echo '===== 测试5: ALTER TABLE ADD COLUMN with EMBEDDING ====='

CREATE TABLE test_emb_addcol (
    id SERIAL PRIMARY KEY,
    name TEXT
) WITH (
    vector_len = 3
);

ALTER TABLE test_emb_addcol ADD COLUMN description TEXT EMBEDDING;

-- 检查隐藏列是否创建
\echo '-- 检查列(包括隐藏列)'
SELECT attname, atttypid::regtype, attembedding, atthidden
FROM pg_attribute
WHERE attrelid = 'test_emb_addcol'::regclass AND attnum > 0 AND NOT attisdropped
ORDER BY attnum;

-- 检查触发器是否创建
\echo '-- 检查触发器(包括内部触发器)'
SELECT tgname, tgisinternal, proname
FROM pg_trigger t
JOIN pg_proc p ON t.tgfoid = p.oid
WHERE t.tgrelid = 'test_emb_addcol'::regclass
ORDER BY tgname;

-- 设置embedding_function并插入数据
ALTER TABLE test_emb_addcol SET (embedding_function = 'my_embedding');

INSERT INTO test_emb_addcol (name, description) VALUES ('test', 'hello world');

\echo '-- 查询结果: description_embedding应为3维向量'
SELECT id, name, description, description_embedding FROM test_emb_addcol;

DROP TABLE test_emb_addcol CASCADE;

-- ============================================================
-- 测试6: UPDATE 触发嵌入函数重新计算
-- ============================================================
\echo ''
\echo '===== 测试6: UPDATE 触发嵌入函数重新计算 ====='

CREATE TABLE test_emb_update (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

INSERT INTO test_emb_update (content) VALUES ('original');

\echo '-- 插入后: content_embedding应为[0.1,0.2,0.3]'
SELECT id, content, content_embedding FROM test_emb_update;

UPDATE test_emb_update SET content = 'updated' WHERE id = 1;

\echo '-- UPDATE后: content_embedding应重新计算'
SELECT id, content, content_embedding FROM test_emb_update;

DROP TABLE test_emb_update CASCADE;

-- ============================================================
-- 测试7: NULL 值处理 - EMBEDDING 列为 NULL 时不调用嵌入函数
-- ============================================================
\echo ''
\echo '===== 测试7: NULL 值处理 ====='

CREATE TABLE test_emb_null (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

-- INSERT with NULL content -> should NOT call embedding function
INSERT INTO test_emb_null (id) VALUES (DEFAULT);

\echo '-- 查询结果: content=NULL, content_embedding=NULL'
SELECT id, content, content_embedding FROM test_emb_null;

DROP TABLE test_emb_null CASCADE;

-- ============================================================
-- 测试8: 隐藏列 - SELECT * 不显示 _embedding 列
-- ============================================================
\echo ''
\echo '===== 测试8: 隐藏列 - SELECT * 不显示隐藏列 ====='

CREATE TABLE test_emb_hidden (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

INSERT INTO test_emb_hidden (content) VALUES ('test');

\echo '-- SELECT * 应该只显示id和content,不显示隐藏列'
SELECT * FROM test_emb_hidden;

\echo '-- 显式指定列名可以查询隐藏列'
SELECT id, content, content_embedding FROM test_emb_hidden;

DROP TABLE test_emb_hidden CASCADE;

-- ============================================================
-- 测试9: 触发器命名规则检查
-- ============================================================
\echo ''
\echo '===== 测试9: 触发器命名规则检查 ====='

CREATE TABLE test_emb_trigger_check (
    id SERIAL PRIMARY KEY,
    title TEXT EMBEDDING,
    body TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

\echo '-- 检查触发器名称格式: pg_embedding_{colname}_{relOid}'
SELECT tgname, tgisinternal, proname
FROM pg_trigger t
JOIN pg_proc p ON t.tgfoid = p.oid
WHERE t.tgrelid = 'test_emb_trigger_check'::regclass
ORDER BY tgname;

DROP TABLE test_emb_trigger_check CASCADE;

-- ============================================================
-- 测试10: 向量索引检查
-- ============================================================
\echo ''
\echo '===== 测试10: 向量索引检查 ====='

CREATE TABLE test_emb_index_check (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

\echo '-- 检查索引: 应有 {tablename}_{colname}_embedding_idx'
SELECT indexname, indexdef
FROM pg_indexes
WHERE tablename = 'test_emb_index_check'
ORDER BY indexname;

DROP TABLE test_emb_index_check CASCADE;

-- ============================================================
-- 测试11: L2 距离搜索 (<->)
-- ============================================================
\echo ''
\echo '===== 测试11: L2 距离搜索 (<->) ====='

CREATE TABLE test_emb_l2_search (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

INSERT INTO test_emb_l2_search (content) VALUES ('hello'), ('world'), ('test');

\echo '-- 使用 <-> 操作符进行L2距离搜索'
SELECT id, content, content <-> 'hello' AS distance
FROM test_emb_l2_search
ORDER BY content <-> 'hello';

DROP TABLE test_emb_l2_search CASCADE;

-- ============================================================
-- 测试12: 余弦距离搜索 (<=>)
-- ============================================================
\echo ''
\echo '===== 测试12: 余弦距离搜索 (<=>) ====='

CREATE TABLE test_emb_cosine_search (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

INSERT INTO test_emb_cosine_search (content) VALUES ('hello'), ('world'), ('test');

\echo '-- 使用 <=> 操作符进行余弦距离搜索'
SELECT id, content, content <=> 'hello' AS distance
FROM test_emb_cosine_search
ORDER BY content <=> 'hello';

DROP TABLE test_emb_cosine_search CASCADE;

-- ============================================================
-- 测试13: 内积距离搜索 (<#>)
-- ============================================================
\echo ''
\echo '===== 测试13: 内积距离搜索 (<#>) ====='

CREATE TABLE test_emb_ip_search (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

INSERT INTO test_emb_ip_search (content) VALUES ('hello'), ('world'), ('test');

\echo '-- 使用 <#> 操作符进行内积距离搜索'
SELECT id, content, content <#> 'hello' AS distance
FROM test_emb_ip_search
ORDER BY content <#> 'hello';

DROP TABLE test_emb_ip_search CASCADE;

-- ============================================================
-- 测试14: WHERE 条件向量查询重写
-- ============================================================
\echo ''
\echo '===== 测试14: WHERE 条件向量查询重写 ====='

CREATE TABLE test_emb_where_rewrite (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

INSERT INTO test_emb_where_rewrite (content) VALUES ('hello'), ('world'), ('test');

\echo '-- WHERE条件中使用向量距离操作符'
SELECT id, content, content <-> 'hello' AS distance
FROM test_emb_where_rewrite
WHERE content <-> 'hello' < 1.0
ORDER BY content <-> 'hello';

DROP TABLE test_emb_where_rewrite CASCADE;

-- ============================================================
-- 测试15: ORDER BY 向量距离查询重写
-- ============================================================
\echo ''
\echo '===== 测试15: ORDER BY 向量距离查询重写 ====='

CREATE TABLE test_emb_orderby_rewrite (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

INSERT INTO test_emb_orderby_rewrite (content) VALUES ('hello'), ('world'), ('test');

\echo '-- ORDER BY中使用向量距离操作符'
SELECT id, content
FROM test_emb_orderby_rewrite
ORDER BY content <-> 'hello'
LIMIT 2;

DROP TABLE test_emb_orderby_rewrite CASCADE;

-- ============================================================
-- 测试16: reloptions 检查
-- ============================================================
\echo ''
\echo '===== 测试16: reloptions 检查 ====='

CREATE TABLE test_emb_reloptions (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

\echo '-- 检查reloptions'
SELECT reloptions FROM pg_class WHERE relname = 'test_emb_reloptions';

DROP TABLE test_emb_reloptions CASCADE;

-- ============================================================
-- 测试17: PREDICT + EMBEDDING 组合使用
-- ============================================================
\echo ''
\echo '===== 测试17: PREDICT + EMBEDDING 组合使用 ====='

CREATE OR REPLACE FUNCTION simple_predict(rec record)
RETURNS FLOAT AS $$
BEGIN
    RETURN 1.0;
END;
$$ LANGUAGE plpgsql;

CREATE TABLE test_emb_predict_combo (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING,
    score FLOAT PREDICT
) WITH (
    embedding_function = 'my_embedding',
    predict_function = 'simple_predict',
    predict_timing = immediate,
    vector_len = 3
);

INSERT INTO test_emb_predict_combo (content) VALUES ('test');

\echo '-- 查询结果: content_embedding应为向量, score应为1.0'
SELECT id, content, content_embedding, score, score_predict, score_actual
FROM test_emb_predict_combo;

DROP TABLE test_emb_predict_combo CASCADE;
DROP FUNCTION simple_predict(record) CASCADE;

-- ============================================================
-- 测试18: 批量插入测试
-- ============================================================
\echo ''
\echo '===== 测试18: 批量插入测试 ====='

CREATE TABLE test_emb_batch_insert (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

INSERT INTO test_emb_batch_insert (content) VALUES ('doc1'), ('doc2'), ('doc3'), ('doc4'), ('doc5');

\echo '-- 查询结果: 所有行都应有content_embedding'
SELECT id, content, content_embedding FROM test_emb_batch_insert ORDER BY id;

DROP TABLE test_emb_batch_insert CASCADE;

-- ============================================================
-- 测试19: pg_attribute 中 attembedding 标记检查
-- ============================================================
\echo ''
\echo '===== 测试19: pg_attribute 中 attembedding 标记检查 ====='

CREATE TABLE test_emb_attr_check (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING,
    normal_col TEXT
) WITH (
    vector_len = 3
);

\echo '-- 检查attembedding标记: content应为true, 其他应为false'
SELECT attname, attembedding, attpredict, atthidden
FROM pg_attribute
WHERE attrelid = 'test_emb_attr_check'::regclass AND attnum > 0 AND NOT attisdropped
ORDER BY attnum;

DROP TABLE test_emb_attr_check CASCADE;

-- ============================================================
-- 测试20: embedding_function reloption 列特定格式检查
-- ============================================================
\echo ''
\echo '===== 测试20: embedding_function reloption 列特定格式检查 ====='

CREATE TABLE test_emb_col_func_relopt (
    id SERIAL PRIMARY KEY,
    title TEXT EMBEDDING,
    body TEXT EMBEDDING
) WITH (
    embedding_function = 'title:emb_func_a;body:emb_func_b',
    vector_len = 3
);

\echo '-- 检查reloptions包含列特定嵌入函数'
SELECT reloptions FROM pg_class WHERE relname = 'test_emb_col_func_relopt';

DROP TABLE test_emb_col_func_relopt CASCADE;

-- ============================================================
-- 测试21: 向量距离操作符反向 (text在右侧)
-- ============================================================
\echo ''
\echo '===== 测试21: 向量距离操作符反向 (text在右侧) ====='

CREATE TABLE test_emb_reverse_op (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

INSERT INTO test_emb_reverse_op (content) VALUES ('hello'), ('world');

\echo '-- 反向操作符: ''hello'' <-> content'
SELECT id, content, 'hello' <-> content AS distance
FROM test_emb_reverse_op
ORDER BY 'hello' <-> content;

DROP TABLE test_emb_reverse_op CASCADE;

-- ============================================================
-- 测试22: 不设置 embedding_function 时插入数据
-- ============================================================
\echo ''
\echo '===== 测试22: 不设置 embedding_function 时插入数据 ====='

CREATE TABLE test_emb_no_func (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    vector_len = 3
);

-- 没有设置embedding_function, INSERT应该不会触发嵌入计算
INSERT INTO test_emb_no_func (content) VALUES ('test');

\echo '-- 查询结果: content_embedding应为NULL(没有embedding_function)'
SELECT id, content, content_embedding FROM test_emb_no_func;

DROP TABLE test_emb_no_func CASCADE;

-- 清理嵌入函数
DROP FUNCTION my_embedding(text) CASCADE;
DROP FUNCTION my_embedding_5d(text) CASCADE;
DROP FUNCTION emb_func_a(text) CASCADE;
DROP FUNCTION emb_func_b(text) CASCADE;

\echo ''
\echo '============================================================'
\echo '  所有EMBEDDING测试完成!'
\echo '============================================================'
