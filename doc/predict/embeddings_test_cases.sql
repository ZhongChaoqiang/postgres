-- ============================================================
-- CREATE EMBEDDINGS / DROP EMBEDDINGS 测试用例
-- 包含：DDL、DML、查询、错误场景
-- ============================================================

-- 准备：安装扩展
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS jolix_embedding;

-- ============================================================
-- 测试1：基础建表 + CREATE EMBEDDINGS
-- ============================================================
DROP TABLE IF EXISTS t_customers;
CREATE TABLE t_customers (
    id SERIAL PRIMARY KEY,
    age INTEGER,
    income FLOAT8,
    category TEXT
);

CREATE EMBEDDINGS demographic ON t_customers
    USING ft_transformer_embedding (age, income, category)
    WITH (vector_len = 128);

-- 验证：隐藏列已创建
SELECT attname, atttypid::regtype, atthidden, attvectorize, attgenerated
FROM pg_attribute
WHERE attrelid = 't_customers'::regclass AND attname = 'demographic';

-- 验证：触发器已创建
SELECT tgname, tgenabled FROM pg_trigger
WHERE tgrelid = 't_customers'::regclass AND tgname LIKE 'vectorize_%';

-- 验证：\d 显示表结构
\d t_customers

-- ============================================================
-- 测试2：INSERT 数据，验证触发器自动计算向量
-- ============================================================
INSERT INTO t_customers (age, income, category) VALUES (30, 50000.0, 'premium');
INSERT INTO t_customers (age, income, category) VALUES (25, 35000.0, 'basic');
INSERT INTO t_customers (age, income, category) VALUES (45, 80000.0, 'premium');

-- 验证：隐藏列有值（显式查询隐藏列）
SELECT id, age, income, category, demographic FROM t_customers;

-- 验证：SELECT * 不显示隐藏列
SELECT * FROM t_customers;

-- ============================================================
-- 测试3：UPDATE 数据，验证触发器重新计算
-- ============================================================
UPDATE t_customers SET age = 31, income = 55000.0 WHERE id = 1;

-- 验证：向量已更新
SELECT id, age, income, demographic FROM t_customers WHERE id = 1;

-- ============================================================
-- 测试4：向量相似性查询 - Cosine 距离
-- ============================================================
-- 查找与第1行最相似的行（cosine距离越小越相似）
SELECT id, age, income, category,
       demographic <=> (SELECT demographic FROM t_customers WHERE id = 1) AS cosine_distance
FROM t_customers
WHERE id != 1
ORDER BY cosine_distance;

-- ============================================================
-- 测试5：向量相似性查询 - L2 距离
-- ============================================================
SELECT id, age, income, category,
       demographic <-> (SELECT demographic FROM t_customers WHERE id = 1) AS l2_distance
FROM t_customers
WHERE id != 1
ORDER BY l2_distance;

-- ============================================================
-- 测试6：向量相似性查询 - 内积距离
-- ============================================================
SELECT id, age, income, category,
       demographic <#> (SELECT demographic FROM t_customers WHERE id = 1) AS ip_distance
FROM t_customers
WHERE id != 1
ORDER BY ip_distance;

-- ============================================================
-- 测试7：Top-K 相似性搜索
-- ============================================================
SELECT id, age, income, category,
       demographic <=> (SELECT demographic FROM t_customers WHERE id = 1) AS cosine_dist
FROM t_customers
WHERE id != 1
ORDER BY cosine_dist
LIMIT 2;

-- ============================================================
-- 测试8：条件过滤 + 向量搜索
-- ============================================================
SELECT id, age, income, category,
       demographic <=> (SELECT demographic FROM t_customers WHERE id = 1) AS cosine_dist
FROM t_customers
WHERE category = 'premium' AND id != 1
ORDER BY cosine_dist;

-- ============================================================
-- 测试9：跨表向量相似性查询
-- ============================================================
DROP TABLE IF EXISTS t_products;
CREATE TABLE t_products (
    id SERIAL PRIMARY KEY,
    price FLOAT8,
    rating FLOAT8,
    brand TEXT
);

CREATE EMBEDDINGS product_vec ON t_products
    USING ft_transformer_embedding (price, brand)
    WITH (vector_len = 128);

INSERT INTO t_products (price, rating, brand) VALUES (99.9, 4.5, 'BrandA');
INSERT INTO t_products (price, rating, brand) VALUES (199.9, 3.5, 'BrandB');

-- 跨表：查找与客户1的demographic最相似的产品
SELECT p.id, p.price, p.brand,
       p.product_vec <=> c.demographic AS cross_distance
FROM t_products p, t_customers c
WHERE c.id = 1
ORDER BY cross_distance;

DROP EMBEDDINGS product_vec ON t_products;
DROP TABLE t_products;

-- ============================================================
-- 测试10：DROP EMBEDDINGS
-- ============================================================
DROP EMBEDDINGS demographic ON t_customers;

-- 验证：隐藏列已删除
SELECT count(*) = 0 AS col_dropped FROM pg_attribute
WHERE attrelid = 't_customers'::regclass AND attname = 'demographic';

-- 验证：触发器已删除
SELECT count(*) = 0 AS trigger_dropped FROM pg_trigger
WHERE tgrelid = 't_customers'::regclass AND tgname LIKE 'vectorize_%';

-- 验证：表结构恢复
\d t_customers

-- ============================================================
-- 测试11：CREATE EMBEDDINGS IF NOT EXISTS
-- ============================================================
CREATE EMBEDDINGS demo_vec ON t_customers
    USING ft_transformer_embedding (age, income)
    WITH (vector_len = 64);

-- 再次创建同名（应跳过并给出 NOTICE）
CREATE EMBEDDINGS IF NOT EXISTS demo_vec ON t_customers
    USING ft_transformer_embedding (age, income)
    WITH (vector_len = 64);

-- 不带 IF NOT EXISTS 创建同名（应报错）
-- CREATE EMBEDDINGS demo_vec ON t_customers
--     USING ft_transformer_embedding (age, income)
--     WITH (vector_len = 64);

-- ============================================================
-- 测试12：DROP EMBEDDINGS IF EXISTS
-- ============================================================
DROP EMBEDDINGS demo_vec ON t_customers;

-- 再次删除（应跳过并给出 NOTICE）
DROP EMBEDDINGS IF EXISTS demo_vec ON t_customers;

-- 不带 IF EXISTS 删除不存在的（应报错）
-- DROP EMBEDDINGS demo_vec ON t_customers;

-- ============================================================
-- 测试13：不同 vector_len 参数
-- ============================================================
CREATE EMBEDDINGS vec64 ON t_customers
    USING ft_transformer_embedding (age)
    WITH (vector_len = 64);

SELECT attname, atttypmod FROM pg_attribute
WHERE attrelid = 't_customers'::regclass AND attname = 'vec64';

DROP EMBEDDINGS vec64 ON t_customers;

-- ============================================================
-- 测试14：单列 EMBEDDINGS
-- ============================================================
CREATE EMBEDDINGS single_col ON t_customers
    USING ft_transformer_embedding (category)
    WITH (vector_len = 32);

SELECT id, category, single_col FROM t_customers;

DROP EMBEDDINGS single_col ON t_customers;

-- ============================================================
-- 测试15：同表多 EMBEDDINGS
-- ============================================================
CREATE EMBEDDINGS vec1 ON t_customers
    USING ft_transformer_embedding (age)
    WITH (vector_len = 32);

CREATE EMBEDDINGS vec2 ON t_customers
    USING ft_transformer_embedding (income)
    WITH (vector_len = 32);

INSERT INTO t_customers (age, income, category) VALUES (35, 60000.0, 'standard');

-- 验证：两个向量列都有值
SELECT id, age, income, vec1, vec2 FROM t_customers WHERE id = 4;

-- 多向量列相似性查询
SELECT id, age, income,
       vec1 <=> (SELECT vec1 FROM t_customers WHERE id = 1) AS vec1_dist,
       vec2 <=> (SELECT vec2 FROM t_customers WHERE id = 1) AS vec2_dist
FROM t_customers
WHERE id != 1
ORDER BY vec1_dist;

DROP EMBEDDINGS vec1 ON t_customers;
DROP EMBEDDINGS vec2 ON t_customers;

-- ============================================================
-- 测试16：多表多 EMBEDDINGS
-- ============================================================
DROP TABLE IF EXISTS t_products;
CREATE TABLE t_products (
    id SERIAL PRIMARY KEY,
    price FLOAT8,
    rating FLOAT8,
    brand TEXT
);

CREATE EMBEDDINGS basic_features ON t_products
    USING ft_transformer_embedding (price, brand)
    WITH (vector_len = 64);

CREATE EMBEDDINGS performance ON t_products
    USING ft_transformer_embedding (price, rating)
    WITH (vector_len = 64);

INSERT INTO t_products (price, rating, brand) VALUES (99.9, 4.5, 'BrandA');

-- 验证：两个隐藏向量列都有值
SELECT id, price, rating, brand, basic_features, performance FROM t_products;

-- 验证：SELECT * 不显示隐藏列
SELECT * FROM t_products;

DROP EMBEDDINGS basic_features ON t_products;
DROP EMBEDDINGS performance ON t_products;

-- ============================================================
-- 测试17：错误场景 - 不存在的列
-- ============================================================
-- CREATE EMBEDDINGS err_vec ON t_customers
--     USING ft_transformer_embedding (nonexistent_col)
--     WITH (vector_len = 64);
-- 预期：ERROR: column "nonexistent_col" does not exist

-- ============================================================
-- 测试18：错误场景 - 不存在的函数
-- ============================================================
-- CREATE EMBEDDINGS err_vec ON t_customers
--     USING nonexistent_function (age)
--     WITH (vector_len = 64);
-- 预期：触发器创建时可能报错或运行时报错

-- ============================================================
-- 测试19：错误场景 - 不存在的表
-- ============================================================
-- CREATE EMBEDDINGS err_vec ON nonexistent_table
--     USING ft_transformer_embedding (age)
--     WITH (vector_len = 64);
-- 预期：ERROR: relation "nonexistent_table" does not exist

-- ============================================================
-- 测试20：错误场景 - 与现有列同名
-- ============================================================
-- CREATE EMBEDDINGS age ON t_customers
--     USING ft_transformer_embedding (age)
--     WITH (vector_len = 64);
-- 预期：ERROR: column "age" of relation "t_customers" already exists

-- ============================================================
-- 测试21：错误场景 - VECTORIZE 关键字不再可用
-- ============================================================
-- CREATE VECTORIZE test ON t_customers
--     USING ft_transformer_embedding (age)
--     WITH (vector_len = 64);
-- 预期：ERROR: syntax error at or near "VECTORIZE"

-- ============================================================
-- 清理
-- ============================================================
DROP TABLE IF EXISTS t_customers;
DROP TABLE IF EXISTS t_products;
