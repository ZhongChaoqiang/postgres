-- ================================================
-- PostgreSQL EMBEDDING 功能完整测试脚本
-- ================================================

-- 准备：创建embedding函数
CREATE OR REPLACE FUNCTION simple_embedding(input text) RETURNS vector
LANGUAGE plpgsql IMMUTABLE AS $$
BEGIN
    RETURN '[0.1, 0.2, 0.3]'::vector;
END;
$$;

CREATE OR REPLACE FUNCTION title_embedding(input text) RETURNS vector
LANGUAGE plpgsql IMMUTABLE AS $$
BEGIN
    RETURN '[0.4, 0.5, 0.6]'::vector;
END;
$$;

-- ============================================
-- 测试1: 创建带有EMBEDDING列的表
-- ============================================
CREATE TABLE test_emb_full (
    id int PRIMARY KEY,
    content text,
    category text EMBEDDING AS (simple_embedding(content)) STORED
) WITH (vector_len=3);

-- 验证表结构
SELECT attname, atttypid::regtype, attgenerated, attembedding
FROM pg_attribute
WHERE attrelid = 'test_emb_full'::regclass AND attnum > 0 AND NOT attisdropped
ORDER BY attnum;

-- 验证触发器
SELECT tgname FROM pg_trigger WHERE tgrelid = 'test_emb_full'::regclass;

-- 验证索引
SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'test_emb_full';

-- 验证\d显示
\d test_emb_full

-- ============================================
-- 测试2: INSERT数据（触发器自动生成向量）
-- ============================================
INSERT INTO test_emb_full (id, content) VALUES (1, 'hello world');
INSERT INTO test_emb_full (id, content) VALUES (2, 'machine learning');
INSERT INTO test_emb_full (id, content) VALUES (3, 'database system');

SELECT id, content, category, category_embedding FROM test_emb_full ORDER BY id;

-- ============================================
-- 测试3: UPDATE数据（触发器自动更新向量）
-- ============================================
UPDATE test_emb_full SET content = 'updated content' WHERE id = 1;

SELECT id, content, category, category_embedding FROM test_emb_full WHERE id = 1;

-- ============================================
-- 测试4: INSERT提供category值
-- ============================================
INSERT INTO test_emb_full (id, content, category) VALUES (4, 'new item', 'test category');

SELECT id, content, category, category_embedding FROM test_emb_full WHERE id = 4;

-- ============================================
-- 测试5: NULL值处理
-- ============================================
INSERT INTO test_emb_full (id, content) VALUES (5, NULL);

SELECT id, content, category, category_embedding FROM test_emb_full WHERE id = 5;

-- ============================================
-- 测试6: <-> 操作符查询（L2距离）
-- ============================================
SELECT id, content, category <-> 'search text' AS distance
FROM test_emb_full
ORDER BY category <-> 'search text'
LIMIT 5;

-- ============================================
-- 测试7: <=> 操作符查询（余弦距离）
-- ============================================
SELECT id, content, category <=> 'search text' AS distance
FROM test_emb_full
ORDER BY category <=> 'search text'
LIMIT 5;

-- ============================================
-- 测试8: <#> 操作符查询（内积）
-- ============================================
SELECT id, content, category <#> 'search text' AS distance
FROM test_emb_full
ORDER BY category <#> 'search text'
LIMIT 5;

-- ============================================
-- 测试9: <+> 操作符查询（L1距离）
-- ============================================
SELECT id, content, category <+> 'search text' AS distance
FROM test_emb_full
ORDER BY category <+> 'search text'
LIMIT 5;

-- ============================================
-- 测试10: WHERE条件中的向量距离查询
-- ============================================
SELECT id, content, category <=> 'search text' AS distance
FROM test_emb_full
WHERE category <=> 'search text' < 1.0
ORDER BY category <=> 'search text';

-- ============================================
-- 测试11: 多个embedding列使用不同函数
-- ============================================
CREATE TABLE test_emb_multi_func (
    id int PRIMARY KEY,
    title text,
    body text,
    title_vec text EMBEDDING AS (title_embedding(title)) STORED,
    body_vec text EMBEDDING AS (simple_embedding(body)) STORED
) WITH (vector_len=3);

INSERT INTO test_emb_multi_func (id, title, body) VALUES (1, 'AI News', 'New breakthrough in AI');

SELECT id, title, body, title_vec_embedding, body_vec_embedding
FROM test_emb_multi_func;

SELECT id, title_vec <=> 'search' AS title_dist, body_vec <=> 'search' AS body_dist
FROM test_emb_multi_func;

-- ============================================
-- 测试12: 向量索引验证
-- ============================================
SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'test_emb_full';

-- ============================================
-- 测试13: 批量插入性能测试
-- ============================================
INSERT INTO test_emb_full (id, content)
SELECT generate_series(10, 109), 'batch test ' || generate_series(10, 109);

SELECT COUNT(*) AS total_rows FROM test_emb_full;

-- ============================================
-- 测试14: UPDATE embedding列直接修改
-- ============================================
UPDATE test_emb_full SET category = 'manual category' WHERE id = 1;

SELECT id, content, category, category_embedding FROM test_emb_full WHERE id = 1;

-- ============================================
-- 测试15: 查询重写验证 - EXPLAIN
-- ============================================
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM test_emb_full WHERE category <=> 'search' < 1.0 ORDER BY category <=> 'search';

-- ============================================
-- 测试16: 错误处理 - 非EMBEDDING列使用text距离操作符
-- ============================================
-- 以下查询应该报错
SELECT id FROM test_emb_full WHERE content <=> 'test' < 1.0 LIMIT 1;

-- ============================================
-- 测试17: pg_attrdef验证（embedding表达式存储）
-- ============================================
SELECT a.attname, pg_get_expr(d.adbin, d.adrelid) AS default_expr
FROM pg_attribute a
JOIN pg_attrdef d ON a.attrelid = d.adrelid AND a.attnum = d.adnum
WHERE a.attrelid = 'test_emb_full'::regclass
AND a.attname = 'category';

-- ============================================
-- 测试18: ALTER TABLE ADD EMBEDDING列
-- ============================================
ALTER TABLE test_emb_full ADD COLUMN summary text EMBEDDING AS (simple_embedding(content)) STORED;

SELECT attname, atttypid::regtype, attgenerated, attembedding
FROM pg_attribute
WHERE attrelid = 'test_emb_full'::regclass AND attnum > 0 AND NOT attisdropped AND attname LIKE '%summary%'
ORDER BY attnum;

-- ============================================
-- 测试19: 扩展自动安装验证
-- ============================================
SELECT extname, extnamespace::regnamespace, extversion FROM pg_extension ORDER BY extname;

-- ============================================
-- 测试20: GUC参数验证
-- ============================================
LOAD 'jolix_predict';
LOAD 'jolix_embedding';

SHOW jolix_predict.llm_api_url;
SHOW jolix_predict.llm_model;
SHOW jolix_embedding.model_name;

-- ============================================
-- 测试21: 清理测试数据
-- ============================================
DROP TABLE IF EXISTS test_emb_full;
DROP TABLE IF EXISTS test_emb_multi_func;
