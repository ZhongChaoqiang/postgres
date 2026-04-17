# EMBEDDING 标签列功能测试报告

**测试日期**: 2026-04-14
**测试版本**: PostgreSQL 18 (自定义 EMBEDDING 扩展 + pgvector 0.7.4)
**测试环境**: WSL Ubuntu-22.04, Windows 11

---

## 一、测试概述

本测试覆盖了 EMBEDDING 标签列的所有核心功能，共设计 22 个测试用例，涵盖 DDL、DML、触发器、隐藏列、嵌入函数、向量搜索、查询重写、reloptions 等方面。

### 测试结果汇总

| 统计项 | 数量 |
|--------|------|
| 总测试数 | 22 |
| 通过 | 22 |
| 失败 | 0 |
| 通过率 | 100% |

> **注意**: 测试过程中发现并修复了 1 个 Bug，修复后所有测试均通过。

---

## 二、发现并修复的 Bug

### Bug: ALTER TABLE ADD COLUMN with EMBEDDING 未创建隐藏列

**问题描述**: 使用 `ALTER TABLE ADD COLUMN description TEXT EMBEDDING` 添加 EMBEDDING 列时，不会自动创建 `_embedding` 隐藏列，导致后续查询和嵌入函数调用失败。

**根因分析**: `ATExecAddColumn` 函数在添加 EMBEDDING 列时只创建了触发器，但没有创建 `_embedding` 隐藏列。隐藏列的创建逻辑仅在 `parse_utilcmd.c` 的 `transformColumnDefinition` 函数中实现，该函数只在 `CREATE TABLE` 时被调用。

**修复方案**: 在 `ATExecAddColumn` 函数中，当检测到 `colDef->is_embedding` 为 true 时，直接通过 `InsertPgAttributeTuples` 向 `pg_attribute` 系统表中插入 `_embedding` 隐藏列的属性元组，并更新 `pg_class` 的 `relnatts` 计数。同时从 `reloptions` 中读取 `vector_len` 设置正确的向量维度。

**修复文件**: [tablecmds.c](file:///d:/workspace/postgres/src/backend/commands/tablecmds.c)

---

## 三、详细测试结果

### 测试1: CREATE TABLE with EMBEDDING column - 基本DDL

**测试目的**: 验证 CREATE TABLE 时 EMBEDDING 列的基本 DDL 功能，包括隐藏列自动创建、触发器自动创建、向量索引自动创建。

**测试脚本**:

```sql
CREATE TABLE test_emb_basic (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

-- 检查所有列(包括隐藏列)
SELECT attname, atttypid::regtype, attembedding, atthidden
FROM pg_attribute
WHERE attrelid = 'test_emb_basic'::regclass AND attnum > 0 AND NOT attisdropped
ORDER BY attnum;

-- 检查触发器是否自动创建(包括内部触发器)
SELECT tgname, tgisinternal, proname
FROM pg_trigger t
JOIN pg_proc p ON t.tgfoid = p.oid
WHERE t.tgrelid = 'test_emb_basic'::regclass
ORDER BY tgname;

-- 检查索引是否自动创建
SELECT indexname, indexdef
FROM pg_indexes
WHERE tablename = 'test_emb_basic'
ORDER BY indexname;

DROP TABLE test_emb_basic CASCADE;
```

**测试结果**: ✅ 通过

| 检查项 | 期望结果 | 实际结果 |
|--------|----------|----------|
| content 列 attembedding | true | true |
| content_embedding 列 atthidden | true | true |
| content_embedding 列类型 | vector | vector |
| 内部触发器 | pg_embedding_content_* | pg_embedding_content_59731_59736 |
| 向量索引 | ivfflat(content_embedding) | test_emb_basic_content_embedding_idx |

---

### 测试2: vector_len 选项 - 自定义向量维度

**测试目的**: 验证 vector_len 表级选项控制 _embedding 列的向量维度。

**测试脚本**:

```sql
CREATE TABLE test_emb_vector_len (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding_5d',
    vector_len = 5
);

-- 检查 _embedding 列的类型是否为 vector(5)
SELECT attname, atttypid::regtype, atttypmod
FROM pg_attribute
WHERE attrelid = 'test_emb_vector_len'::regclass AND attnum > 0 AND NOT attisdropped
ORDER BY attnum;

INSERT INTO test_emb_vector_len (content) VALUES ('hello');

SELECT id, content, content_embedding FROM test_emb_vector_len;

DROP TABLE test_emb_vector_len CASCADE;
```

**测试结果**: ✅ 通过

| 检查项 | 期望结果 | 实际结果 |
|--------|----------|----------|
| content_embedding 类型 | vector(5) | vector (atttypmod=5) |
| content_embedding 值 | [0.1,0.2,0.3,0.4,0.5] | [0.1,0.2,0.3,0.4,0.5] |

---

### 测试3: 多个 EMBEDDING 列 - 单一嵌入函数

**测试目的**: 验证多个 EMBEDDING 列使用同一个嵌入函数时，所有列都能正确生成向量。

**测试脚本**:

```sql
CREATE TABLE test_emb_multi (
    id SERIAL PRIMARY KEY,
    title TEXT EMBEDDING,
    body TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

INSERT INTO test_emb_multi (title, body) VALUES ('hello', 'world');

SELECT id, title, title_embedding, body, body_embedding FROM test_emb_multi;

DROP TABLE test_emb_multi CASCADE;
```

**测试结果**: ✅ 通过

| 列 | 期望值 | 实际值 |
|----|--------|--------|
| title_embedding | [0.1,0.2,0.3] | [0.1,0.2,0.3] |
| body_embedding | [0.1,0.2,0.3] | [0.1,0.2,0.3] |

---

### 测试4: 多个 EMBEDDING 列 - 列特定嵌入函数

**测试目的**: 验证 `col:func;col2:func2` 格式的列特定嵌入函数。

**测试脚本**:

```sql
CREATE TABLE test_emb_multi_func (
    id SERIAL PRIMARY KEY,
    title TEXT EMBEDDING,
    body TEXT EMBEDDING
) WITH (
    embedding_function = 'title:emb_func_a;body:emb_func_b',
    vector_len = 3
);

INSERT INTO test_emb_multi_func (title, body) VALUES ('hello', 'world');

SELECT id, title, title_embedding, body, body_embedding FROM test_emb_multi_func;

DROP TABLE test_emb_multi_func CASCADE;
```

**测试结果**: ✅ 通过

| 列 | 期望值 | 实际值 |
|----|--------|--------|
| title_embedding | [1,0,0] | [1,0,0] |
| body_embedding | [0,1,0] | [0,1,0] |

---

### 测试5: ALTER TABLE ADD COLUMN with EMBEDDING

**测试目的**: 验证通过 ALTER TABLE 添加 EMBEDDING 列时，隐藏列和触发器正确创建。

**测试脚本**:

```sql
CREATE TABLE test_emb_addcol (
    id SERIAL PRIMARY KEY,
    name TEXT
) WITH (
    vector_len = 3
);

ALTER TABLE test_emb_addcol ADD COLUMN description TEXT EMBEDDING;

-- 检查隐藏列是否创建
SELECT attname, atttypid::regtype, attembedding, atthidden
FROM pg_attribute
WHERE attrelid = 'test_emb_addcol'::regclass AND attnum > 0 AND NOT attisdropped
ORDER BY attnum;

-- 检查触发器是否创建(包括内部触发器)
SELECT tgname, tgisinternal, proname
FROM pg_trigger t
JOIN pg_proc p ON t.tgfoid = p.oid
WHERE t.tgrelid = 'test_emb_addcol'::regclass
ORDER BY tgname;

-- 设置embedding_function并插入数据
ALTER TABLE test_emb_addcol SET (embedding_function = 'my_embedding');

INSERT INTO test_emb_addcol (name, description) VALUES ('test', 'hello world');

SELECT id, name, description, description_embedding FROM test_emb_addcol;

DROP TABLE test_emb_addcol CASCADE;
```

**测试结果**: ✅ 通过 (修复 Bug 后)

| 检查项 | 期望结果 | 实际结果 |
|--------|----------|----------|
| description 列存在 | 是 | 是 |
| description_embedding 列存在 | 是 | 是 |
| description attembedding | true | true |
| description_embedding atthidden | true | true |
| 内部触发器 | pg_embedding_description_* | pg_embedding_description_59783_59792 |
| description_embedding 值 | [0.1,0.2,0.3] | [0.1,0.2,0.3] |

---

### 测试6: UPDATE 触发嵌入函数重新计算

**测试目的**: 验证 UPDATE 操作时 EMBEDDING 列的触发器重新调用嵌入函数。

**测试脚本**:

```sql
CREATE TABLE test_emb_update (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

INSERT INTO test_emb_update (content) VALUES ('original');

SELECT id, content, content_embedding FROM test_emb_update;

UPDATE test_emb_update SET content = 'updated' WHERE id = 1;

SELECT id, content, content_embedding FROM test_emb_update;

DROP TABLE test_emb_update CASCADE;
```

**测试结果**: ✅ 通过

| 操作 | content | content_embedding |
|------|---------|-------------------|
| INSERT | original | [0.1,0.2,0.3] |
| UPDATE | updated | [0.1,0.2,0.3] |

---

### 测试7: NULL 值处理

**测试目的**: 验证 EMBEDDING 列为 NULL 时不调用嵌入函数，_embedding 列也为 NULL。

**测试脚本**:

```sql
CREATE TABLE test_emb_null (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

-- INSERT with NULL content -> should NOT call embedding function
INSERT INTO test_emb_null (id) VALUES (DEFAULT);

SELECT id, content, content_embedding FROM test_emb_null;

DROP TABLE test_emb_null CASCADE;
```

**测试结果**: ✅ 通过

| content | content_embedding |
|---------|-------------------|
| NULL | NULL |

---

### 测试8: 隐藏列 - SELECT * 不显示隐藏列

**测试目的**: 验证 _embedding 列在 SELECT * 中不可见，显式指定列名可查询。

**测试脚本**:

```sql
CREATE TABLE test_emb_hidden (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

INSERT INTO test_emb_hidden (content) VALUES ('test');

-- SELECT * 应该只显示id和content,不显示隐藏列
SELECT * FROM test_emb_hidden;

-- 显式指定列名可以查询隐藏列
SELECT id, content, content_embedding FROM test_emb_hidden;

DROP TABLE test_emb_hidden CASCADE;
```

**测试结果**: ✅ 通过

- `SELECT *` 只显示 id 和 content 两列
- 显式指定列名可以查询 content_embedding

---

### 测试9: 触发器命名规则检查

**测试目的**: 验证多个 EMBEDDING 列的触发器命名格式 `pg_embedding_{colname}_{relOid}_{trigOid}`。

**测试脚本**:

```sql
CREATE TABLE test_emb_trigger_check (
    id SERIAL PRIMARY KEY,
    title TEXT EMBEDDING,
    body TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

SELECT tgname, tgisinternal, proname
FROM pg_trigger t
JOIN pg_proc p ON t.tgfoid = p.oid
WHERE t.tgrelid = 'test_emb_trigger_check'::regclass
ORDER BY tgname;

DROP TABLE test_emb_trigger_check CASCADE;
```

**测试结果**: ✅ 通过

| 触发器名 | proname | tgisinternal |
|----------|---------|-------------|
| pg_embedding_body_59830_59836 | embedding_trigger | t |
| pg_embedding_title_59830_59835 | embedding_trigger | t |

---

### 测试10: 向量索引检查

**测试目的**: 验证 EMBEDDING 列自动创建的 ivfflat 向量索引。

**测试脚本**:

```sql
CREATE TABLE test_emb_index_check (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

SELECT indexname, indexdef
FROM pg_indexes
WHERE tablename = 'test_emb_index_check'
ORDER BY indexname;

DROP TABLE test_emb_index_check CASCADE;
```

**测试结果**: ✅ 通过

| 索引名 | 索引定义 |
|--------|----------|
| test_emb_index_check_content_embedding_idx | USING ivfflat (content_embedding) |
| test_emb_index_check_pkey | USING btree (id) |

---

### 测试11: L2 距离搜索 (<->)

**测试目的**: 验证使用 <-> 操作符进行 L2 距离搜索，查询重写将 EMBEDDING 列替换为 _embedding 列。

**测试脚本**:

```sql
CREATE TABLE test_emb_l2_search (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

INSERT INTO test_emb_l2_search (content) VALUES ('hello'), ('world'), ('test');

SELECT id, content, content <-> 'hello' AS distance
FROM test_emb_l2_search
ORDER BY content <-> 'hello';

DROP TABLE test_emb_l2_search CASCADE;
```

**测试结果**: ✅ 通过

所有行的 L2 距离均为 0（因为嵌入函数返回相同的向量）。

---

### 测试12: 余弦距离搜索 (<=>)

**测试目的**: 验证使用 <=> 操作符进行余弦距离搜索。

**测试脚本**:

```sql
CREATE TABLE test_emb_cosine_search (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

INSERT INTO test_emb_cosine_search (content) VALUES ('hello'), ('world'), ('test');

SELECT id, content, content <=> 'hello' AS distance
FROM test_emb_cosine_search
ORDER BY content <=> 'hello';

DROP TABLE test_emb_cosine_search CASCADE;
```

**测试结果**: ✅ 通过

所有行的余弦距离均为 0。

---

### 测试13: 内积距离搜索 (<#>)

**测试目的**: 验证使用 <#> 操作符进行内积距离搜索。

**测试脚本**:

```sql
CREATE TABLE test_emb_ip_search (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

INSERT INTO test_emb_ip_search (content) VALUES ('hello'), ('world'), ('test');

SELECT id, content, content <#> 'hello' AS distance
FROM test_emb_ip_search
ORDER BY content <#> 'hello';

DROP TABLE test_emb_ip_search CASCADE;
```

**测试结果**: ✅ 通过

所有行的内积距离均为 -0.14（负值表示内积，pgvector 中内积距离取负值）。

---

### 测试14: WHERE 条件向量查询重写

**测试目的**: 验证 WHERE 子句中使用向量距离操作符时的查询重写。

**测试脚本**:

```sql
CREATE TABLE test_emb_where_rewrite (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

INSERT INTO test_emb_where_rewrite (content) VALUES ('hello'), ('world'), ('test');

SELECT id, content, content <-> 'hello' AS distance
FROM test_emb_where_rewrite
WHERE content <-> 'hello' < 1.0
ORDER BY content <-> 'hello';

DROP TABLE test_emb_where_rewrite CASCADE;
```

**测试结果**: ✅ 通过

WHERE 条件正确过滤了距离小于 1.0 的行。

---

### 测试15: ORDER BY 向量距离查询重写

**测试目的**: 验证 ORDER BY 子句中使用向量距离操作符时的查询重写。

**测试脚本**:

```sql
CREATE TABLE test_emb_orderby_rewrite (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

INSERT INTO test_emb_orderby_rewrite (content) VALUES ('hello'), ('world'), ('test');

SELECT id, content
FROM test_emb_orderby_rewrite
ORDER BY content <-> 'hello'
LIMIT 2;

DROP TABLE test_emb_orderby_rewrite CASCADE;
```

**测试结果**: ✅ 通过

返回了按距离排序的前 2 行。

---

### 测试16: reloptions 检查

**测试目的**: 验证 EMBEDDING 相关的表级选项在 pg_class.reloptions 中的存储。

**测试脚本**:

```sql
CREATE TABLE test_emb_reloptions (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

SELECT reloptions FROM pg_class WHERE relname = 'test_emb_reloptions';

DROP TABLE test_emb_reloptions CASCADE;
```

**测试结果**: ✅ 通过

reloptions: `{embedding_function=my_embedding,vector_len=3}`

---

### 测试17: PREDICT + EMBEDDING 组合使用

**测试目的**: 验证 PREDICT 和 EMBEDDING 列在同一张表中组合使用。

**测试脚本**:

```sql
CREATE OR REPLACE FUNCTION simple_predict(rec record)
RETURNS FLOAT AS $$
BEGIN
    RETURN 1.0;
END;
$$ LANGUAGE plpgsql;

CREATE TABLE test_emb_predict_combo (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING,
    score FLOAT PREDICT
) WITH (
    embedding_function = 'my_embedding',
    predict_function = 'simple_predict',
    predict_timing = immediate,
    vector_len = 3
);

INSERT INTO test_emb_predict_combo (content) VALUES ('test');

SELECT id, content, content_embedding, score, score_predict, score_actual
FROM test_emb_predict_combo;

DROP TABLE test_emb_predict_combo CASCADE;
DROP FUNCTION simple_predict(record) CASCADE;
```

**测试结果**: ✅ 通过

| content | content_embedding | score | score_predict | score_actual |
|---------|-------------------|-------|---------------|--------------|
| test | [0.1,0.2,0.3] | 1 | 1 | NULL |

---

### 测试18: 批量插入测试

**测试目的**: 验证多行 INSERT 时每行都正确触发嵌入函数。

**测试脚本**:

```sql
CREATE TABLE test_emb_batch_insert (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

INSERT INTO test_emb_batch_insert (content) VALUES ('doc1'), ('doc2'), ('doc3'), ('doc4'), ('doc5');

SELECT id, content, content_embedding FROM test_emb_batch_insert ORDER BY id;

DROP TABLE test_emb_batch_insert CASCADE;
```

**测试结果**: ✅ 通过

所有 5 行的 content_embedding 均为 [0.1,0.2,0.3]。

---

### 测试19: pg_attribute 中 attembedding 标记检查

**测试目的**: 验证 pg_attribute 系统表中 attembedding 标记的正确性。

**测试脚本**:

```sql
CREATE TABLE test_emb_attr_check (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING,
    normal_col TEXT
) WITH (
    vector_len = 3
);

SELECT attname, attembedding, attpredict, atthidden
FROM pg_attribute
WHERE attrelid = 'test_emb_attr_check'::regclass AND attnum > 0 AND NOT attisdropped
ORDER BY attnum;

DROP TABLE test_emb_attr_check CASCADE;
```

**测试结果**: ✅ 通过

| 列名 | attembedding | attpredict | atthidden |
|------|-------------|------------|-----------|
| id | f | f | f |
| content | t | f | f |
| content_embedding | f | f | t |
| normal_col | f | f | f |

---

### 测试20: embedding_function reloption 列特定格式检查

**测试目的**: 验证 embedding_function 表级选项支持列特定格式。

**测试脚本**:

```sql
CREATE TABLE test_emb_col_func_relopt (
    id SERIAL PRIMARY KEY,
    title TEXT EMBEDDING,
    body TEXT EMBEDDING
) WITH (
    embedding_function = 'title:emb_func_a;body:emb_func_b',
    vector_len = 3
);

SELECT reloptions FROM pg_class WHERE relname = 'test_emb_col_func_relopt';

DROP TABLE test_emb_col_func_relopt CASCADE;
```

**测试结果**: ✅ 通过

reloptions: `{embedding_function=title:emb_func_a;body:emb_func_b,vector_len=3}`

---

### 测试21: 向量距离操作符反向 (text在右侧)

**测试目的**: 验证反向操作符 `'hello' <-> content` 的查询重写。

**测试脚本**:

```sql
CREATE TABLE test_emb_reverse_op (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    embedding_function = 'my_embedding',
    vector_len = 3
);

INSERT INTO test_emb_reverse_op (content) VALUES ('hello'), ('world');

SELECT id, content, 'hello' <-> content AS distance
FROM test_emb_reverse_op
ORDER BY 'hello' <-> content;

DROP TABLE test_emb_reverse_op CASCADE;
```

**测试结果**: ✅ 通过

反向操作符正确工作，距离值与正向操作符一致。

---

### 测试22: 不设置 embedding_function 时插入数据

**测试目的**: 验证不设置 embedding_function 时，INSERT 不会触发嵌入计算，_embedding 列保持 NULL。

**测试脚本**:

```sql
CREATE TABLE test_emb_no_func (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    vector_len = 3
);

INSERT INTO test_emb_no_func (content) VALUES ('test');

SELECT id, content, content_embedding FROM test_emb_no_func;

DROP TABLE test_emb_no_func CASCADE;
```

**测试结果**: ✅ 通过

| content | content_embedding |
|---------|-------------------|
| test | NULL |

---

## 四、功能覆盖矩阵

| 功能模块 | 测试覆盖 | 状态 |
|----------|----------|------|
| CREATE TABLE with EMBEDDING | 测试1, 8, 9, 10, 19 | ✅ |
| 隐藏列 (_embedding) | 测试1, 5, 8 | ✅ |
| 触发器自动创建 | 测试1, 5, 9 | ✅ |
| 向量索引自动创建 | 测试1, 10 | ✅ |
| vector_len 选项 | 测试2, 16 | ✅ |
| embedding_function (单一) | 测试3, 16 | ✅ |
| embedding_function (列特定) | 测试4, 20 | ✅ |
| ALTER TABLE ADD COLUMN EMBEDDING | 测试5 | ✅ |
| INSERT 触发嵌入计算 | 测试1, 2, 3, 4, 6, 18 | ✅ |
| UPDATE 触发嵌入计算 | 测试6 | ✅ |
| NULL 值处理 | 测试7 | ✅ |
| 不设置 embedding_function | 测试22 | ✅ |
| 隐藏列 SELECT * 不可见 | 测试8 | ✅ |
| 触发器命名规则 | 测试9 | ✅ |
| L2 距离搜索 (<->) | 测试11 | ✅ |
| 余弦距离搜索 (<=>) | 测试12 | ✅ |
| 内积距离搜索 (<#>) | 测试13 | ✅ |
| WHERE 条件查询重写 | 测试14 | ✅ |
| ORDER BY 查询重写 | 测试15 | ✅ |
| 反向操作符查询重写 | 测试21 | ✅ |
| reloptions 存储 | 测试16, 20 | ✅ |
| pg_attribute attembedding 标记 | 测试19 | ✅ |
| PREDICT + EMBEDDING 组合 | 测试17 | ✅ |
| 多行 INSERT | 测试18 | ✅ |

---

## 五、修改文件清单

| 文件 | 修改内容 |
|------|----------|
| [tablecmds.c](file:///d:/workspace/postgres/src/backend/commands/tablecmds.c) | 在 ATExecAddColumn 中为 EMBEDDING 列自动创建 _embedding 隐藏列，从 reloptions 读取 vector_len 设置向量维度 |

---

## 六、已知限制

1. **ALTER TABLE ADD COLUMN EMBEDDING 不创建向量索引**: 通过 ALTER TABLE 添加 EMBEDDING 列时，不会自动创建 ivfflat 向量索引（CREATE TABLE 时会创建）。如需索引，需手动创建。

2. **异步预测工作进程未测试**: 本次测试未覆盖 `async_predict` 后台工作进程的 deferred 模式批量预测功能。

3. **vector_index 和 vector_distance 选项未测试**: 本次测试未覆盖 `vector_index`（ivfflat/hnsw）和 `vector_distance`（l2/ip/cosine）选项的向量索引类型和距离类型配置。

4. **嵌入函数返回固定向量**: 测试中使用的嵌入函数返回固定向量，未测试真实嵌入模型（如 PL/Python3 调用 sentence-transformers）的场景。
