-- SELECT INFER 功能测试
\echo '=== 0. 创建测试表 ==='
DROP TABLE IF EXISTS infer_test CASCADE;
CREATE TABLE infer_test (
    id serial PRIMARY KEY,
    content text,
    sentiment text PREDICT AS (upper(content)) STORED
) WITH (predict_timing = deferred);

\echo '=== 1. 插入测试数据 ==='
INSERT INTO infer_test (content) VALUES ('hello'), ('world');

\echo '=== 2. 默认 SELECT（跳过推理）：sentiment 应为 NULL ==='
SELECT id, content, sentiment FROM infer_test ORDER BY id;

\echo '=== 3. SELECT INFER（立即推理）：sentiment 应为 HELLO, WORLD ==='
SELECT INFER id, content, sentiment FROM infer_test ORDER BY id;

\echo '=== 4. 再次默认 SELECT（验证持久化）：sentiment 应有值 ==='
SELECT id, content, sentiment FROM infer_test ORDER BY id;

\echo '=== 5. 测试与 DISTINCT 组合 ==='
SELECT INFER DISTINCT sentiment FROM infer_test ORDER BY sentiment;

\echo '=== 6. 测试与 WHERE 组合 ==='
TRUNCATE infer_test;
INSERT INTO infer_test (content) VALUES ('foo'), ('bar'), ('baz');
SELECT INFER id, content FROM infer_test WHERE sentiment IS NULL ORDER BY id;

\echo '=== 7. 查看未推理行数（默认跳过推理） ==='
TRUNCATE infer_test;
INSERT INTO infer_test (content) VALUES ('one'), ('two'), ('three');
SELECT count(*) AS null_count FROM infer_test WHERE sentiment IS NULL;

\echo '=== 测试完成 ==='
