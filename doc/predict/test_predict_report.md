# PREDICT 标签列功能测试报告

**测试日期**: 2026-04-14
**测试版本**: PostgreSQL 18 (自定义 PREDICT 扩展)
**测试环境**: WSL Ubuntu-22.04, Windows 11

---

## 一、测试概述

本测试覆盖了 PREDICT 标签列的所有核心功能，共设计 19 个测试用例，涵盖 DDL、DML、触发器、隐藏列、预测函数、reloptions 等方面。

### 测试结果汇总

| 统计项 | 数量 |
|--------|------|
| 总测试数 | 19 |
| 通过 | 19 |
| 失败 | 0 |
| 通过率 | 100% |

> **注意**: 测试过程中发现并修复了 2 个 Bug，修复后所有测试均通过。

---

## 二、发现并修复的 Bug

### Bug 1: ALTER TABLE ADD COLUMN with PREDICT 未创建隐藏列

**问题描述**: 使用 `ALTER TABLE ADD COLUMN score FLOAT PREDICT` 添加 PREDICT 列时，不会自动创建 `_predict` 和 `_actual` 隐藏列，也不会创建触发器。

**根因分析**: `ATExecAddColumn` 函数在添加 PREDICT 列时只创建了触发器，但没有创建隐藏列（`_predict` 和 `_actual`）。隐藏列的创建逻辑仅在 `parse_utilcmd.c` 的 `transformColumnDefinition` 函数中实现，该函数只在 `CREATE TABLE` 时被调用。

**修复方案**: 在 `ATExecAddColumn` 函数中，当检测到 `colDef->is_predict` 为 true 时，直接通过 `InsertPgAttributeTuples` 向 `pg_attribute` 系统表中插入隐藏列的属性元组，并更新 `pg_class` 的 `relnatts` 计数。

**修复文件**: [tablecmds.c](file:///d:/workspace/postgres/src/backend/commands/tablecmds.c)

### Bug 2: predict_trigger UPDATE 操作使用错误的 tuple

**问题描述**: `predict_trigger` 触发器在 UPDATE 操作中使用 `tg_trigtuple`（旧元组）而非 `tg_newtuple`（新元组），导致：
1. 读取的是旧值而非用户输入的新值
2. 修改的是旧元组而非新元组
3. UPDATE 设置 PREDICT 列为 NULL 时无法正确触发预测

**根因分析**: PostgreSQL 中 BEFORE UPDATE 触发器的 `tg_trigtuple` 是旧元组，`tg_newtuple` 是新元组。原代码对所有操作统一使用 `tg_trigtuple`，在 INSERT 时正确（新元组），但在 UPDATE 时错误（应为新元组）。

**修复方案**: 在 `predict_trigger` 和 `embedding_trigger` 函数中，根据触发事件类型选择正确的 tuple：
- INSERT 操作: 使用 `tg_trigtuple`（新元组）
- UPDATE 操作: 使用 `tg_newtuple`（新元组）

**修复文件**: [predict.c](file:///d:/workspace/postgres/src/backend/utils/adt/predict.c)

---

## 三、详细测试结果

### 测试1: CREATE TABLE with PREDICT column - 基本DDL

**测试目的**: 验证 CREATE TABLE 时 PREDICT 列的基本 DDL 功能

**测试内容**:
- 创建包含 PREDICT 列的表
- 检查隐藏列 `_predict` 和 `_actual` 是否自动创建
- 检查 `attpredict` 和 `atthidden` 属性标记
- 检查内部触发器是否自动创建
- 检查复合索引是否自动创建

**测试结果**: ✅ 通过

**验证要点**:
| 检查项 | 期望结果 | 实际结果 |
|--------|----------|----------|
| PREDICT 列 attpredict | true | true |
| _predict 列 atthidden | true | true |
| _actual 列 atthidden | true | true |
| 内部触发器 | pg_predict_value_{oid} | pg_predict_value_51479_51484 |
| 复合索引 | {table}_{col}_predict_idx | test_predict_basic_value_predict_idx |

---

### 测试2: immediate 模式 - INSERT 触发预测

**测试目的**: 验证 predict_timing=immediate 时，INSERT NULL 值触发预测

**测试SQL**:
```sql
CREATE TABLE test (id SERIAL PRIMARY KEY, value FLOAT PREDICT)
    WITH (predict_timing = immediate, predict_function = 'simple_predict');
INSERT INTO test (id) VALUES (DEFAULT);
```

**测试结果**: ✅ 通过

| 列 | 期望值 | 实际值 |
|----|--------|--------|
| value | 100.0 | 100 |
| value_predict | 100.0 | 100 |
| value_actual | NULL | NULL |

---

### 测试3: deferred 模式 - INSERT 不触发预测

**测试目的**: 验证 predict_timing=deferred 时，INSERT NULL 值不触发预测

**测试结果**: ✅ 通过

| 列 | 期望值 | 实际值 |
|----|--------|--------|
| value | NULL | NULL |
| value_predict | NULL | NULL |
| value_actual | NULL | NULL |

---

### 测试4: 多个 PREDICT 列 - 单一预测函数

**测试目的**: 验证多个 PREDICT 列使用同一个预测函数

**测试SQL**:
```sql
CREATE TABLE test (id SERIAL PRIMARY KEY, temp FLOAT PREDICT, humidity FLOAT PREDICT)
    WITH (predict_timing = immediate, predict_function = 'simple_predict');
```

**测试结果**: ✅ 通过

| 列 | 期望值 | 实际值 |
|----|--------|--------|
| temp | 100.0 | 100 |
| temp_predict | 100.0 | 100 |
| humidity | 100.0 | 100 |
| humidity_predict | 100.0 | 100 |

---

### 测试5: 多个 PREDICT 列 - 列特定预测函数

**测试目的**: 验证 `col:func;col2:func2` 格式的列特定预测函数

**测试SQL**:
```sql
CREATE TABLE test (id SERIAL PRIMARY KEY, temp FLOAT PREDICT, humidity FLOAT PREDICT, pressure FLOAT PREDICT)
    WITH (predict_timing = immediate, predict_function = 'temp:temp_predict_func;humidity:humidity_predict_func');
```

**测试结果**: ✅ 通过

| 列 | 期望值 | 实际值 |
|----|--------|--------|
| temp | 26.775 | 26.775 |
| humidity | 57.0 | 57 |
| pressure | NULL | NULL |

---

### 测试6: ALTER TABLE SET PREDICT FUNCTION

**测试目的**: 验证通过 ALTER TABLE 动态设置预测函数

**测试SQL**:
```sql
CREATE TABLE test (id SERIAL PRIMARY KEY, value FLOAT PREDICT) WITH (predict_timing = immediate);
INSERT INTO test (id) VALUES (DEFAULT);  -- 无预测函数，value=NULL
ALTER TABLE test SET PREDICT FUNCTION simple_predict;
INSERT INTO test (id) VALUES (DEFAULT);  -- 有预测函数，value=100
```

**测试结果**: ✅ 通过

| 行 | 期望 value | 实际 value |
|----|-----------|-----------|
| 第1行 (无函数) | NULL | NULL |
| 第2行 (有函数) | 100.0 | 100 |

---

### 测试7: ALTER TABLE ADD COLUMN with PREDICT

**测试目的**: 验证通过 ALTER TABLE 添加 PREDICT 列

**测试SQL**:
```sql
CREATE TABLE test (id SERIAL PRIMARY KEY, name TEXT);
ALTER TABLE test ADD COLUMN score FLOAT PREDICT;
ALTER TABLE test SET PREDICT FUNCTION simple_predict;
ALTER TABLE test SET (predict_timing = immediate);
INSERT INTO test (name) VALUES ('test');
```

**测试结果**: ✅ 通过 (修复 Bug 1 后)

| 检查项 | 期望结果 | 实际结果 |
|--------|----------|----------|
| score 列存在 | 是 | 是 |
| score_predict 列存在 | 是 | 是 |
| score_actual 列存在 | 是 | 是 |
| score attpredict | true | true |
| 内部触发器 | pg_predict_score_* | pg_predict_score_51552_51561 |
| 预测结果 score | 42.0 | 42 |

---

### 测试8: UPDATE 触发预测

**测试目的**: 验证 UPDATE 操作时 PREDICT 列的触发行为

**测试SQL**:
```sql
INSERT INTO test (id) VALUES (DEFAULT);  -- value=100
UPDATE test SET value = NULL WHERE id = 1;  -- 重新触发预测
```

**测试结果**: ✅ 通过 (修复 Bug 2 后)

| 操作 | value | value_predict | value_actual |
|------|-------|---------------|--------------|
| INSERT (NULL) | 100 | 100 | NULL |
| UPDATE (NULL) | 100 | 100 | NULL |

**说明**: UPDATE 将 value 设为 NULL 后，触发器重新调用预测函数，value 和 value_predict 均更新为预测值 100。value_actual 为 NULL 表示用户输入的是 NULL。

---

### 测试9: INSERT with non-NULL PREDICT column value

**测试目的**: 验证 INSERT 显式值时不触发预测，将值存入 _actual

**测试SQL**:
```sql
INSERT INTO test (value) VALUES (50.0);
```

**测试结果**: ✅ 通过

| 列 | 期望值 | 实际值 |
|----|--------|--------|
| value | 50.0 | 50 |
| value_predict | NULL | NULL |
| value_actual | 50.0 | 50 |

---

### 测试10: is_predict_column() 函数

**测试目的**: 验证 is_predict_column() 系统函数

**测试结果**: ✅ 通过

| 测试场景 | 期望结果 | 实际结果 |
|----------|----------|----------|
| PREDICT 列 | true | t |
| 普通列 | false | f |
| 不存在的列 | false | f |

---

### 测试11: 隐藏列 - SELECT * 不显示隐藏列

**测试目的**: 验证 _predict 和 _actual 列在 SELECT * 中不可见

**测试结果**: ✅ 通过

- `SELECT *` 只显示 id 和 value
- 显式指定列名可以查询 value_predict 和 value_actual

---

### 测试12: 触发器命名规则检查

**测试目的**: 验证多个 PREDICT 列的触发器命名

**测试结果**: ✅ 通过

| 触发器名 | 格式 |
|----------|------|
| pg_predict_temperature_51606_51611 | pg_predict_{colname}_{relOid}_{trigOid} |
| pg_predict_humidity_51606_51612 | pg_predict_{colname}_{relOid}_{trigOid} |

---

### 测试13: 复合索引检查

**测试目的**: 验证自动创建的复合索引

**测试结果**: ✅ 通过

| 索引名 | 索引定义 |
|--------|----------|
| test_predict_index_check_value_predict_idx | btree (value, value_predict) |

---

### 测试14: 预测函数使用行中的特征列

**测试目的**: 验证预测函数可以访问行中的其他列

**测试SQL**:
```sql
CREATE FUNCTION predict_with_features(rec record) RETURNS FLOAT AS $$
BEGIN RETURN rec.feature1 * 1.05 + rec.feature2 * 0.5; END;
$$ LANGUAGE plpgsql;
INSERT INTO test (feature1, feature2) VALUES (10.0, 20.0);
```

**测试结果**: ✅ 通过

| feature1 | feature2 | 期望 prediction | 实际 prediction |
|----------|----------|-----------------|-----------------|
| 10.0 | 20.0 | 20.5 | 20.5 |
| 5.0 | 15.0 | 12.75 | 12.75 |

---

### 测试15: 多行插入测试

**测试目的**: 验证多行 INSERT 时每行都触发预测

**测试结果**: ✅ 通过

所有 3 行的 value 和 value_predict 均为 100。

---

### 测试16: predict_timing reloption 检查

**测试目的**: 验证 predict_timing 表级选项的存储

**测试结果**: ✅ 通过

| 模式 | reloptions |
|------|-----------|
| deferred | {predict_timing=deferred} |
| immediate | {predict_timing=immediate} |

---

### 测试17: predict_function reloption 检查

**测试目的**: 验证 predict_function 表级选项的存储

**测试结果**: ✅ 通过

reloptions: `{predict_timing=immediate,predict_function=simple_predict}`

---

### 测试18: pg_attribute 中 attpredict 标记检查

**测试目的**: 验证 pg_attribute 系统表中 attpredict 标记的正确性

**测试结果**: ✅ 通过

| 列名 | attpredict |
|------|-----------|
| id | f |
| predict_val | t |
| predict_val_predict | f |
| predict_val_actual | f |
| normal_val | f |

---

### 测试19: 预测函数返回不同类型

**测试目的**: 验证预测函数返回 FLOAT 类型值

**测试结果**: ✅ 通过

| score | score_predict |
|-------|---------------|
| 3.14159 | 3.14159 |

---

## 四、功能覆盖矩阵

| 功能模块 | 测试覆盖 | 状态 |
|----------|----------|------|
| CREATE TABLE with PREDICT | 测试1, 11, 12, 13, 18 | ✅ |
| 隐藏列 (_predict, _actual) | 测试1, 7, 11 | ✅ |
| 触发器自动创建 | 测试1, 7, 12 | ✅ |
| 复合索引自动创建 | 测试1, 13 | ✅ |
| predict_timing=immediate | 测试2, 4, 5, 8, 14, 15 | ✅ |
| predict_timing=deferred | 测试3 | ✅ |
| predict_function (单一) | 测试2, 4, 6 | ✅ |
| predict_function (列特定) | 测试5 | ✅ |
| ALTER TABLE SET PREDICT FUNCTION | 测试6 | ✅ |
| ALTER TABLE ADD COLUMN PREDICT | 测试7 | ✅ |
| INSERT 触发预测 | 测试2, 4, 5, 14, 15 | ✅ |
| UPDATE 触发预测 | 测试8 | ✅ |
| INSERT 显式值 (不触发预测) | 测试9 | ✅ |
| is_predict_column() 函数 | 测试10 | ✅ |
| 隐藏列 SELECT * 不可见 | 测试11 | ✅ |
| 触发器命名规则 | 测试12 | ✅ |
| 预测函数访问行特征列 | 测试14 | ✅ |
| 多行 INSERT | 测试15 | ✅ |
| reloptions 存储 | 测试16, 17 | ✅ |
| pg_attribute attpredict 标记 | 测试18 | ✅ |
| 不同返回类型预测函数 | 测试19 | ✅ |

---

## 五、修改文件清单

| 文件 | 修改内容 |
|------|----------|
| [predict.c](file:///d:/workspace/postgres/src/backend/utils/adt/predict.c) | 修复 predict_trigger 和 embedding_trigger 在 UPDATE 操作中使用 tg_newtuple 替代 tg_trigtuple |
| [tablecmds.c](file:///d:/workspace/postgres/src/backend/commands/tablecmds.c) | 在 ATExecAddColumn 中为 PREDICT 列自动创建 _predict 和 _actual 隐藏列 |

---

## 六、已知限制

1. **ALTER TABLE ADD COLUMN PREDICT 不创建复合索引**: 通过 ALTER TABLE 添加 PREDICT 列时，不会自动创建复合索引（CREATE TABLE 时会创建）。如需索引，需手动创建。

2. **异步预测工作进程未测试**: 本次测试未覆盖 `async_predict` 后台工作进程的 deferred 模式批量预测功能。

3. **EMBEDDING 列功能未测试**: 本次测试专注于 PREDICT 标签列，EMBEDDING 列属性的相关功能未包含在本次测试范围内。
