-- contrib/tsvector_funcs/tsvector_funcs--1.0.sql
-- Register ts2v and timeseries_vector_run functions.

-- ts2v: convert a float8[] array to a vector of specified dimension.
-- Applies min-max normalization, then pads cyclically to target_dim.
CREATE FUNCTION ts2v(float8[], int DEFAULT 384)
RETURNS vector
AS 'MODULE_PATHNAME', 'ts2v'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

-- timeseries_vector_run: manually trigger vector computation for a vector table.
CREATE FUNCTION timeseries_vector_run(text)
RETURNS text
AS 'MODULE_PATHNAME', 'timeseries_vector_run'
LANGUAGE C VOLATILE;
