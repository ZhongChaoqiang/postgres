\pset pager off
\set ON_ERROR_STOP off

\echo '============================================================'
\echo '  PREDICT 标签列功能综合测试'
\echo '============================================================'

-- 清理环境
DROP TABLE IF EXISTS test_predict_basic CASCADE;
DROP TABLE IF EXISTS test_predict_immediate CASCADE;
DROP TABLE IF EXISTS test_predict_deferred CASCADE;
DROP TABLE IF EXISTS test_predict_multi_func CASCADE;
DROP TABLE IF EXISTS test_predict_single_func CASCADE;
DROP TABLE IF EXISTS test_predict_alter CASCADE;
DROP TABLE IF EXISTS test_predict_addcol CASCADE;
DROP TABLE IF EXISTS test_predict_update CASCADE;
DROP TABLE IF EXISTS test_predict_nonnull CASCADE;
DROP TABLE IF EXISTS test_predict_is_func CASCADE;
DROP TABLE IF EXISTS test_predict_hidden CASCADE;
DROP TABLE IF EXISTS test_predict_trigger_check CASCADE;
DROP TABLE IF EXISTS test_predict_index_check CASCADE;
DROP FUNCTION IF EXISTS simple_predict(record) CASCADE;
DROP FUNCTION IF EXISTS temp_predict_func(record) CASCADE;
DROP FUNCTION IF EXISTS humidity_predict_func(record) CASCADE;
DROP FUNCTION IF EXISTS predict_double(record) CASCADE;
DROP FUNCTION IF EXISTS predict_with_features(record) CASCADE;

-- ============================================================
-- 测试1: CREATE TABLE with PREDICT column - 基本DDL
-- ============================================================
\echo ''
\echo '===== 测试1: CREATE TABLE with PREDICT column - 基本DDL ====='

CREATE TABLE test_predict_basic (
    id SERIAL PRIMARY KEY,
    value FLOAT PREDICT
);

-- 检查表结构: 隐藏列 _predict 和 _actual 是否创建
\echo '-- 检查所有列(包括隐藏列)'
SELECT attname, atttypid::regtype, attpredict, attembedding, atthidden
FROM pg_attribute
WHERE attrelid = 'test_predict_basic'::regclass AND attnum > 0 AND NOT attisdropped
ORDER BY attnum;

-- 检查触发器是否自动创建
\echo '-- 检查自动创建的触发器(包括内部触发器)'
SELECT tgname, tgisinternal, proname
FROM pg_trigger t
JOIN pg_proc p ON t.tgfoid = p.oid
WHERE t.tgrelid = 'test_predict_basic'::regclass
ORDER BY tgname;

-- 检查索引是否自动创建
\echo '-- 检查自动创建的索引'
SELECT indexname, indexdef
FROM pg_indexes
WHERE tablename = 'test_predict_basic'
ORDER BY indexname;

DROP TABLE test_predict_basic CASCADE;

-- ============================================================
-- 测试2: PREDICT列 - immediate模式 - INSERT触发预测
-- ============================================================
\echo ''
\echo '===== 测试2: immediate模式 - INSERT触发预测 ====='

CREATE OR REPLACE FUNCTION simple_predict(rec record)
RETURNS FLOAT AS $$
BEGIN
    RETURN 100.0;
END;
$$ LANGUAGE plpgsql;

CREATE TABLE test_predict_immediate (
    id SERIAL PRIMARY KEY,
    value FLOAT PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'simple_predict'
);

-- INSERT with NULL PREDICT column -> should trigger prediction
INSERT INTO test_predict_immediate (id) VALUES (DEFAULT);

\echo '-- 查询结果: value应为100.0, value_predict应为100.0, value_actual应为NULL'
SELECT id, value, value_predict, value_actual FROM test_predict_immediate;

DROP TABLE test_predict_immediate CASCADE;
DROP FUNCTION simple_predict(record) CASCADE;

-- ============================================================
-- 测试3: PREDICT列 - deferred模式 - INSERT不触发预测
-- ============================================================
\echo ''
\echo '===== 测试3: deferred模式 - INSERT不触发预测 ====='

CREATE OR REPLACE FUNCTION simple_predict(rec record)
RETURNS FLOAT AS $$
BEGIN
    RETURN 100.0;
END;
$$ LANGUAGE plpgsql;

CREATE TABLE test_predict_deferred (
    id SERIAL PRIMARY KEY,
    value FLOAT PREDICT
) WITH (
    predict_timing = deferred,
    predict_function = 'simple_predict'
);

-- INSERT with NULL PREDICT column -> should NOT trigger prediction in deferred mode
INSERT INTO test_predict_deferred (id) VALUES (DEFAULT);

\echo '-- 查询结果: value应为NULL, value_predict应为NULL, value_actual应为NULL'
SELECT id, value, value_predict, value_actual FROM test_predict_deferred;

DROP TABLE test_predict_deferred CASCADE;
DROP FUNCTION simple_predict(record) CASCADE;

-- ============================================================
-- 测试4: 多个PREDICT列 - 单一预测函数
-- ============================================================
\echo ''
\echo '===== 测试4: 多个PREDICT列 - 单一预测函数 ====='

CREATE OR REPLACE FUNCTION simple_predict(rec record)
RETURNS FLOAT AS $$
BEGIN
    RETURN 100.0;
END;
$$ LANGUAGE plpgsql;

CREATE TABLE test_predict_single_func (
    id SERIAL PRIMARY KEY,
    temp FLOAT PREDICT,
    humidity FLOAT PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'simple_predict'
);

INSERT INTO test_predict_single_func (id) VALUES (DEFAULT);

\echo '-- 查询结果: temp=100.0, humidity=100.0 (同一函数应用于所有PREDICT列)'
SELECT id, temp, temp_predict, temp_actual, humidity, humidity_predict, humidity_actual
FROM test_predict_single_func;

DROP TABLE test_predict_single_func CASCADE;
DROP FUNCTION simple_predict(record) CASCADE;

-- ============================================================
-- 测试5: 多个PREDICT列 - 列特定预测函数 (col:func;col2:func2)
-- ============================================================
\echo ''
\echo '===== 测试5: 多个PREDICT列 - 列特定预测函数 ====='

CREATE OR REPLACE FUNCTION temp_predict_func(rec record)
RETURNS FLOAT AS $$
BEGIN
    RETURN 25.5 * 1.05;
END;
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION humidity_predict_func(rec record)
RETURNS FLOAT AS $$
BEGIN
    RETURN 60.0 * 0.95;
END;
$$ LANGUAGE plpgsql;

CREATE TABLE test_predict_multi_func (
    id SERIAL PRIMARY KEY,
    temp FLOAT PREDICT,
    humidity FLOAT PREDICT,
    pressure FLOAT PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'temp:temp_predict_func;humidity:humidity_predict_func'
);

INSERT INTO test_predict_multi_func (id) VALUES (DEFAULT);

\echo '-- 查询结果: temp=26.775, humidity=57.0, pressure=NULL(无指定函数)'
SELECT id, temp, temp_predict, temp_actual,
       humidity, humidity_predict, humidity_actual,
       pressure, pressure_predict, pressure_actual
FROM test_predict_multi_func;

DROP TABLE test_predict_multi_func CASCADE;
DROP FUNCTION temp_predict_func(record) CASCADE;
DROP FUNCTION humidity_predict_func(record) CASCADE;

-- ============================================================
-- 测试6: ALTER TABLE SET PREDICT FUNCTION
-- ============================================================
\echo ''
\echo '===== 测试6: ALTER TABLE SET PREDICT FUNCTION ====='

CREATE OR REPLACE FUNCTION simple_predict(rec record)
RETURNS FLOAT AS $$
BEGIN
    RETURN 100.0;
END;
$$ LANGUAGE plpgsql;

CREATE TABLE test_predict_alter (
    id SERIAL PRIMARY KEY,
    value FLOAT PREDICT
) WITH (
    predict_timing = immediate
);

-- 没有设置predict_function, INSERT应该不会触发预测
INSERT INTO test_predict_alter (id) VALUES (DEFAULT);

\echo '-- 查询结果: value应为NULL(没有predict_function)'
SELECT id, value, value_predict, value_actual FROM test_predict_alter;

-- 通过ALTER TABLE设置predict_function
ALTER TABLE test_predict_alter SET PREDICT FUNCTION simple_predict;

-- 检查reloptions是否更新
\echo '-- 检查reloptions'
SELECT reloptions FROM pg_class WHERE relname = 'test_predict_alter';

-- 再次INSERT, 现在应该触发预测
INSERT INTO test_predict_alter (id) VALUES (DEFAULT);

\echo '-- 查询结果: 第二行value应为100.0'
SELECT id, value, value_predict, value_actual FROM test_predict_alter;

DROP TABLE test_predict_alter CASCADE;
DROP FUNCTION simple_predict(record) CASCADE;

-- ============================================================
-- 测试7: ALTER TABLE ADD COLUMN with PREDICT
-- ============================================================
\echo ''
\echo '===== 测试7: ALTER TABLE ADD COLUMN with PREDICT ====='

CREATE OR REPLACE FUNCTION simple_predict(rec record)
RETURNS FLOAT AS $$
BEGIN
    RETURN 42.0;
END;
$$ LANGUAGE plpgsql;

CREATE TABLE test_predict_addcol (
    id SERIAL PRIMARY KEY,
    name TEXT
);

-- 添加PREDICT列
ALTER TABLE test_predict_addcol ADD COLUMN score FLOAT PREDICT;

-- 检查隐藏列是否创建
\echo '-- 检查列(包括隐藏列)'
SELECT attname, atttypid::regtype, attpredict, atthidden
FROM pg_attribute
WHERE attrelid = 'test_predict_addcol'::regclass AND attnum > 0 AND NOT attisdropped
ORDER BY attnum;

-- 检查触发器是否创建
\echo '-- 检查触发器(包括内部触发器)'
SELECT tgname, tgisinternal, proname
FROM pg_trigger t
JOIN pg_proc p ON t.tgfoid = p.oid
WHERE t.tgrelid = 'test_predict_addcol'::regclass
ORDER BY tgname;

-- 设置predict_function并插入数据
ALTER TABLE test_predict_addcol SET PREDICT FUNCTION simple_predict;
ALTER TABLE test_predict_addcol SET (predict_timing = immediate);

INSERT INTO test_predict_addcol (name) VALUES ('test');

\echo '-- 查询结果: score应为42.0'
SELECT id, name, score, score_predict, score_actual FROM test_predict_addcol;

DROP TABLE test_predict_addcol CASCADE;
DROP FUNCTION simple_predict(record) CASCADE;

-- ============================================================
-- 测试8: UPDATE触发预测
-- ============================================================
\echo ''
\echo '===== 测试8: UPDATE触发预测 ====='

CREATE OR REPLACE FUNCTION simple_predict(rec record)
RETURNS FLOAT AS $$
BEGIN
    RETURN 100.0;
END;
$$ LANGUAGE plpgsql;

CREATE TABLE test_predict_update (
    id SERIAL PRIMARY KEY,
    value FLOAT PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'simple_predict'
);

-- 先插入一行(触发预测)
INSERT INTO test_predict_update (id) VALUES (DEFAULT);

\echo '-- 插入后: value=100.0'
SELECT id, value, value_predict, value_actual FROM test_predict_update;

-- UPDATE将value设为NULL -> 应该重新触发预测
UPDATE test_predict_update SET value = NULL WHERE id = 1;

\echo '-- UPDATE后(value=NULL): value应为100.0(重新预测)'
SELECT id, value, value_predict, value_actual FROM test_predict_update;

DROP TABLE test_predict_update CASCADE;
DROP FUNCTION simple_predict(record) CASCADE;

-- ============================================================
-- 测试9: INSERT with non-NULL PREDICT column value
-- ============================================================
\echo ''
\echo '===== 测试9: INSERT with non-NULL PREDICT column value ====='

CREATE OR REPLACE FUNCTION simple_predict(rec record)
RETURNS FLOAT AS $$
BEGIN
    RETURN 100.0;
END;
$$ LANGUAGE plpgsql;

CREATE TABLE test_predict_nonnull (
    id SERIAL PRIMARY KEY,
    value FLOAT PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'simple_predict'
);

-- INSERT with explicit value -> should NOT trigger prediction, store in _actual
INSERT INTO test_predict_nonnull (value) VALUES (50.0);

\echo '-- 查询结果: value=50.0(用户输入), value_actual=50.0, value_predict=NULL(未触发预测)'
SELECT id, value, value_predict, value_actual FROM test_predict_nonnull;

DROP TABLE test_predict_nonnull CASCADE;
DROP FUNCTION simple_predict(record) CASCADE;

-- ============================================================
-- 测试10: is_predict_column() 函数
-- ============================================================
\echo ''
\echo '===== 测试10: is_predict_column() 函数 ====='

CREATE TABLE test_predict_is_func (
    id SERIAL PRIMARY KEY,
    predict_col FLOAT PREDICT,
    normal_col FLOAT
);

\echo '-- is_predict_column检查PREDICT列'
SELECT is_predict_column('test_predict_is_func'::regclass, 'predict_col') AS is_predict_true;

\echo '-- is_predict_column检查普通列'
SELECT is_predict_column('test_predict_is_func'::regclass, 'normal_col') AS is_predict_false;

\echo '-- is_predict_column检查不存在的列'
SELECT is_predict_column('test_predict_is_func'::regclass, 'nonexistent') AS is_nonexistent_false;

DROP TABLE test_predict_is_func CASCADE;

-- ============================================================
-- 测试11: 隐藏列 - SELECT * 不显示 _predict 和 _actual
-- ============================================================
\echo ''
\echo '===== 测试11: 隐藏列 - SELECT * 不显示隐藏列 ====='

CREATE TABLE test_predict_hidden (
    id SERIAL PRIMARY KEY,
    value FLOAT PREDICT
);

\echo '-- SELECT * 应该只显示id和value,不显示隐藏列'
SELECT * FROM test_predict_hidden;

\echo '-- 显式指定列名可以查询隐藏列'
SELECT id, value, value_predict, value_actual FROM test_predict_hidden;

DROP TABLE test_predict_hidden CASCADE;

-- ============================================================
-- 测试12: 触发器命名规则检查
-- ============================================================
\echo ''
\echo '===== 测试12: 触发器命名规则检查 ====='

CREATE TABLE test_predict_trigger_check (
    id SERIAL PRIMARY KEY,
    temperature FLOAT PREDICT,
    humidity FLOAT PREDICT
);

\echo '-- 检查触发器名称格式: pg_predict_{colname}_{relOid}'
SELECT tgname, tgisinternal, proname
FROM pg_trigger t
JOIN pg_proc p ON t.tgfoid = p.oid
WHERE t.tgrelid = 'test_predict_trigger_check'::regclass
ORDER BY tgname;

DROP TABLE test_predict_trigger_check CASCADE;

-- ============================================================
-- 测试13: 复合索引检查
-- ============================================================
\echo ''
\echo '===== 测试13: 复合索引检查 ====='

CREATE TABLE test_predict_index_check (
    id SERIAL PRIMARY KEY,
    value FLOAT PREDICT
);

\echo '-- 检查索引: 应有 {tablename}_{colname}_predict_idx'
SELECT indexname, indexdef
FROM pg_indexes
WHERE tablename = 'test_predict_index_check'
ORDER BY indexname;

DROP TABLE test_predict_index_check CASCADE;

-- ============================================================
-- 测试14: 预测函数使用行中的特征列
-- ============================================================
\echo ''
\echo '===== 测试14: 预测函数使用行中的特征列 ====='

CREATE OR REPLACE FUNCTION predict_with_features(rec record)
RETURNS FLOAT AS $$
BEGIN
    RETURN rec.feature1 * 1.05 + rec.feature2 * 0.5;
END;
$$ LANGUAGE plpgsql;

CREATE TABLE test_predict_features (
    id SERIAL PRIMARY KEY,
    feature1 FLOAT,
    feature2 FLOAT,
    prediction FLOAT PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'predict_with_features'
);

INSERT INTO test_predict_features (feature1, feature2) VALUES (10.0, 20.0);

\echo '-- 查询结果: prediction = 10.0*1.05 + 20.0*0.5 = 10.5 + 10.0 = 20.5'
SELECT id, feature1, feature2, prediction, prediction_predict, prediction_actual
FROM test_predict_features;

INSERT INTO test_predict_features (feature1, feature2) VALUES (5.0, 15.0);

\echo '-- 查询结果: prediction = 5.0*1.05 + 15.0*0.5 = 5.25 + 7.5 = 12.75'
SELECT id, feature1, feature2, prediction, prediction_predict, prediction_actual
FROM test_predict_features;

DROP TABLE test_predict_features CASCADE;
DROP FUNCTION predict_with_features(record) CASCADE;

-- ============================================================
-- 测试15: 多行插入测试
-- ============================================================
\echo ''
\echo '===== 测试15: 多行插入测试 ====='

CREATE OR REPLACE FUNCTION simple_predict(rec record)
RETURNS FLOAT AS $$
BEGIN
    RETURN 100.0;
END;
$$ LANGUAGE plpgsql;

CREATE TABLE test_predict_multirow (
    id SERIAL PRIMARY KEY,
    value FLOAT PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'simple_predict'
);

INSERT INTO test_predict_multirow (id) VALUES (DEFAULT), (DEFAULT), (DEFAULT);

\echo '-- 查询结果: 所有行的value都应为100.0'
SELECT id, value, value_predict, value_actual FROM test_predict_multirow ORDER BY id;

DROP TABLE test_predict_multirow CASCADE;
DROP FUNCTION simple_predict(record) CASCADE;

-- ============================================================
-- 测试16: predict_timing reloption检查
-- ============================================================
\echo ''
\echo '===== 测试16: predict_timing reloption检查 ====='

CREATE TABLE test_predict_timing_deferred (
    id SERIAL PRIMARY KEY,
    value FLOAT PREDICT
) WITH (
    predict_timing = deferred
);

\echo '-- deferred模式的reloptions'
SELECT reloptions FROM pg_class WHERE relname = 'test_predict_timing_deferred';

CREATE TABLE test_predict_timing_immediate (
    id SERIAL PRIMARY KEY,
    value FLOAT PREDICT
) WITH (
    predict_timing = immediate
);

\echo '-- immediate模式的reloptions'
SELECT reloptions FROM pg_class WHERE relname = 'test_predict_timing_immediate';

DROP TABLE test_predict_timing_deferred CASCADE;
DROP TABLE test_predict_timing_immediate CASCADE;

-- ============================================================
-- 测试17: predict_function reloption检查
-- ============================================================
\echo ''
\echo '===== 测试17: predict_function reloption检查 ====='

CREATE TABLE test_predict_func_opt (
    id SERIAL PRIMARY KEY,
    value FLOAT PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'simple_predict'
);

\echo '-- 检查reloptions包含predict_function'
SELECT reloptions FROM pg_class WHERE relname = 'test_predict_func_opt';

DROP TABLE test_predict_func_opt CASCADE;

-- ============================================================
-- 测试18: pg_attribute中attpredict标记检查
-- ============================================================
\echo ''
\echo '===== 测试18: pg_attribute中attpredict标记检查 ====='

CREATE TABLE test_predict_attr (
    id SERIAL PRIMARY KEY,
    predict_val FLOAT PREDICT,
    normal_val FLOAT
);

\echo '-- 检查attpredict标记: predict_val应为true, normal_val应为false'
SELECT attname, attpredict
FROM pg_attribute
WHERE attrelid = 'test_predict_attr'::regclass AND attnum > 0 AND NOT attisdropped
ORDER BY attnum;

DROP TABLE test_predict_attr CASCADE;

-- ============================================================
-- 测试19: 预测函数返回不同类型(INT)
-- ============================================================
\echo ''
\echo '===== 测试19: 预测函数返回不同类型(INT) ====='

CREATE OR REPLACE FUNCTION predict_double(rec record)
RETURNS FLOAT AS $$
BEGIN
    RETURN 3.14159;
END;
$$ LANGUAGE plpgsql;

CREATE TABLE test_predict_int (
    id SERIAL PRIMARY KEY,
    score FLOAT PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'predict_double'
);

INSERT INTO test_predict_int (id) VALUES (DEFAULT);

\echo '-- 查询结果: score=3.14159'
SELECT id, score, score_predict, score_actual FROM test_predict_int;

DROP TABLE test_predict_int CASCADE;
DROP FUNCTION predict_double(record) CASCADE;

\echo ''
\echo '============================================================'
\echo '  所有测试完成!'
\echo '============================================================'
