-- PREDICT AS 语法测试
CREATE FUNCTION add_tax(numeric) RETURNS numeric
AS $$ SELECT $1 * 1.1 $$ LANGUAGE SQL IMMUTABLE;

CREATE TABLE test_predict_basic (
    id int PRIMARY KEY,
    price numeric,
    price_with_tax numeric PREDICT AS (add_tax(price)) STORED
) WITH (predict_timing=immediate);

-- INSERT不提供predict值
INSERT INTO test_predict_basic (id, price) VALUES (1, 100);

SELECT id, price, price_with_tax, price_with_tax_predict, price_with_tax_actual
FROM test_predict_basic WHERE id = 1;

-- INSERT提供predict值
INSERT INTO test_predict_basic (id, price, price_with_tax) VALUES (2, 200, 220);

SELECT id, price, price_with_tax, price_with_tax_predict, price_with_tax_actual
FROM test_predict_basic WHERE id = 2;

-- UPDATE源列
UPDATE test_predict_basic SET price = 150 WHERE id = 1;

SELECT id, price, price_with_tax, price_with_tax_predict, price_with_tax_actual
FROM test_predict_basic WHERE id = 1;

-- UPDATE predict列
UPDATE test_predict_basic SET price_with_tax = 275 WHERE id = 2;

SELECT id, price, price_with_tax, price_with_tax_predict, price_with_tax_actual
FROM test_predict_basic WHERE id = 2;

-- 多个PREDICT列
CREATE FUNCTION double_price(numeric) RETURNS numeric
AS $$ SELECT $1 * 2 $$ LANGUAGE SQL IMMUTABLE;

CREATE TABLE test_predict_multi (
    id int PRIMARY KEY,
    price numeric,
    price_with_tax numeric PREDICT AS (add_tax(price)) STORED,
    price_doubled numeric PREDICT AS (double_price(price)) STORED
) WITH (predict_timing=immediate);

INSERT INTO test_predict_multi (id, price) VALUES (1, 100);

SELECT id, price, price_with_tax, price_with_tax_predict, price_doubled, price_doubled_predict
FROM test_predict_multi WHERE id = 1;

-- \d 显示
\d test_predict_basic
\d test_predict_multi
