# PostgreSQL EMBEDDING 功能测试报告

## 测试执行时间
执行日期：2026-04-17

## 测试环境
- PostgreSQL 版本：自定义版本（带EMBEDDING功能）
- 测试数据库：默认数据库
- 操作系统：WSL Ubuntu

## 测试结果汇总

### ✅ 基础功能测试（15/15 通过）

| 测试项 | 测试内容 | 结果 |
|--------|---------|------|
| 测试1 | 创建embedding函数 | ✓ 通过 |
| 测试2 | 创建带有EMBEDDING列的表 | ✓ 通过 |
| 测试3 | 插入数据（触发器自动生成向量） | ✓ 通过 |
| 测试4 | 更新数据（触发器自动更新向量） | ✓ 通过 |
| 测试5 | <-> 操作符查询（L2距离） | ✓ 通过 |
| 测试6 | <=> 操作符查询（余弦距离） | ✓ 通过 |
| 测试7 | <#> 操作符查询（内积） | ✓ 通过 |
| 测试8 | NULL值处理 | ✓ 通过 |
| 测试9 | 多个embedding列 | ✓ 通过 |
| 测试10 | 向量索引 | ✓ 通过 |
| 测试11 | 复杂查询 | ✓ 通过 |
| 测试12 | 性能测试（批量插入） | ✓ 通过 |
| 测试13 | 错误处理 | ✓ 通过 |
| 测试14 | 查询重写验证 | ✓ 通过 |
| 测试15 | 清理测试数据 | ✓ 通过 |

### ✅ 向量索引自动创建测试（9/9 通过）

| 测试项 | 测试内容 | 结果 |
|--------|---------|------|
| V1 | 默认索引（ivfflat + vector_l2_ops） | ✓ 通过 |
| V2 | 显式指定 ivfflat + vector_l2_ops | ✓ 通过 |
| V3 | ivfflat + vector_cosine_ops | ✓ 通过 |
| V4 | ivfflat + vector_ip_ops | ✓ 通过 |
| V5 | hnsw + vector_l2_ops | ✓ 通过 |
| V6 | hnsw + vector_cosine_ops | ✓ 通过 |
| V7 | hnsw + vector_ip_ops | ✓ 通过 |
| V8 | 多个EMBEDDING列各自创建索引 | ✓ 通过 |
| V9 | reloptions存储验证 | ✓ 通过 |

### ✅ 向量索引参数传递测试（7/7 通过）

| 测试项 | 测试内容 | 结果 |
|--------|---------|------|
| P1 | 默认参数（无lists/m/ef_construction） | ✓ 通过 |
| P2 | ivfflat + lists=100 | ✓ 通过 |
| P3 | hnsw + m=16 | ✓ 通过 |
| P4 | hnsw + m=16 + ef_construction=64 | ✓ 通过 |
| P5 | ivfflat + lists=50 + vector_cosine_ops | ✓ 通过 |
| P6 | reloptions存储验证 | ✓ 通过 |
| P7 | 查询功能验证 | ✓ 通过 |

---

## 功能验证详情

### 1. 核心功能验证

#### ✓ Embedding函数创建
- 成功创建接受text参数的embedding函数
- 函数返回vector类型

#### ✓ 表创建
- 成功创建带有EMBEDDING标记的列
- 自动创建对应的_embedding后缀列
- 自动创建触发器

#### ✓ 数据插入
- 触发器正确调用embedding函数
- 向量正确生成并存储在_embedding列
- 支持批量插入

#### ✓ 数据更新
- 触发器在UPDATE时正确触发
- 向量正确更新

### 2. 查询功能验证

#### ✓ 向量距离操作符
- **<-> (L2距离)**：正确计算并排序
- **<=> (余弦距离)**：正确计算并排序
- **<#> (内积)**：正确计算并排序

#### ✓ 查询重写
- ORDER BY中的embedding列自动替换为_embedding列
- 查询条件自动通过embedding函数转换为向量
- 日志显示重写成功

### 3. 高级功能验证

#### ✓ NULL值处理
- NULL值正确处理，不调用embedding函数
- _embedding列保持NULL

#### ✓ 多embedding列
- 支持表中多个EMBEDDING列
- 每个列独立生成向量

#### ✓ 向量索引
- 成功创建ivfflat索引
- 索引可用于加速查询

#### ✓ 复杂查询
- 支持带WHERE条件的向量查询
- 支持带JOIN的向量查询

### 4. 性能测试

#### ✓ 批量插入
- 成功插入100条记录
- 每条记录的向量正确生成
- 触发器性能良好

---

## 向量索引自动创建功能详细测试

### 测试说明

EMBEDDING功能支持在建表时通过 `vector_index` 和 `vector_distance` 参数自动创建向量索引，无需手动执行 `CREATE INDEX` 语句。

- **vector_index**：索引类型，支持 `ivfflat`（默认）和 `hnsw`
- **vector_distance**：距离度量操作符类，支持 `vector_l2_ops`（默认）、`vector_cosine_ops`、`vector_ip_ops`

### V1: 默认索引（ivfflat + vector_l2_ops）

**测试脚本：**
```sql
CREATE TABLE test_vec_default (
    id int PRIMARY KEY,
    content text EMBEDDING
) WITH (
    vector_len = 3,
    embedding_function = 'simple_embedding'
);
```

**验证脚本：**
```sql
SELECT indexname, indexdef FROM pg_indexes
WHERE tablename = 'test_vec_default' AND indexname LIKE '%embedding%';
```

**实际结果：**
```
              indexname                |                                        indexdef
---------------------------------------+---------------------------------------------------------------------------------------
 test_vec_default_content_embedding_idx | CREATE INDEX test_vec_default_content_embedding_idx ON public.test_vec_default USING ivfflat (content_embedding)
```

**结论：** ✅ 不指定 vector_index 和 vector_distance 时，自动创建 ivfflat 索引，使用默认的 vector_l2_ops 操作符类。索引名格式为 `{表名}_{列名}_embedding_idx`。

### V2-V7: 索引类型与距离度量组合

ivfflat/hnsw 与 vector_l2_ops/vector_cosine_ops/vector_ip_ops 的所有6种组合均测试通过，自动创建的索引定义正确。

### V8: 多个EMBEDDING列 - 每个都自动创建索引

**测试脚本：**
```sql
CREATE TABLE test_vec_multi_embed (
    id int PRIMARY KEY,
    title text EMBEDDING,
    body text EMBEDDING
) WITH (
    vector_len = 3,
    embedding_function = 'simple_embedding',
    vector_index = hnsw,
    vector_distance = vector_cosine_ops
);
```

**实际结果：**
```
                indexname                 |                                        indexdef
------------------------------------------+---------------------------------------------------------------------------------------
 test_vec_multi_embed_body_embedding_idx  | CREATE INDEX ... USING hnsw (body_embedding vector_cosine_ops)
 test_vec_multi_embed_title_embedding_idx | CREATE INDEX ... USING hnsw (title_embedding vector_cosine_ops)
```

**结论：** ✅ 每个EMBEDDING列都自动创建了对应的向量索引。

### V9: reloptions存储验证

**验证脚本：**
```sql
SELECT reloptions FROM pg_class WHERE relname = 'test_vec_reloptions';
```

**实际结果：**
```
 {vector_len=3,embedding_function=simple_embedding,vector_index=hnsw,vector_distance=vector_cosine_ops}
```

**结论：** ✅ vector_index 和 vector_distance 参数正确存储在 pg_class.reloptions 中。

---

## 向量索引参数传递功能详细测试

### 测试说明

建表时可以在 WITH 子句中指定向量索引的构建参数（`lists`、`m`、`ef_construction`），这些参数会自动传递到自动创建的向量索引的 WITH 子句中。

### P1: 默认参数（无lists/m/ef_construction）

**测试脚本：**
```sql
CREATE TABLE test_idx_params_default (
    id int PRIMARY KEY,
    content text EMBEDDING
) WITH (
    vector_len = 3,
    embedding_function = 'simple_embedding',
    vector_index = ivfflat,
    vector_distance = vector_l2_ops
);
```

**验证脚本：**
```sql
SELECT indexname, indexdef FROM pg_indexes
WHERE tablename = 'test_idx_params_default' AND indexname LIKE '%embedding%';
```

**实际结果：**
```
                   indexname                   |                                        indexdef
-----------------------------------------------+--------------------------------------------------------------------------------------------------------------------------------
 test_idx_params_default_content_embedding_idx | CREATE INDEX test_idx_params_default_content_embedding_idx ON public.test_idx_params_default USING ivfflat (content_embedding)
```

**结论：** ✅ 不指定 lists/m/ef_construction 时，索引不包含 WITH 子句，使用 pgvector 默认值。

### P2: ivfflat + lists=100

**测试脚本：**
```sql
CREATE TABLE test_idx_params_ivfflat_lists (
    id int PRIMARY KEY,
    content text EMBEDDING
) WITH (
    vector_len = 3,
    embedding_function = 'simple_embedding',
    vector_index = ivfflat,
    vector_distance = vector_l2_ops,
    lists = 100
);
```

**验证脚本：**
```sql
SELECT indexname, indexdef FROM pg_indexes
WHERE tablename = 'test_idx_params_ivfflat_lists' AND indexname LIKE '%embedding%';
```

**实际结果：**
```
                     indexname                      |                                                        indexdef
-----------------------------------------------------+---------------------------------------------------------------------------------------------------------------------------------------------------------------
 test_idx_params_ivfflat_lists_content_embedding_idx | CREATE INDEX test_idx_params_ivfflat_lists_content_embedding_idx ON public.test_idx_params_ivfflat_lists USING ivfflat (content_embedding) WITH (lists='100')
```

**结论：** ✅ lists=100 参数正确传递到索引的 WITH 子句中。

### P3: hnsw + m=16

**测试脚本：**
```sql
CREATE TABLE test_idx_params_hnsw_m (
    id int PRIMARY KEY,
    content text EMBEDDING
) WITH (
    vector_len = 3,
    embedding_function = 'simple_embedding',
    vector_index = hnsw,
    vector_distance = vector_cosine_ops,
    m = 16
);
```

**验证脚本：**
```sql
SELECT indexname, indexdef FROM pg_indexes
WHERE tablename = 'test_idx_params_hnsw_m' AND indexname LIKE '%embedding%';
```

**实际结果：**
```
                  indexname                   |                                        indexdef
----------------------------------------------+-----------------------------------------------------------------------------------------------------------------------------------------------------------
 test_idx_params_hnsw_m_content_embedding_idx | CREATE INDEX test_idx_params_hnsw_m_content_embedding_idx ON public.test_idx_params_hnsw_m USING hnsw (content_embedding vector_cosine_ops) WITH (m='16')
```

**结论：** ✅ m=16 参数正确传递到索引的 WITH 子句中。

### P4: hnsw + m=16 + ef_construction=64

**测试脚本：**
```sql
CREATE TABLE test_idx_params_hnsw_all (
    id int PRIMARY KEY,
    content text EMBEDDING
) WITH (
    vector_len = 3,
    embedding_function = 'simple_embedding',
    vector_index = hnsw,
    vector_distance = vector_cosine_ops,
    m = 16,
    ef_construction = 64
);
```

**验证脚本：**
```sql
SELECT indexname, indexdef FROM pg_indexes
WHERE tablename = 'test_idx_params_hnsw_all' AND indexname LIKE '%embedding%';
```

**实际结果：**
```
                   indexname                    |                                        indexdef
------------------------------------------------+-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
 test_idx_params_hnsw_all_content_embedding_idx | CREATE INDEX test_idx_params_hnsw_all_content_embedding_idx ON public.test_idx_params_hnsw_all USING hnsw (content_embedding vector_cosine_ops) WITH (m='16', ef_construction='64')
```

**结论：** ✅ m=16 和 ef_construction=64 两个参数同时正确传递到索引的 WITH 子句中。

### P5: ivfflat + lists=50 + vector_cosine_ops

**测试脚本：**
```sql
CREATE TABLE test_idx_params_ivfflat_lists_cosine (
    id int PRIMARY KEY,
    content text EMBEDDING
) WITH (
    vector_len = 3,
    embedding_function = 'simple_embedding',
    vector_index = ivfflat,
    vector_distance = vector_cosine_ops,
    lists = 50
);
```

**验证脚本：**
```sql
SELECT indexname, indexdef FROM pg_indexes
WHERE tablename = 'test_idx_params_ivfflat_lists_cosine' AND indexname LIKE '%embedding%';
```

**实际结果：**
```
                         indexname                          |                                        indexdef
------------------------------------------------------------+----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
 test_idx_params_ivfflat_lists_cosine_content_embedding_idx | CREATE INDEX test_idx_params_ivfflat_lists_cosine_content_embedding_idx ON public.test_idx_params_ivfflat_lists_cosine USING ivfflat (content_embedding vector_cosine_ops) WITH (lists='50')
```

**结论：** ✅ lists=50 和 vector_cosine_ops 组合正确，lists 参数正确传递到索引。

### P6: reloptions存储验证

**验证脚本：**
```sql
SELECT relname, reloptions FROM pg_class WHERE relname LIKE 'test_idx_params_%' ORDER BY relname;
```

**实际结果：**
```
                          relname                           |                                        reloptions
------------------------------------------------------------+-----------------------------------------------------------------
 test_idx_params_default                                    | {vector_len=3,embedding_function=simple_embedding,vector_index=ivfflat,vector_distance=vector_l2_ops}
 test_idx_params_hnsw_all                                   | {vector_len=3,embedding_function=simple_embedding,vector_index=hnsw,vector_distance=vector_cosine_ops,m=16,ef_construction=64}
 test_idx_params_hnsw_m                                     | {vector_len=3,embedding_function=simple_embedding,vector_index=hnsw,vector_distance=vector_cosine_ops,m=16}
 test_idx_params_ivfflat_lists                              | {vector_len=3,embedding_function=simple_embedding,vector_index=ivfflat,vector_distance=vector_l2_ops,lists=100}
 test_idx_params_ivfflat_lists_cosine                       | {vector_len=3,embedding_function=simple_embedding,vector_index=ivfflat,vector_distance=vector_cosine_ops,lists=50}
```

**结论：** ✅ lists、m、ef_construction 参数正确存储在表的 pg_class.reloptions 中，同时也正确传递到索引的 reloptions 中。

### P7: 查询功能验证

**测试脚本：**
```sql
SELECT id, content FROM test_idx_params_hnsw_all ORDER BY content <=> 'test' LIMIT 5;
```

**实际结果：** ✅ 返回正确排序结果（1行）

---

## 向量索引自动创建功能总结

### 索引类型与距离度量组合矩阵

| | vector_l2_ops | vector_cosine_ops | vector_ip_ops |
|---|---|---|---|
| **ivfflat** | ✅ V1(默认)/V2 | ✅ V3 | ✅ V4 |
| **hnsw** | ✅ V5 | ✅ V6 | ✅ V7 |

所有6种组合均测试通过。

### 索引参数传递矩阵

| 参数 | ivfflat | hnsw | 测试结果 |
|------|---------|------|----------|
| `lists` | ✅ P2, P5 | 不适用 | 正确传递 |
| `m` | 不适用 | ✅ P3, P4 | 正确传递 |
| `ef_construction` | 不适用 | ✅ P4 | 正确传递 |
| 无参数 | ✅ P1 | ✅ P1 | 使用pgvector默认值 |

### 索引命名规则

自动创建的向量索引遵循命名规则：`{表名}_{EMBEDDING列名}_embedding_idx`

### 默认值

- 不指定 `vector_index` 时默认为 `ivfflat`
- 不指定 `vector_distance` 时默认为 `vector_l2_ops`
- 不指定 `lists`/`m`/`ef_construction` 时，索引不包含 WITH 子句，使用 pgvector 自身默认值

### 注意事项

1. **ivfflat 索引提示**：数据量较少时创建 ivfflat 索引会产生 NOTICE 提示 "ivfflat index created with little data, This will cause low recall"，建议数据量达到一定规模后索引效果更佳
2. **HNSW 索引**：没有数据量限制，适合初始数据量较少的场景
3. **多EMBEDDING列**：每个EMBEDDING列都会自动创建对应的向量索引
4. **索引参数**：`lists` 适用于 ivfflat，`m` 和 `ef_construction` 适用于 hnsw，不相关的参数传递到索引时会被 pgvector 忽略
5. **手动创建索引**：如需自定义更多索引参数，可先删除自动创建的索引再手动创建

---

## 功能亮点

1. **简化使用**：用户只需创建一个text→vector的函数
2. **自动化**：触发器自动管理向量生成和更新
3. **透明重写**：查询自动重写，用户无需关心底层实现
4. **完整支持**：支持所有pgvector距离操作符
5. **索引支持**：支持向量索引加速查询
6. **自动索引创建**：建表时通过参数自动创建向量索引，支持 ivfflat/hnsw + 多种距离度量
7. **索引参数传递**：lists/m/ef_construction 参数自动传递到向量索引

## 测试结论

**总体评价：优秀** ✅

所有核心功能均正常工作，包括向量索引自动创建和索引参数传递功能。embedding功能已经可以投入使用。

## 测试通过率

**100%** (31/31项测试全部通过：基础功能15项 + 向量索引自动创建9项 + 索引参数传递7项)
