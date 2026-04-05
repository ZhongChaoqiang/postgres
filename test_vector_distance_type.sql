-- 测试 jolixdb_vector_distance_type 参数

-- 测试 vector_l2_ops 值（默认值）
CREATE TABLE test_dist_l2 (id int, content text) WITH (jolixdb_vector_distance_type = 'vector_l2_ops');

-- 测试 vector_ip_ops 值
CREATE TABLE test_dist_ip (id int, content text) WITH (jolixdb_vector_distance_type = 'vector_ip_ops');

-- 测试 vector_cosine_ops 值
CREATE TABLE test_dist_cosine (id int, content text) WITH (jolixdb_vector_distance_type = 'vector_cosine_ops');

-- 测试不指定参数（使用默认值）
CREATE TABLE test_dist_default (id int, content text);

-- 查看表选项
SELECT relname, reloptions FROM pg_class WHERE relname LIKE 'test_dist%';

-- 测试所有参数组合
CREATE TABLE test_all_params (
    id int PRIMARY KEY,
    content text EMBEDDING
) WITH (
    jolixdb_embedding_vector_len = 128,
    jolixdb_vector_index_type = 'hnsw',
    jolixdb_vector_distance_type = 'vector_cosine_ops'
);

SELECT relname, reloptions FROM pg_class WHERE relname = 'test_all_params';

-- 清理
DROP TABLE test_dist_l2;
DROP TABLE test_dist_ip;
DROP TABLE test_dist_cosine;
DROP TABLE test_dist_default;
DROP TABLE test_all_params;
