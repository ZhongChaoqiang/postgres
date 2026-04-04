-- 测试 jolixdb_vector_index_type 参数

-- 测试 hnsw 值
CREATE TABLE test_index_hnsw (id int, content text) WITH (jolixdb_vector_index_type = 'hnsw');

-- 测试 ivfflat 值（默认值）
CREATE TABLE test_index_ivfflat (id int, content text) WITH (jolixdb_vector_index_type = 'ivfflat');

-- 测试不指定参数（使用默认值）
CREATE TABLE test_index_default (id int, content text);

-- 查看表选项
SELECT relname, reloptions FROM pg_class WHERE relname LIKE 'test_index%';

-- 测试无效值
-- CREATE TABLE test_index_invalid (id int) WITH (jolixdb_vector_index_type = 'invalid');

-- 清理
DROP TABLE test_index_hnsw;
DROP TABLE test_index_ivfflat;
DROP TABLE test_index_default;
