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
-- Takes the OID of the vector table.
CREATE FUNCTION timeseries_vector_run(oid)
RETURNS boolean
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
