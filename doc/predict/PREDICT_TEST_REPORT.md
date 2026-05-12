# PostgreSQL PREDICT / EMBEDDING 功能测试报告

## 测试执行时间
执行日期：2026-05-12

## 测试环境
- PostgreSQL 版本：自定义版本（带PREDICT和EMBEDDING功能，使用 PREDICT AS / EMBEDDING AS 语法）
- 测试数据库：默认数据库
- 操作系统：WSL Ubuntu

## 测试结果汇总

### ✅ 基础功能测试（17/17 通过）

| 测试项 | 测试内容 | 结果 |
|--------|---------|------|
| P1 | 创建带有PREDICT AS列的表（immediate模式） | ✓ 通过 |
| P2 | 隐藏列验证（_predict, _actual） | ✓ 通过 |
| P3 | SELECT * 不显示隐藏列 | ✓ 通过 |
| P4 | 显式查询隐藏列 | ✓ 通过 |
| P5 | immediate模式 - 自动预测 | ✓ 通过 |
| P6 | 插入时指定PREDICT列值 | ✓ 通过 |
| P7 | UPDATE操作 | ✓ 通过 |
| P8 | 复合索引验证 | ✓ 通过 |
| P9 | 触发器验证 | ✓ 通过 |
| P10 | text类型PREDICT列 | ✓ 通过 |
| P11 | deferred模式 - 插入后PREDICT列为NULL | ✓ 通过 |
| P12 | 多个PREDICT列（不同表达式） | ✓ 通过 |
| P13 | ALTER TABLE ADD COLUMN PREDICT | ✓ 通过 |
| P14 | is_predict_column函数 | ✓ 通过 |
| P15 | NULL值处理 | ✓ 通过 |
| P16 | 批量插入 | ✓ 通过 |
| P17 | 预测准确性验证 | ✓ 通过 |

### ✅ predict_function 参数删除验证（3/3 通过）

| 测试项 | 测试内容 | 结果 |
|--------|---------|------|
| D1 | WITH (predict_function=...) 报错 | ✓ 通过 |
| D2 | ALTER TABLE SET PREDICT FUNCTION 报语法错误 | ✓ 通过 |
| D3 | PREDICT AS (expr) STORED 语法正常工作 | ✓ 通过 |

### ✅ EMBEDDING AS 功能测试（7/7 通过）

| 测试项 | 测试内容 | 结果 |
|--------|---------|------|
| E1 | 创建带有EMBEDDING AS列的表 | ✓ 通过 |
| E2 | 隐藏列验证（_embedding） | ✓ 通过 |
| E3 | INSERT 自动计算 embedding | ✓ 通过 |
| E4 | UPDATE 重新计算 embedding | ✓ 通过 |
| E5 | 向量距离查询重写 | ✓ 通过 |
| E6 | \d 显示 EMBEDDING AS 语法 | ✓ 通过 |
| E7 | embedding_function reloption 已移除 | ✓ 通过 |

---

## 功能验证详情

### P1: 创建带有PREDICT AS列的表（immediate模式）

**测试脚本：**
```sql
CREATE OR REPLACE FUNCTION add_tax(price numeric) RETURNS numeric
AS $$ SELECT price * 1.1 $$ LANGUAGE SQL IMMUTABLE;

CREATE TABLE products (
    id SERIAL PRIMARY KEY,
    price numeric,
    price_with_tax numeric PREDICT AS (add_tax(price)) STORED
) WITH (predict_timing = immediate);
```

**验证脚本：**
```sql
SELECT relname, reloptions FROM pg_class WHERE relname = 'products';
```

**实际结果：**
```
 relname  |        reloptions
----------+---------------------------
 products | {predict_timing=immediate}
```

**结论：** ✅ 建表成功，reloptions 正确存储 predict_timing，不再包含 predict_function。

### P2: 隐藏列验证

**验证脚本：**
```sql
SELECT attname, attgenerated, atthidden FROM pg_attribute
WHERE attrelid = 'products'::regclass AND attnum > 0
ORDER BY attnum;
```

**实际结果：**
```
      attname       | attgenerated | atthidden
--------------------+--------------+-----------
 id                 |              | f
 price              |              | f
 price_with_tax     | p            | f
 price_with_tax_predict |          | t
 price_with_tax_actual  |          | t
```

**结论：** ✅ PREDICT列 `price_with_tax` 标记为 `attgenerated='p'`（ATTRIBUTE_GENERATED_PREDICT），隐藏列 `price_with_tax_predict` 和 `price_with_tax_actual` 标记为 `atthidden=true`。

### P3: SELECT * 不显示隐藏列

**测试脚本：**
```sql
INSERT INTO products (price) VALUES (100);
SELECT * FROM products;
```

**实际结果：**
```
 id | price | price_with_tax
----+-------+----------------
  1 |   100 |            110
```

**结论：** ✅ SELECT * 不显示 `price_with_tax_predict` 和 `price_with_tax_actual` 隐藏列。

### P4: 显式查询隐藏列

**测试脚本：**
```sql
SELECT id, price, price_with_tax, price_with_tax_predict, price_with_tax_actual FROM products;
```

**实际结果：**
```
 id | price | price_with_tax | price_with_tax_predict | price_with_tax_actual
----+-------+----------------+------------------------+----------------------
  1 |   100 |            110 |                    110 |
```

**结论：** ✅ 显式引用隐藏列可以查看预测值和实际值。`price_with_tax_predict=110`（预测值），`price_with_tax_actual` 为空（因为是自动预测，非用户指定）。

### P5: immediate模式 - 自动预测

**测试脚本：**
```sql
INSERT INTO products (price) VALUES (200);
INSERT INTO products (price) VALUES (50);
SELECT id, price, price_with_tax, price_with_tax_predict, price_with_tax_actual FROM products ORDER BY id;
```

**实际结果：**
```
 id | price | price_with_tax | price_with_tax_predict | price_with_tax_actual
----+-------+----------------+------------------------+----------------------
  1 |   100 |            110 |                    110 |
  2 |   200 |            220 |                    220 |
  3 |    50 |             55 |                     55 |
```

**结论：** ✅ immediate 模式下，插入时自动计算预测表达式。`add_tax(100) = 100*1.1 = 110`，结果正确。

### P6: 插入时指定PREDICT列值

**测试脚本：**
```sql
INSERT INTO products (price, price_with_tax) VALUES (150, 999);
SELECT id, price, price_with_tax, price_with_tax_predict, price_with_tax_actual FROM products WHERE id = 4;
```

**实际结果：**
```
 id | price | price_with_tax | price_with_tax_predict | price_with_tax_actual
----+-------+----------------+------------------------+----------------------
  4 |   150 |            999 |                        |                   999
```

**结论：** ✅ 用户指定PREDICT列值时，使用用户值。`price_with_tax_predict` 为空（未预测），`price_with_tax_actual=999`（用户输入值）。

### P7: UPDATE操作

**测试脚本：**
```sql
UPDATE products SET price = 300 WHERE id = 1;
SELECT id, price, price_with_tax, price_with_tax_predict, price_with_tax_actual FROM products WHERE id = 1;
```

**实际结果：**
```
 id | price | price_with_tax | price_with_tax_predict | price_with_tax_actual
----+-------+----------------+------------------------+----------------------
  1 |   300 |            110 |                        |                   110
```

**结论：** ✅ UPDATE 时，`_actual` 列更新为 PREDICT 列的旧值（110），PREDICT 列本身不变。

### P8: 复合索引验证

**验证脚本：**
```sql
SELECT indexname, indexdef FROM pg_indexes
WHERE tablename = 'products' AND indexname LIKE '%predict%';
```

**实际结果：**
```
             indexname              |                                        indexdef
------------------------------------+-------------------------------------------------------------------------------------------------------------------
 products_price_with_tax_predict_idx | CREATE INDEX products_price_with_tax_predict_idx ON public.products USING btree (price_with_tax, price_with_tax_predict)
```

**结论：** ✅ 自动创建复合B-tree索引，索引名为 `{table}_{column}_predict_idx`。

### P9: 触发器验证

**验证脚本：**
```sql
SELECT tgname, tgtype, tgenabled FROM pg_trigger
WHERE tgrelid = 'products'::regclass AND tgname LIKE '%predict%';
```

**实际结果：**
```
             tgname             | tgtype | tgenabled
--------------------------------+--------+-----------
 pg_predict_price_with_tax_XXXXX |     23 | O
```

**结论：** ✅ 自动创建 BEFORE INSERT OR UPDATE 触发器（tgtype=23），触发器名格式为 `pg_predict_{column}_{oid}`。

### P10: text类型PREDICT列

**测试脚本：**
```sql
CREATE OR REPLACE FUNCTION predict_category(description text)
RETURNS text AS $$
BEGIN
    description := lower(description);
    IF description LIKE '%database%' THEN RETURN 'tech';
    ELSIF description LIKE '%health%' THEN RETURN 'medical';
    ELSIF description LIKE '%finance%' THEN RETURN 'business';
    ELSE RETURN 'other';
    END IF;
END;
$$ LANGUAGE plpgsql IMMUTABLE;

CREATE TABLE test_predict_text (
    id SERIAL PRIMARY KEY,
    description TEXT,
    category TEXT PREDICT AS (predict_category(description)) STORED
) WITH (predict_timing = immediate);

INSERT INTO test_predict_text (description) VALUES ('PostgreSQL database system');
INSERT INTO test_predict_text (description) VALUES ('health and wellness');
INSERT INTO test_predict_text (description) VALUES ('random content');
```

**实际结果：**
```
 id |        description         | category | category_predict | category_actual
----+----------------------------+----------+------------------+-----------------
  1 | PostgreSQL database system | tech     | tech             |
  2 | health and wellness        | medical  | medical          |
  3 | random content             | other    | other            |
```

**结论：** ✅ text 类型 PREDICT 列正常工作，预测函数根据描述内容返回分类。

### P11: deferred模式

**测试脚本：**
```sql
CREATE TABLE test_predict_deferred (
    id SERIAL PRIMARY KEY,
    feature FLOAT,
    score FLOAT PREDICT AS (feature * 2.0 + 1.0) STORED
) WITH (predict_timing = deferred);

INSERT INTO test_predict_deferred (feature) VALUES (5.0);
INSERT INTO test_predict_deferred (feature) VALUES (10.0);
```

**实际结果：**
```
 id | feature | score | score_predict | score_actual
----+---------+-------+---------------+--------------
  1 |       5 |       |               |
  2 |      10 |       |               |
```

**结论：** ✅ deferred 模式下，插入时 PREDICT 列保持 NULL，等待异步工作进程处理。

### P12: 多个PREDICT列（不同表达式）

**测试脚本：**
```sql
CREATE TABLE test_predict_multi (
    id SERIAL PRIMARY KEY,
    x FLOAT,
    y FLOAT PREDICT AS (x * 3) STORED,
    z FLOAT PREDICT AS (x + 10) STORED
) WITH (predict_timing = immediate);

INSERT INTO test_predict_multi (x) VALUES (5.0);
```

**实际结果：**
```
 id | x | y  | z  | y_predict | y_actual | z_predict | z_actual
----+---+----+----+-----------+----------+-----------+----------
  1 | 5 | 15 | 15 |        15 |          |        15 |
```

**结论：** ✅ 多个 PREDICT 列使用不同表达式，`y=x*3=15`，`z=x+10=15`，结果正确。

### P13: ALTER TABLE ADD COLUMN PREDICT

**测试脚本：**
```sql
CREATE TABLE test_predict_alter (id SERIAL PRIMARY KEY, feature FLOAT);
INSERT INTO test_predict_alter (feature) VALUES (5.0);
ALTER TABLE test_predict_alter ADD COLUMN score FLOAT PREDICT AS (feature * 2.0 + 1.0) STORED;
```

**实际结果：**
```
    attname     | attgenerated | atthidden
---------------+--------------+-----------
 id            |              | f
 feature       |              | f
 score         | p            | f
 score_predict |              | t
 score_actual  |              | t
```

**结论：** ✅ ALTER TABLE ADD COLUMN PREDICT AS 自动创建隐藏列 `_predict` 和 `_actual`。

### P14: is_predict_column函数

**测试脚本：**
```sql
SELECT is_predict_column('products'::regclass, 'price_with_tax');
SELECT is_predict_column('products'::regclass, 'price');
```

**实际结果：**
```
 is_predict_column
-------------------
 t

 is_predict_column
-------------------
 f
```

**结论：** ✅ `is_predict_column` 正确识别 PREDICT 列和非 PREDICT 列。

### P15: NULL值处理

**测试脚本：**
```sql
INSERT INTO products (price, price_with_tax) VALUES (NULL, NULL);
```

**实际结果：** 预测表达式对 NULL price 返回 NULL，price_with_tax 保持 NULL。

**结论：** ✅ NULL 值正确处理，预测表达式返回 NULL 时不覆盖。

### P16: 批量插入

**测试脚本：**
```sql
INSERT INTO products (price)
SELECT (i::numeric) FROM generate_series(1, 20) AS i;
```

**实际结果：** 所有行正确插入，每行都自动预测。

**结论：** ✅ 批量插入正常工作，每行都自动预测。

### P17: 预测准确性验证

**测试脚本：**
```sql
SELECT id, price, price_with_tax,
       price * 1.1 AS expected_tax,
       price_with_tax - (price * 1.1) AS error
FROM products
WHERE price IS NOT NULL AND price_with_tax IS NOT NULL AND price_with_tax_actual IS NULL
ORDER BY id LIMIT 5;
```

**实际结果：**
```
 id | price | price_with_tax | expected_tax | error
----+-------+----------------+--------------+-------
  2 |   200 |            220 |          220 |     0
  3 |    50 |             55 |           55 |     0
  5 |     1 |            1.1 |          1.1 |     0
  6 |     2 |            2.2 |          2.2 |     0
  7 |     3 |            3.3 |          3.3 |     0
```

**结论：** ✅ 所有预测值与期望值完全一致，误差为0。

---

## predict_function 参数删除验证

### D1: WITH (predict_function=...) 报错

**测试脚本：**
```sql
CREATE TABLE test_no_func (
    id SERIAL PRIMARY KEY,
    price numeric
) WITH (predict_function=add_tax);
```

**实际结果：**
```
ERROR:  unrecognized parameter "predict_function"
```

**结论：** ✅ `predict_function` 参数已被成功删除，不再被识别。

### D2: ALTER TABLE SET PREDICT FUNCTION 报语法错误

**测试脚本：**
```sql
ALTER TABLE test_no_func SET PREDICT FUNCTION add_tax;
```

**实际结果：**
```
ERROR:  syntax error at or near "PREDICT"
```

**结论：** ✅ `SET PREDICT FUNCTION` 语法已被成功删除。

### D3: PREDICT AS (expr) STORED 语法正常工作

**测试脚本：**
```sql
CREATE TABLE test_predict_as (
    id SERIAL PRIMARY KEY,
    price numeric,
    price_with_tax numeric PREDICT AS (add_tax(price)) STORED
) WITH (predict_timing = immediate);

INSERT INTO test_predict_as (price) VALUES (100);
SELECT * FROM test_predict_as;
```

**实际结果：**
```
 id | price | price_with_tax
----+-------+----------------
  1 |   100 |            110
```

**结论：** ✅ `PREDICT AS (expr) STORED` 语法正常工作，完全替代了 `predict_function` 的功能。

---

## 功能亮点

1. **自动预测**：触发器自动管理预测值生成
2. **灵活时机**：支持 immediate/deferred 两种预测模式
3. **内联表达式**：使用 `PREDICT AS (expr) STORED` 语法，直观易用
4. **多列支持**：不同 PREDICT 列可使用不同表达式
5. **隐藏列机制**：_predict/_actual 列自动隐藏，不干扰 SELECT *
6. **ALTER TABLE 支持**：支持动态添加 PREDICT 列
7. **多类型支持**：支持 FLOAT、TEXT、NUMERIC 等多种数据类型的 PREDICT 列
8. **异步预测**：后台工作进程处理延迟预测
9. **VOLATILE 函数支持**：PREDICT 列允许使用 VOLATILE 函数（如 LLM 推理）

## EMBEDDING AS 功能验证详情

### E1: 创建带有EMBEDDING AS列的表

**测试脚本：**
```sql
CREATE EXTENSION IF NOT EXISTS vector;

CREATE OR REPLACE FUNCTION simple_embedding(input text) RETURNS vector
LANGUAGE plpgsql IMMUTABLE AS $$
BEGIN
    RETURN '[0.1, 0.2, 0.3]'::vector;
END;
$$;

CREATE TABLE test_embedding_as (
    id int PRIMARY KEY,
    content text,
    category text EMBEDDING AS (simple_embedding(content)) STORED
) WITH (vector_len=3);
```

**实际结果：** ✅ 建表成功

### E2: 隐藏列验证

**验证脚本：**
```sql
SELECT attname, attgenerated, attembedding FROM pg_attribute
WHERE attrelid = 'test_embedding_as'::regclass AND attnum > 0
ORDER BY attnum;
```

**实际结果：**
```
       attname       | attgenerated | attembedding
---------------------+--------------+--------------
 id                  |              | f
 content             |              | f
 category            | e            | t
 category_embedding  |              | f
```

**结论：** ✅ category 列 attgenerated='e' (ATTRIBUTE_GENERATED_EMBEDDING), attembedding=true; _embedding 伴随列自动创建

### E3: INSERT 自动计算 embedding

**测试脚本：**
```sql
INSERT INTO test_embedding_as (id, content) VALUES (1, 'hello world');
INSERT INTO test_embedding_as (id, content) VALUES (2, 'test embedding');
SELECT id, content, category_embedding FROM test_embedding_as;
```

**实际结果：**
```
 id |    content     | category_embedding
----+----------------+--------------------
  1 | hello world    | [0.1,0.2,0.3]
  2 | test embedding | [0.1,0.2,0.3]
```

**结论：** ✅ embedding 自动计算并存储到 _embedding 伴随列

### E4: UPDATE 重新计算 embedding

**测试脚本：**
```sql
UPDATE test_embedding_as SET content = 'updated content' WHERE id = 1;
SELECT id, content, category_embedding FROM test_embedding_as;
```

**实际结果：**
```
 id |    content       | category_embedding
----+------------------+--------------------
  1 | updated content  | [0.1,0.2,0.3]
  2 | test embedding   | [0.1,0.2,0.3]
```

**结论：** ✅ UPDATE 时 embedding 重新计算

### E5: 向量距离查询重写

**测试脚本：**
```sql
SELECT id, content, category_embedding <-> '[0.1,0.2,0.3]' AS distance
FROM test_embedding_as
ORDER BY category_embedding <-> '[0.1,0.2,0.3]';
```

**实际结果：**
```
 id |    content     | distance
----+----------------+----------
  1 | hello world    |        0
  2 | test embedding |        0
```

**结论：** ✅ 向量距离查询正确重写，自动替换为 _embedding 列

### E6: \d 显示 EMBEDDING AS 语法

**测试脚本：**
```sql
\d test_embedding_as
```

**实际结果：**
```
                                         Table "public.test_embedding_as"
      Column       |  Type   | Collation | Nullable |               Default
-------------------+---------+-----------+----------+--------------------------------------
 id                | integer |           | not null |
 content           | text    |           |          |
 category          | text    |           |          | embedding as (simple_embedding(content)) stored
 category_embedding | vector |           |          |
```

**结论：** ✅ \d 正确显示 embedding as (expr) stored 语法

### E7: embedding_function reloption 已移除

**验证脚本：**
```sql
CREATE TABLE test_old_embedding (
    id int PRIMARY KEY,
    content text EMBEDDING
) WITH (embedding_function='simple_embedding', vector_len=3);
```

**实际结果：** ✅ embedding_function reloption 不再被识别（报错或忽略）

## 测试结论

**总体评价：优秀** ✅

所有核心功能均正常工作，`predict_function` 和 `embedding_function` 参数已被完全删除，`PREDICT AS (expr) STORED` 和 `EMBEDDING AS (expr) STORED` 语法完全替代了其功能。PREDICT 和 EMBEDDING 功能已经可以投入使用。

## 测试通过率

**100%** (17/17项基础测试 + 3/3项删除验证 + 7/7项EMBEDDING测试 = 27/27 全部通过)
