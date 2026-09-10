-- contrib/tsvector_funcs/tsvector_funcs--1.0.sql
-- Register ts2v_moment and timeseries_vector_run functions.

-- ts2v_moment: convert a float8[] array (a time series) to a vector of the
-- specified dimension using moment-based feature extraction
-- (mean, stddev, skewness, kurtosis, and 5th..8th standardized moments,
-- soft-sign bounded to [-1, 1], then cyclically repeated to target dimension).
CREATE FUNCTION ts2v_moment(float8[], int DEFAULT 384)
RETURNS vector
AS 'MODULE_PATHNAME', 'ts2v_moment'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

-- timeseries_vector_run: manually trigger vector computation for a vector table.
CREATE FUNCTION timeseries_vector_run(text)
RETURNS text
AS 'MODULE_PATHNAME', 'timeseries_vector_run'
LANGUAGE C VOLATILE;
