-- Clean up any existing test tables
DROP TABLE IF EXISTS test_single_func CASCADE;
DROP TABLE IF EXISTS test_multi_func CASCADE;
DROP TABLE IF EXISTS test_real_world CASCADE;
DROP FUNCTION IF EXISTS simple_predict(record);
DROP FUNCTION IF EXISTS temp_predict(record);
DROP FUNCTION IF EXISTS humidity_predict(record);
DROP FUNCTION IF EXISTS temp_predict_v2(record);
DROP FUNCTION IF EXISTS humidity_predict_v2(record);

-- Test predict_function with multiple columns
-- Test 1: Single function for all PREDICT columns
\echo 'Test 1: Single function for all PREDICT columns'

-- Create a simple predict function
CREATE OR REPLACE FUNCTION simple_predict(rec record) 
RETURNS FLOAT AS $$
BEGIN
    RETURN 100.0;
END;
$$ LANGUAGE plpgsql;

-- Create table with single predict function
CREATE TABLE test_single_func (
    id SERIAL PRIMARY KEY,
    temp FLOAT PREDICT,
    humidity FLOAT PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'simple_predict'
);

-- Insert data with NULL values to trigger prediction
INSERT INTO test_single_func (id) VALUES (DEFAULT);

-- Check results - explicitly query all columns
SELECT id, temp, temp_predict, temp_actual, humidity, humidity_predict, humidity_actual FROM test_single_func;

-- Clean up
DROP TABLE test_single_func;
DROP FUNCTION simple_predict(record);

-- Test 2: Different functions for different PREDICT columns
\echo 'Test 2: Different functions for different PREDICT columns'

-- Create predict functions for each column
CREATE OR REPLACE FUNCTION temp_predict(rec record) 
RETURNS FLOAT AS $$
BEGIN
    RETURN 25.5 * 1.05;
END;
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION humidity_predict(rec record) 
RETURNS FLOAT AS $$
BEGIN
    RETURN 60.0 * 0.95;
END;
$$ LANGUAGE plpgsql;

-- Create table with column-specific predict functions
CREATE TABLE test_multi_func (
    id SERIAL PRIMARY KEY,
    temp FLOAT PREDICT,
    humidity FLOAT PREDICT,
    pressure FLOAT PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'temp:temp_predict;humidity:humidity_predict'
);

-- Insert data with NULL values to trigger prediction
INSERT INTO test_multi_func (id) VALUES (DEFAULT);

-- Check results - explicitly query all columns
-- temp should be 25.5 * 1.05 = 26.775
-- temp_predict should be 26.775
-- humidity should be 60.0 * 0.95 = 57.0
-- humidity_predict should be 57.0
-- pressure should be NULL (no function specified)
-- pressure_predict should be NULL (no function specified)
SELECT id, temp, temp_predict, temp_actual, humidity, humidity_predict, humidity_actual, pressure, pressure_predict, pressure_actual FROM test_multi_func;

-- Clean up
DROP TABLE test_multi_func;
DROP FUNCTION temp_predict(record);
DROP FUNCTION humidity_predict(record);

\echo 'All tests completed!'

-- Test 3: Real-world scenario with feature columns
\echo 'Test 3: Real-world scenario with feature columns'

-- Create predict functions that use feature columns
CREATE OR REPLACE FUNCTION temp_predict_v2(rec record) 
RETURNS FLOAT AS $$
BEGIN
    -- Use feature1 and feature2 from the record to predict temperature
    RETURN rec.feature1 * 1.05 + rec.feature2 * 0.5;
END;
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION humidity_predict_v2(rec record) 
RETURNS FLOAT AS $$
BEGIN
    -- Use feature1 and feature2 from the record to predict humidity
    RETURN rec.feature1 * 0.3 + rec.feature2 * 0.7;
END;
$$ LANGUAGE plpgsql;

-- Create table with feature columns and PREDICT columns
CREATE TABLE test_real_world (
    id SERIAL PRIMARY KEY,
    feature1 FLOAT,
    feature2 FLOAT,
    temp FLOAT PREDICT,
    humidity FLOAT PREDICT
) WITH (
    predict_timing = immediate,
    predict_function = 'temp:temp_predict_v2;humidity:humidity_predict_v2'
);

-- Insert data with feature values but NULL PREDICT columns
INSERT INTO test_real_world (feature1, feature2) VALUES (10.0, 20.0);

-- Check results - explicitly query all columns
-- temp should be 10.0 * 1.05 + 20.0 * 0.5 = 10.5 + 10.0 = 20.5
-- temp_predict should be 20.5
-- humidity should be 10.0 * 0.3 + 20.0 * 0.7 = 3.0 + 14.0 = 17.0
-- humidity_predict should be 17.0
SELECT id, feature1, feature2, temp, temp_predict, temp_actual, humidity, humidity_predict, humidity_actual FROM test_real_world;

-- Clean up
DROP TABLE test_real_world;
DROP FUNCTION temp_predict_v2(record);
DROP FUNCTION humidity_predict_v2(record);

\echo 'Test 3 completed!'
