-- contrib/tsvector_funcs/tsvector_funcs--1.0.sql
-- Register timeseries vector helper functions and trigger function.

-- ts2v_moment: convert a float8[] array to a pgvector vector type.
-- Uses moment-based feature extraction (mean, stddev, min, max, skewness,
-- kurtosis, and quantile-based histogram).
CREATE FUNCTION ts2v_moment(float8[], int DEFAULT 384)
RETURNS vector
AS 'MODULE_PATHNAME', 'ts2v_moment'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

-- timeseries_vector_run: manually trigger vector computation for a vector table.
-- Takes the OID of the vector table. Returns the number of rows inserted/updated.
CREATE FUNCTION timeseries_vector_run(oid)
RETURNS integer
AS 'MODULE_PATHNAME', 'timeseries_vector_run'
LANGUAGE C VOLATILE;

-- tsvector_trigger_func: AFTER INSERT trigger on source table.
-- Implements three-rule decision logic that sends asynchronous
-- computation tasks via pg_notify('tsvector_task', payload_json).
CREATE FUNCTION tsvector_trigger_func()
RETURNS trigger
AS 'MODULE_PATHNAME', 'tsvector_trigger_func'
LANGUAGE C VOLATILE;

-- Helper to create the trigger on a source table for a given vector table.
CREATE OR REPLACE FUNCTION timeseries_register_trigger(source_table text)
RETURNS void AS $$
DECLARE
    trig_name text := 'tsvector_trigger';
BEGIN
    EXECUTE format(
        'DROP TRIGGER IF EXISTS %I ON %s',
        trig_name, source_table
    );
    EXECUTE format(
        'CREATE TRIGGER %I AFTER INSERT ON %s FOR EACH ROW '
        'EXECUTE FUNCTION tsvector_trigger_func()',
        trig_name, source_table
    );
END;
$$ LANGUAGE plpgsql VOLATILE;

-- Helper to remove the trigger when a vector table is dropped.
CREATE OR REPLACE FUNCTION timeseries_unregister_trigger(source_table text)
RETURNS void AS $$
BEGIN
    EXECUTE format(
        'DROP TRIGGER IF EXISTS tsvector_trigger ON %s',
        source_table
    );
END;
$$ LANGUAGE plpgsql VOLATILE;

-- ====================================================================
-- timeseries_vector_search — vector similarity search on a timeseries
-- vector table.
--
-- Usage:
--   SELECT * FROM timeseries_vector_search(
--       'sensor_vec',                       -- vector table name
--       '2026-09-16 10:00:00+00',           -- target slice start (auto-aligned)
--       'device_id = 123',                  -- optional carry-column filter
--       5                                   -- top-N results
--   );
--
-- The function reads bucket_interval and carry_columns from reloptions,
-- auto-aligns target_slice_start to the nearest bucket boundary, then
-- looks up the target slice's embedding. If the slice has not been
-- computed (_processed IS NULL or FALSE), the query returns zero rows.
-- Otherwise it returns the top-N most similar slices (by cosine distance).
--
-- Return columns:
--   slice_start, slice_end, carry columns, cosine_distance
-- ====================================================================
CREATE OR REPLACE FUNCTION timeseries_vector_search(
    vec_table           text,
    target_slice_start  timestamptz,
    carry_filter        text DEFAULT NULL,
    top_n               int  DEFAULT 10
) RETURNS TABLE (
    slice_start_out   timestamptz,
    slice_end_out     timestamptz,
    cosine_distance   float8
) AS $$
DECLARE
    v_oid              oid;
    v_bucket_interval  int;
    v_vector_column    text;
    v_slice_start      timestamptz;
    v_slice_end        timestamptz;
    v_target_vec       vector;
    v_sql              text;
BEGIN
    -- 1. Resolve OID and validate it's a real vector table.
    BEGIN
        v_oid := vec_table::regclass;
    EXCEPTION WHEN OTHERS THEN
        RAISE EXCEPTION 'relation "%" does not exist', vec_table;
    END;

    -- 2. Read reloptions from pg_class.reloptions.
    SELECT split_part(opt::text, '=', 2) INTO v_bucket_interval
    FROM pg_class, LATERAL unnest(reloptions) AS opt
    WHERE oid = v_oid AND opt LIKE 'timeseries.bucket_interval=%'
    LIMIT 1;

    IF v_bucket_interval IS NULL OR v_bucket_interval <= 0 THEN
        RAISE EXCEPTION 'vector table "%" has no valid timeseries.bucket_interval',
            vec_table;
    END IF;

    v_vector_column := NULL;
    SELECT split_part(opt::text, '=', 2) INTO v_vector_column
    FROM pg_class, LATERAL unnest(reloptions) AS opt
    WHERE oid = v_oid AND opt LIKE 'timeseries.vector_column=%'
    LIMIT 1;
    IF v_vector_column IS NULL OR v_vector_column = '' THEN
        v_vector_column := 'embedding';
    END IF;

    -- 3. Align target_slice_start to the nearest bucket boundary.
    --    Formula used by build_vector_run_query:
    --      bucket_epoch = FLOOR(extract(epoch FROM target) / bucket_interval)
    --                     * bucket_interval
    v_slice_start := to_timestamp(
        FLOOR(EXTRACT(EPOCH FROM target_slice_start) / v_bucket_interval)
        * v_bucket_interval
    );
    v_slice_end := to_timestamp(
        FLOOR(EXTRACT(EPOCH FROM target_slice_start) / v_bucket_interval)
        * v_bucket_interval + v_bucket_interval
    );

    -- 4. Look up target embedding (respect carry_filter if provided).
    v_sql := format(
        'SELECT %I FROM %s '
        'WHERE slice_start = $1',
        v_vector_column, vec_table
    );

    -- If a carry_filter was given, AND it in.
    IF carry_filter IS NOT NULL AND btrim(carry_filter) <> '' THEN
        v_sql := v_sql || format(' AND (%s)', carry_filter);
    END IF;

    -- Also require the slice to have been processed.
    v_sql := v_sql || ' AND (_processed IS TRUE OR _processed IS NULL)';

    EXECUTE v_sql INTO v_target_vec USING v_slice_start;

    -- 5. If no target embedding, return nothing.
    IF v_target_vec IS NULL THEN
        RETURN;
    END IF;

    -- 6. Build the similarity-search query.
    v_sql := format(
        'SELECT slice_start, slice_end, (%I <=> $1) AS cosine_distance '
        'FROM %s '
        'WHERE slice_start <> $2',
        v_vector_column, vec_table
    );

    IF carry_filter IS NOT NULL AND btrim(carry_filter) <> '' THEN
        v_sql := v_sql || format(' AND (%s)', carry_filter);
    END IF;

    v_sql := v_sql || format(
        ' ORDER BY %I <=> $1 LIMIT %L',
        v_vector_column, top_n
    );

    FOR slice_start_out, slice_end_out, cosine_distance IN
        EXECUTE v_sql USING v_target_vec, v_slice_start
    LOOP
        RETURN NEXT;
    END LOOP;
    RETURN;
END;
$$ LANGUAGE plpgsql STABLE;

-- ====================================================================
-- ts2v_pool — vector[] 输入的向量化函数，用于"向量表当 source"的二级场景。
--
-- 默认 ts2v_moment(float8[], int) 接受数值数组，当 value_column 是 vector
-- 类型（上一层向量表的 embedding 列）时 array_agg 得到 vector[]，类型不匹配。
-- ts2v_pool 用 UNNEST + pgvector 内置 avg() 聚合，把一组向量聚合成
-- 一个"中心向量"。
--
-- 参数 vecs: vector[] — array_agg 聚合来的一组向量
-- 参数 dim:  int      — 输出向量维度（保留签名一致性，当前实现不截断）
-- ====================================================================
CREATE OR REPLACE FUNCTION ts2v_pool(vecs vector[], dim int DEFAULT NULL)
RETURNS vector AS $$
BEGIN
    IF vecs IS NULL OR array_length(vecs, 1) IS NULL THEN
        RETURN NULL;
    END IF;
    RETURN (SELECT avg(v) FROM unnest(vecs) AS v);
END;
$$ LANGUAGE plpgsql IMMUTABLE;
