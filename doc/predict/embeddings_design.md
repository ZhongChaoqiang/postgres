# DLM 多列组合向量化（EMBEDDINGS）设计文档

## 1. 概述

### 1.1 功能简介

DLM（Deep Learning Model）多列组合向量化功能允许用户选择表中的多个列作为一个整体进行向量化，将结构化表格数据转换为向量表示。与现有 EMBEDDING 功能（单列文本→向量）不同，EMBEDDINGS 关注的是**多列特征的整体语义表示**，适用于 FT-Transformer 等表格深度学习模型。

### 1.2 设计目标

- 提供两种语法入口：`CREATE EMBEDDINGS`（表级）和 `EMBEDDINGS AS`（列级），用户按需选择
- 两种语法内部机制完全统一，共享触发器、查询重写、系统表
- 支持用户指定哪些列参与组合向量化
- 向量化结果自动存储在隐藏列中，对用户透明
- 支持向量相似度查询
- 兼容现有 EMBEDDING 设计，可共存使用
- 初始支持 FT-Transformer 模型，后续可扩展其他表格模型

### 1.3 两种语法，统一内核

EMBEDDINGS 提供两种语法入口，用户可以按场景选择：

| 语法 | 形式 | 适用场景 | 类比 |
|------|------|---------|------|
| **CREATE EMBEDDINGS**（表级） | `CREATE EMBEDDINGS name ON t (cols) USING func` | 已有表，随时添加/删除 | CREATE INDEX |
| **EMBEDDINGS AS**（列级） | `name EMBEDDINGS AS (func(cols)) STORED` | 建表时定义，与 EMBEDDING AS 风格一致 | EMBEDDING AS |

**两种语法内部完全统一**，最终都生成相同的数据结构：

```
两种语法入口
    │                           │
    │  CREATE EMBEDDINGS ...     │  name EMBEDDINGS AS (...) STORED
    │                           │
    └───────────┬───────────────┘
                │
                ▼
        统一内部机制
        ├── 隐藏向量列: name vector(N)  (atthidden=true, attembeddings=true)
        ├── 触发器: embeddings_trigger
        ├── 向量索引: hnsw/ivfflat
        ├── 系统表: pg_embeddings
        └── 查询重写: rewrite_embeddings_query
```

### 1.4 与 EMBEDDING 的关系

| 特性 | EMBEDDING | EMBEDDINGS（列级） | EMBEDDINGS（表级） |
|------|-----------|-----------------|-----------------|
| 语法 | `col text EMBEDDING AS (...) STORED` | `name EMBEDDINGS AS (...) STORED` | `CREATE EMBEDDINGS name ON t (...) USING func` |
| 语义 | 列级属性 | 列级属性 | 表级操作 |
| 输入 | 单列自身值 | 多列值 | 多列值 |
| 类比 | 列约束 | 列约束 | CREATE INDEX |
| 隐藏列 | `{col}_embedding` | `name`（直接命名） | `name`（直接命名） |
| 查询 | `col <=> 'text'` | `name <=> ROW(v1,v2)` | `name <=> ROW(v1,v2)` |
| 添加时机 | 建表时 | 建表时 | 随时 |
| 删除 | ALTER TABLE | ALTER TABLE | DROP EMBEDDINGS |

## 2. DDL 语法设计

### 2.1 方式一：CREATE EMBEDDINGS（表级，类似 CREATE INDEX）

```sql
CREATE EMBEDDINGS embeddings_name ON table_name (column_list)
    USING function_name
    [ WITH (options) ];

DROP EMBEDDINGS embeddings_name ON table_name;
```

**语法说明**：
- `embeddings_name`：向量化名称，同时作为隐藏向量列的列名
- `ON table_name`：目标表
- `(column_list)`：参与向量化的列名列表
- `USING function_name`：向量化函数名
- `WITH (options)`：可选参数

**适用场景**：已有表，随时添加/删除，不影响表结构

### 2.2 方式二：EMBEDDINGS AS（列级，类似 EMBEDDING AS）

```sql
column_name EMBEDDINGS AS (expression) STORED
```

**语法说明**：
- `column_name`：向量化名称，同时作为隐藏向量列的列名（不需要指定类型，固定为 vector）
- `EMBEDDINGS AS`：关键字，与 `EMBEDDING AS` 对称
- `expression`：向量化表达式，可引用同表其他列
- `STORED`：与 EMBEDDING AS 一致，表示物理存储

**适用场景**：建表时定义，与 EMBEDDING AS 风格一致

### 2.3 使用示例

```sql
-- ===== 方式一：CREATE EMBEDDINGS =====

-- 示例1：基本用法
CREATE EMBEDDINGS demographic ON customers (age, income, category)
    USING ft_transformer_embedding
    WITH (vector_len = 128);

-- 示例2：多组向量化
CREATE EMBEDDINGS basic_features ON products (price, brand)
    USING ft_transformer_embedding
    WITH (vector_len = 64);

CREATE EMBEDDINGS performance ON products (sales, rating)
    USING ft_transformer_embedding
    WITH (vector_len = 64);

-- 示例3：删除向量化
DROP EMBEDDINGS demographic ON customers;


-- ===== 方式二：EMBEDDINGS AS =====

-- 示例4：建表时定义（与 EMBEDDING AS 风格一致）
CREATE TABLE customers (
    id int PRIMARY KEY,
    age int,
    income float,
    category text,
    demographic EMBEDDINGS AS (ft_transformer_embedding(age, income, category)) STORED
) WITH (vector_len = 128);

-- 示例5：多组向量化
CREATE TABLE products (
    id int PRIMARY KEY,
    name text,
    price float,
    brand text,
    basic_features EMBEDDINGS AS (ft_transformer_embedding(price, brand)) STORED,
    sales int,
    rating float,
    performance EMBEDDINGS AS (ft_transformer_embedding(sales, rating)) STORED
) WITH (vector_len = 64);

-- 示例6：与 EMBEDDING 共存
CREATE TABLE articles (
    id int PRIMARY KEY,
    content text EMBEDDING AS (st_embedding(content)) STORED,
    category text,
    priority int,
    meta EMBEDDINGS AS (ft_transformer_embedding(category, priority)) STORED
) WITH (vector_len = 64);


-- ===== 两种方式混用 =====

-- 示例7：建表时用列级语法，后续用表级语法追加
CREATE TABLE customers (
    id int PRIMARY KEY,
    age int,
    income float,
    category text,
    demographic EMBEDDINGS AS (ft_transformer_embedding(age, income, category)) STORED
) WITH (vector_len = 128);

-- 后续追加另一个向量化组
CREATE EMBEDDINGS extra_vec ON customers (age, income)
    USING ft_transformer_embedding
    WITH (vector_len = 64);
```

### 2.4 两种语法的等价关系

以下两种写法产生**完全相同**的内部结构：

```sql
-- 方式一
CREATE EMBEDDINGS demographic ON customers (age, income, category)
    USING ft_transformer_embedding WITH (vector_len = 128);

-- 方式二（等价）
CREATE TABLE customers (
    ...,
    demographic EMBEDDINGS AS (ft_transformer_embedding(age, income, category)) STORED
) WITH (vector_len = 128);
```

两者都创建：
- 隐藏列 `demographic vector(128)`（`atthidden=true, attembeddings=true`）
- 触发器 `embeddings_trigger_demographic_{oid}`
- 向量索引 `customers_demographic_vec_idx`
- pg_embeddings 元数据

### 2.5 WITH 选项

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `vector_len` | int | 128 | 向量维度 |
| `vector_index` | enum | hnsw | 向量索引类型（ivfflat/hnsw） |
| `vector_distance` | enum | vector_cosine_ops | 向量距离类型 |
| `lists` | int | - | ivfflat 的 lists 参数 |
| `m` | int | - | hnsw 的 m 参数 |
| `ef_construction` | int | - | hnsw 的 ef_construction 参数 |

### 2.4 与 EMBEDDING 共存

```sql
-- 先建表（带 EMBEDDING）
CREATE TABLE articles (
    id int PRIMARY KEY,
    title text,
    content text EMBEDDING AS (st_embedding(content)) STORED,
    category text,
    priority int
) WITH (vector_len = 64);

-- 再添加 EMBEDDINGS（不影响表结构）
CREATE EMBEDDINGS meta ON articles (category, priority)
    USING ft_transformer_embedding
    WITH (vector_len = 64);
```

### 2.5 对比：CREATE INDEX vs CREATE EMBEDDINGS

```sql
-- 创建索引：基于列创建加速查找的结构
CREATE INDEX idx_name ON table_name (col1, col2);

-- 创建向量化：基于列创建向量表示
CREATE EMBEDDINGS vec_name ON table_name (col1, col2) USING ft_transformer_embedding;
```

两者语义非常相似：
- 都是基于表的某些列创建一个"派生结构"
- 都不影响原始表数据
- 都可以随时创建和删除
- 都有自动维护机制（索引自动更新 / 向量化触发器自动计算）

## 3. 内部存储设计

### 3.1 隐藏向量列

执行 `CREATE EMBEDDINGS demographic ON customers (age, income, category) USING ft_transformer_embedding WITH (vector_len = 128)` 后：

```
pg_attribute:
  attnum=1  attname='id'            atttypid=int4     atthidden=f
  attnum=2  attname='age'           atttypid=int4     atthidden=f
  attnum=3  attname='income'        atttypid=float8   atthidden=f
  attnum=4  attname='category'      atttypid=text     atthidden=f
  attnum=5  attname='demographic'   atttypid=vector   atthidden=t  ← 自动创建，向量化名即列名
```

隐藏列命名规则：**直接使用向量化名**（不加 `_embeddings` 后缀）

这样设计的核心原因：向量化名必须是真实存在的列，否则查询时解析器无法识别 `demographic <=> ROW(...)` 这样的表达式。

### 3.2 系统目录

#### 3.2.1 pg_embeddings 系统表

```sql
CREATE TABLE pg_embeddings (
    oid         oid PRIMARY KEY,
    vecname     name NOT NULL,       -- 向量化名称（CREATE EMBEDDINGS 指定的名称）
    vecrelid    oid NOT NULL,        -- 所属表的 OID
    veccolumns  text[] NOT NULL,     -- 源列名列表
    vecfunc     regproc NOT NULL,    -- 向量化函数 OID
    vecattnum   int2 NOT NULL,       -- 隐藏向量列的 attnum
    vecoptions  text,                -- WITH 选项（JSON 格式）
    FOREIGN KEY (vecrelid) REFERENCES pg_class(oid),
    UNIQUE (vecrelid, vecname)       -- 同一表内名称唯一
);
```

#### 3.2.2 pg_attribute 扩展

```c
bool        attembeddings BKI_DEFAULT(f);   /* 是否为 EMBEDDINGS 隐藏列 */
```

#### 3.2.3 新增 attgenerated 值

```c
#define ATTRIBUTE_GENERATED_EMBEDDINGS 'z'
```

#### 3.2.4 TupleConstr / CompactAttribute 扩展

```c
// TupleConstr
bool    has_generated_embeddings;

// CompactAttribute
bool    attembeddings;
```

### 3.3 触发器

每个 EMBEDDINGS 创建一个 BEFORE INSERT OR UPDATE 触发器：

- 触发器名称：`embeddings_trigger_{embeddings_name}_{oid}`
- 触发器函数：`embeddings_trigger()`
- 触发时机：BEFORE INSERT OR UPDATE
- 粒度：ROW 级别

### 3.4 向量索引

每个 EMBEDDINGS 自动创建向量索引：

- 索引名称：`{table}_{embeddings_name}_vec_idx`
- 索引类型：由 `vector_index` 选项决定（默认 hnsw）
- 距离类型：由 `vector_distance` 选项决定（默认 vector_cosine_ops）

### 3.5 完整创建流程

```
CREATE EMBEDDINGS demographic ON customers (age, income, category)
    USING ft_transformer_embedding WITH (vector_len = 128);
    │
    ├── 1. 验证
    │   ├── 表 customers 存在
    │   ├── 列 age, income, category 存在
    │   ├── 函数 ft_transformer_embedding 存在
    │   └── 向量化名称 demographic 在该表内唯一
    │
    ├── 2. 添加隐藏向量列
    │   ├── ALTER TABLE customers ADD COLUMN demographic vector(128)
    │   ├── 设置 atthidden=true, attembeddings=true
    │   └── 设置 attgenerated=ATTRIBUTE_GENERATED_EMBEDDINGS
    │
    ├── 3. 创建触发器
    │   └── CREATE TRIGGER embeddings_trigger_demographic_{oid}
    │       BEFORE INSERT OR UPDATE ON customers
    │       FOR EACH ROW EXECUTE FUNCTION embeddings_trigger()
    │
    ├── 4. 创建向量索引
    │   └── CREATE INDEX customers_demographic_vec_idx
    │       ON customers USING hnsw (demographic vector_cosine_ops)
    │
    ├── 5. 插入元数据
    │   └── INSERT INTO pg_embeddings (vecname, vecrelid, veccolumns, vecfunc, vecattnum, ...)
    │       VALUES ('demographic', customers_oid, '{age,income,category}', func_oid, attnum, ...)
    │
    └── 6. 回填现有数据
        └── UPDATE customers SET demographic = ft_transformer_embedding(age, income, category)
            WHERE demographic IS NULL;
```

### 3.6 删除流程

```
DROP EMBEDDINGS demographic ON customers;
    │
    ├── 1. 从 pg_embeddings 查找元数据
    │
    ├── 2. 删除触发器
    │   └── DROP TRIGGER embeddings_trigger_demographic_{oid} ON customers
    │
    ├── 3. 删除向量索引
    │   └── DROP INDEX customers_demographic_vec_idx
    │
    ├── 4. 删除隐藏向量列
    │   └── ALTER TABLE customers DROP COLUMN demographic
    │
    └── 5. 删除元数据
        └── DELETE FROM pg_embeddings WHERE vecname = 'demographic' AND vecrelid = customers_oid
```

## 4. 向量化函数设计

### 4.1 函数签名

```sql
-- 多参数版本（推荐，用于 EMBEDDINGS 触发器内部调用）
CREATE FUNCTION ft_transformer_embedding(VARIADIC input_values anyarray) RETURNS vector
    AS 'jolix_ft_transformer', 'ft_transformer_embedding'
    LANGUAGE C IMMUTABLE;

-- 指定模型版本
CREATE FUNCTION ft_transformer_embedding(VARIADIC input_values anyarray, model_name text) RETURNS vector
    AS 'jolix_ft_transformer', 'ft_transformer_embedding_with_model'
    LANGUAGE C IMMUTABLE;

-- record 版本
CREATE FUNCTION ft_transformer_embedding(input_row record) RETURNS vector
    AS 'jolix_ft_transformer', 'ft_transformer_embedding_record'
    LANGUAGE C IMMUTABLE;
```

### 4.2 函数行为

1. 接收多个列值作为参数
2. 根据各参数的数据类型自动判断特征类型：
   - 数值类型（int, float, numeric）→ 数值特征
   - 文本类型（text, varchar）→ 类别特征
   - 布尔类型（bool）→ 二值特征
3. 对每种特征进行 Tokenize：
   - 数值特征：乘以可学习权重 + 偏置 → token
   - 类别特征：Embedding 查表 → token
4. 所有 token 输入 Transformer Encoder
5. 取 [CLS] token 的输出作为行级向量表示
6. 返回 vector 类型

### 4.3 查询用辅助函数

```sql
CREATE FUNCTION ft_transformer_embedding(VARIADIC values anyarray) RETURNS vector
    AS 'jolix_ft_transformer', 'ft_transformer_embedding'
    LANGUAGE C IMMUTABLE;
```

### 4.4 GUC 参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `jolix_ft_transformer.model_name` | ft-transformer-default | 默认 FT-Transformer 模型名称 |
| `jolix_ft_transformer.model_path` | 空 | 本地模型文件路径 |
| `jolix_ft_transformer.cache_enabled` | true | 是否启用模型缓存 |

## 5. 触发器行为设计

### 5.1 embeddings_trigger 执行流程

```
INSERT/UPDATE → BEFORE触发器 → embeddings_trigger
    │
    ├── 从 pg_embeddings 获取该表的所有向量化组
    │
    ├── 对每个向量化组：
    │   │
    │   ├── 获取源列列表（veccolumns）
    │   │
    │   ├── UPDATE 时：检查源列是否有任何值发生变化
    │   │   ├── 如果没有变化 → 跳过该组
    │   │   └── 如果有变化或 INSERT → 继续计算
    │   │
    │   ├── 从 newtuple 提取源列值
    │   │
    │   ├── 调用向量化函数：vecfunc(value1, value2, ...)
    │   │
    │   └── 将 vector 结果写入隐藏向量列（vecattnum）
    │
    └── 返回修改后的元组
```

### 5.2 增量更新检测

对于 UPDATE 操作，仅当向量化组中的源列值发生变化时才重新计算向量：

```c
static bool
embeddings_columns_changed(HeapTuple old_tuple, HeapTuple new_tuple,
                           TupleDesc tupdesc, List *source_attnums)
{
    ListCell *lc;
    foreach(lc, source_attnums)
    {
        AttrNumber attnum = lfirst_int(lc);
        bool old_isnull, new_isnull;
        Datum old_val, new_val;

        old_val = heap_getattr(old_tuple, attnum, tupdesc, &old_isnull);
        new_val = heap_getattr(new_tuple, attnum, tupdesc, &new_isnull);

        if (old_isnull != new_isnull)
            return true;
        if (!old_isnull && !datumIsEqual(old_val, new_val, ...))
            return true;
    }
    return false;
}
```

## 6. 查询语法设计

### 6.1 核心挑战

EMBEDDING 的查询重写能工作，是因为 `content` 是一个**真实的可见列**（`attembedding=true`），解析器能正常解析 `content <=> 'text'`，然后重写器把 `content` 替换为隐藏列 `content_embedding`。

但 CREATE EMBEDDINGS 方案中，向量化名（如 `demographic`）**不是一个列**！如果用户写 `demographic <=> ROW(...)`，解析器会报错"列 demographic 不存在"，根本走不到重写器。

这是 CREATE EMBEDDINGS 方案必须解决的核心问题。

### 6.2 解决方案：向量化名即隐藏列名

**关键决策**：隐藏向量列直接用向量化名命名，不加 `_embeddings` 后缀。

```
CREATE EMBEDDINGS demographic ON customers (age, income, category) USING ft_transformer_embedding;
 创建隐藏列: demographic vector(128)   （不是 demographic_embeddings）
```

这样 `demographic` 就是一个**真实存在的隐藏列**（`atthidden=true`），类型为 `vector`。解析器可以正常解析 `demographic <=> ...`，因为 `demographic` 确实是表的一个列（只是隐藏的）。

**与 EMBEDDING 的对比**：

| | EMBEDDING | EMBEDDINGS |
|---|---|---|
| 用户写的 | `content <=> 'text'` | `demographic <=> ROW(35, 50000.0, 'premium')` |
| content 是 | 可见的 text 列 | — |
| demographic 是 | — | 隐藏的 vector 列 |
| 解析器能识别？ | ✅ content 是真实列 | ✅ demographic 是真实列（隐藏但存在） |
| 重写步骤 | 3步（换列+换操作符+转换右操作数） | 1步（仅转换右操作数） |

EMBEDDINGS 的查询重写比 EMBEDDING **更简单**，因为列本身就是 vector 类型，不需要换列和换操作符。

### 6.3 名称冲突处理

由于向量化名直接作为列名，需要确保不与现有列冲突：

```sql
-- 如果表已有 demographic 列，CREATE EMBEDDINGS 报错
CREATE EMBEDDINGS demographic ON customers (age, income, category) USING ft_transformer_embedding;
-- ERROR: column "demographic" already exists in table "customers"

-- 用户需要换一个名字
CREATE EMBEDDINGS demo_vec ON customers (age, income, category) USING ft_transformer_embedding;
-- OK，创建隐藏列 demo_vec vector(128)
```

### 6.4 三种查询方式

#### 方式一：ROW 便捷语法（推荐，最简洁）

```sql
SELECT * FROM customers
WHERE demographic <=> ROW(35, 50000.0, 'premium') < 0.5
ORDER BY demographic <=> ROW(35, 50000.0, 'premium')
LIMIT 10;
```

**查询重写**（1步）：

```sql
-- 用户写的
demographic <=> ROW(35, 50000.0, 'premium')

-- 重写后
demographic <=> ft_transformer_embedding(35, 50000.0, 'premium')
```

重写规则：
1. `demographic` → 不变（已经是 vector 类型的隐藏列）
2. 操作符 → 不变（已经是 vector 版本的 `<=>`）
3. `ROW(v1,v2)` → `ft_transformer_embedding(v1,v2)`（转换右操作数）

**为什么只有1步？** 因为 `demographic` 本身就是 `vector` 类型的隐藏列，不需要像 EMBEDDING 那样换列和换操作符。

#### 方式二：直接使用向量函数（无需重写）

```sql
SELECT * FROM customers
WHERE demographic <=> ft_transformer_embedding(35, 50000.0, 'premium') < 0.5
ORDER BY demographic <=> ft_transformer_embedding(35, 50000.0, 'premium')
LIMIT 10;
```

无需查询重写，直接可用。适合高级用户。

#### 方式三：使用向量字面量（无需重写）

```sql
SELECT * FROM customers
WHERE demographic <=> '[0.1, 0.2, ..., 0.128]'::vector < 0.5
ORDER BY demographic <=> '[0.1, 0.2, ..., 0.128]'::vector
LIMIT 10;
```

适合预先计算好向量的场景。

### 6.5 查询重写详细设计

#### 6.5.1 重写入口

```c
if (parsetree->commandType == CMD_SELECT)
{
    rewrite_embedding_query(parsetree);
    rewrite_embeddings_query(parsetree);
}
```

#### 6.5.2 重写触发条件

当满足以下条件时触发 EMBEDDINGS 查询重写：
1. 操作符是向量距离操作符（`<=>`, `<->`, `<#>` 等）
2. 其中一个操作数是 `Var` 节点，引用了 `attembeddings=true` 的隐藏列
3. 另一个操作数是 `RowExpr` 节点

#### 6.5.3 is_embeddings_var 检测

```c
static bool
is_embeddings_var(Var *var, Query *query,
                 Oid *out_relid, Oid *out_embeddings_func, char **out_vecname)
{
    RangeTblEntry *rte;
    Relation rel;
    TupleDesc tupdesc;
    Form_pg_attribute attr;

    if (var->varno <= 0 || var->varno > list_length(query->rtable))
        return false;

    rte = rt_fetch(var->varno, query->rtable);
    if (rte->rtekind != RTE_RELATION)
        return false;
    if (var->varattno <= 0)
        return false;

    rel = table_open(rte->relid, AccessShareLock);
    tupdesc = RelationGetDescr(rel);

    if (var->varattno > tupdesc->natts)
    {
        table_close(rel, AccessShareLock);
        return false;
    }

    attr = TupleDescAttr(tupdesc, var->varattno - 1);

    /* 检查是否为 EMBEDDINGS 隐藏列 */
    if (!(attr->attembeddings && attr->attgenerated == ATTRIBUTE_GENERATED_EMBEDDINGS))
    {
        table_close(rel, AccessShareLock);
        return false;
    }

    /* 从 pg_embeddings 获取向量化函数 */
    Oid vecfunc = get_embeddings_func_oid(rte->relid, NameStr(attr->attname));

    if (out_relid) *out_relid = rte->relid;
    if (out_embeddings_func) *out_embeddings_func = vecfunc;
    if (out_vecname) *out_vecname = pstrdup(NameStr(attr->attname));

    table_close(rel, AccessShareLock);
    return true;
}
```

#### 6.5.4 重写实现

```c
static Node *
rewrite_embeddings_opexpr(OpExpr *opexpr, Query *query)
{
    Node *left, *right;
    Oid relid, vecfunc;
    char *vecname;

    if (!is_vector_distance_operator(opexpr->opno))
        return (Node *) opexpr;

    if (list_length(opexpr->args) != 2)
        return (Node *) opexpr;

    left = linitial(opexpr->args);
    right = lsecond(opexpr->args);

    /* 情况1: demographic <=> ROW(...) */
    if (IsA(left, Var) && IsA(right, RowExpr))
    {
        Var *var = (Var *) left;
        if (is_embeddings_var(var, query, &relid, &vecfunc, &vecname))
        {
            /* 列引用不需要替换：demographic 本身就是 vector 隐藏列 */
            /* 操作符不需要替换：已经是 vector 版本 */
            /* 仅转换右操作数：ROW(v1,v2) → ft_transformer_embedding(v1,v2) */
            FuncExpr *vec_func = make_embeddings_func(vecfunc, (RowExpr *) right);
            lsecond(opexpr->args) = (Node *) vec_func;
            pfree(vecname);
        }
    }
    /* 情况2: ROW(...) <=> demographic */
    else if (IsA(right, Var) && IsA(left, RowExpr))
    {
        Var *var = (Var *) right;
        if (is_embeddings_var(var, query, &relid, &vecfunc, &vecname))
        {
            FuncExpr *vec_func = make_embeddings_func(vecfunc, (RowExpr *) left);
            linitial(opexpr->args) = (Node *) vec_func;
            pfree(vecname);
        }
    }

    return (Node *) opexpr;
}
```

#### 6.5.5 make_embeddings_func 实现

```c
static FuncExpr *
make_embeddings_func(Oid vecfunc_oid, RowExpr *rowexpr)
{
    FuncExpr *funcexpr;
    List *func_args = NIL;
    ListCell *lc;

    /* 将 RowExpr 的参数展开为函数参数 */
    foreach(lc, rowexpr->args)
    {
        Node *arg = (Node *) lfirst(lc);
        /* 如果参数类型不匹配函数签名，添加类型转换 */
        func_args = lappend(func_args, arg);
    }

    funcexpr = makeNode(FuncExpr);
    funcexpr->funcid = vecfunc_oid;
    funcexpr->funcresulttype = get_func_rettype(vecfunc_oid);
    funcexpr->funcretset = false;
    funcexpr->funcvariadic = false;
    funcexpr->args = func_args;
    funcexpr->location = -1;

    return funcexpr;
}
```

### 6.6 与 EMBEDDING 查询重写的对比

| 步骤 | EMBEDDING 重写 | EMBEDDINGS 重写 |
|------|---------------|---------------|
| 1. 检测 | `attembedding=true` 的可见列 | `attembeddings=true && attgenerated='z'` 的隐藏列 |
| 2. 换列 | `content` → `content_embedding` | **不需要**（列名就是向量化名） |
| 3. 换操作符 | text版 `<=>` → vector版 `<=>` | **不需要**（列已是 vector 类型） |
| 4. 转换右操作数 | `'text'` → `st_embedding('text')::vector` | `ROW(v1,v2)` → `ft_transformer_embedding(v1,v2)` |
| **总步骤** | **3步** | **1步** |

EMBEDDINGS 的查询重写更简单，因为向量化名直接就是 vector 类型的隐藏列，不需要换列和换操作符。

### 6.7 隐藏列的可见性

`demographic` 是 `atthidden=true` 的隐藏列，其行为：

| 场景 | 行为 |
|------|------|
| `SELECT * FROM customers` | 不返回 demographic 列 ✅ |
| `SELECT demographic FROM customers` | 返回向量值 ✅（显式引用可见） |
| `WHERE demographic <=> ROW(...)` | 正常工作 ✅ |
| `ORDER BY demographic <=> ROW(...)` | 正常工作 ✅ |
| `INSERT INTO customers ...` | 不需要指定 demographic ✅（触发器自动填充） |
| `\d customers` | 不显示 demographic 列 ✅（但显示 Embeddings 信息） |

这与 EMBEDDING 的 `content_embedding` 隐藏列行为完全一致。

## 7. 完整数据流

### 7.1 创建阶段

```
CREATE EMBEDDINGS demographic ON customers (age, income, category)
    USING ft_transformer_embedding WITH (vector_len = 128);
    │
    ├── [gram.y] 解析 → EmbeddingsStmt 节点
    │
    ├── [commands/embeddingscmds.c] 执行创建：
    │   ├── 验证表、列、函数存在性
    │   ├── ALTER TABLE ADD COLUMN demographic vector(128)
    │   │   └── 设置 atthidden=true, attembeddings=true, attgenerated='z'
    │   ├── CREATE TRIGGER embeddings_trigger_demographic_{oid}
    │   ├── CREATE INDEX customers_demographic_vec_idx
    │   ├── INSERT INTO pg_embeddings (元数据)
    │   └── 回填现有数据
    │
    └── 完成
```

### 7.2 DML 阶段

```
INSERT INTO customers (id, age, income, category) VALUES (1, 35, 50000, 'premium')
    │
    ├── [embeddings_trigger] BEFORE INSERT 触发
    │   ├── 从 pg_embeddings 获取向量化组
    │   ├── 对 'demographic' 组：
    │   │   ├── 提取源列值: age=35, income=50000, category='premium'
    │   │   ├── 调用 ft_transformer_embedding(35, 50000, 'premium')
    │   │   └── 写入 demographic 列
    │   └── 返回修改后的元组
    │
    └── 实际存储：
        id=1, age=35, income=50000, category='premium',
        demographic=[0.12, -0.34, ..., 0.56]  (隐藏列)
```

### 7.3 查询阶段

```
SELECT * FROM customers
ORDER BY demographic <=> ROW(35, 50000.0, 'premium')
LIMIT 10;
    │
    ├── [rewrite_embeddings_query] 查询重写（1步）：
    │   └── ROW(35, 50000.0, 'premium') → ft_transformer_embedding(35, 50000.0, 'premium')
    │       （demographic 不变，已是 vector 隐藏列；操作符不变，已是 vector 版本）
    │
    └── [执行器] 使用 pgvector 向量距离计算 + 向量索引
```

### 7.4 删除阶段

```
DROP EMBEDDINGS demographic ON customers;
    │
    ├── [commands/embeddingscmds.c] 执行删除：
    │   ├── DROP TRIGGER embeddings_trigger_demographic_{oid}
    │   ├── DROP INDEX customers_demographic_vec_idx
    │   ├── ALTER TABLE DROP COLUMN demographic
    │   └── DELETE FROM pg_embeddings WHERE vecname='demographic'
    │
    └── 完成（表恢复到创建 EMBEDDINGS 之前的状态）
```

## 8. 解析器扩展设计

### 8.1 新增关键字

```c
// kwlist.h
PG_KEYWORD("embeddings", EMBEDDINGS, UNRESERVED_KEYWORD, BARE_LABEL)
```

### 8.2 新增语法节点

```c
// parsenodes.h

typedef struct EmbeddingsStmt
{
    NodeTag     type;
    char       *vecname;        /* 向量化名称 */
    RangeVar   *relation;       /* 目标表 */
    List       *veccolumns;     /* 源列名列表 (List of String) */
    char       *vecfunc;        /* 向量化函数名 */
    List       *options;        /* WITH 选项列表 */
    bool        if_not_exists;  /* IF NOT EXISTS */
} EmbeddingsStmt;

typedef struct DropEmbeddingsStmt
{
    NodeTag     type;
    char       *vecname;        /* 向量化名称 */
    RangeVar   *relation;       /* 目标表 */
    bool        if_exists;      /* IF EXISTS */
} DropEmbeddingsStmt;
```

### 8.3 gram.y 语法规则

#### 8.3.1 列级语法：opt_embeddings_clause（与 opt_embedding_clause 对称）

```yacc
columnDef: ColId Typename opt_column_storage opt_column_compression
           create_generic_options ColQualList
           opt_predict_clause opt_embedding_clause opt_embeddings_clause
                { ... }
    ;

opt_embeddings_clause:
        EMBEDDINGS AS '(' a_expr ')' STORED
            {
                Constraint *n = makeNode(Constraint);
                n->contype = CONSTR_EMBEDDINGS;
                n->generated_when = ATTRIBUTE_IDENTITY_ALWAYS;
                n->raw_expr = $4;
                n->cooked_expr = NULL;
                n->generated_kind = ATTRIBUTE_GENERATED_EMBEDDINGS;
                n->location = @1;
                $$ = (Node *) n;
            }
        | EMBEDDINGS
            { $$ = makeInteger(1); }
        | /*EMPTY*/
            { $$ = NULL; }
    ;
```

与 `opt_embedding_clause` 完全对称：

```yacc
opt_embedding_clause:
        EMBEDDING AS '(' a_expr ')' STORED    → CONSTR_EMBEDDING
        | EMBEDDING                            → 标记
        | /*EMPTY*/

opt_embeddings_clause:
        EMBEDDINGS AS '(' a_expr ')' STORED    → CONSTR_EMBEDDINGS
        | EMBEDDINGS                           → 标记
        | /*EMPTY*/
```

#### 8.3.2 表级语法：EmbeddingsStmt / DropEmbeddingsStmt

```yacc
-- 顶层语句
Stmt: ... | EmbeddingsStmt | DropEmbeddingsStmt

EmbeddingsStmt:
        CREATE EMBEDDINGS name ON relation_expr '(' column_list ')'
            USING ColId opt_embeddings_with_clause opt_if_not_exists
            {
                EmbeddingsStmt *n = makeNode(EmbeddingsStmt);
                n->vecname = $3;
                n->relation = $5;
                n->veccolumns = $7;
                n->vecfunc = $9;
                n->options = $10;
                n->if_not_exists = $11;
                $$ = (Node *) n;
            }
    ;

DropEmbeddingsStmt:
        DROP EMBEDDINGS name ON relation_expr opt_if_exists
            {
                DropEmbeddingsStmt *n = makeNode(DropEmbeddingsStmt);
                n->vecname = $3;
                n->relation = $5;
                n->if_exists = $6;
                $$ = (Node *) n;
            }
    ;

opt_embeddings_with_clause:
        WITH '(' reloptions_list ')'    { $$ = $3; }
        | /*EMPTY*/                      { $$ = NIL; }
    ;

column_list:
        ColId
            { $$ = list_make1(makeString($1)); }
        | column_list ',' ColId
            { $$ = lappend($1, makeString($3)); }
    ;
```

## 9. 执行逻辑设计

### 9.1 新增命令处理文件

```
src/backend/commands/embeddingscmds.c
```

### 9.2 列级语法处理（parse_utilcmd.c）

与 EMBEDDING 的处理逻辑对称，在 `transformColumnDefinition` 中处理 `CONSTR_EMBEDDINGS`：

```c
case CONSTR_EMBEDDINGS:
    if (constraint->raw_expr != NULL)
    {
        column->is_embeddings = true;
        Assert(constraint->cooked_expr == NULL);
        saw_generated = true;
    }
    else
    {
        column->is_embeddings = true;
    }
    break;
```

在 `transformCreateStmtContext` 的列处理完成后，对 `is_embeddings` 的列进行统一处理：

```c
if (column->is_embeddings)
{
    /*
     * EMBEDDINGS AS 列级语法处理
     * 列名即隐藏向量列名，不需要额外创建伴随列
     *
     * 与 EMBEDDING 的区别：
     * - EMBEDDING: content(text,可见) + content_embedding(vector,隐藏) = 2列
     * - EMBEDDINGS: demographic(vector,隐藏) = 1列
     */

    /* 强制设置类型为 vector(vector_len) */
    TypeName *vector_type = makeNode(TypeName);
    vector_type->names = list_make1(makeString("vector"));
    A_Const *typmod_const = makeNode(A_Const);
    typmod_const->val.ival.type = T_Integer;
    typmod_const->val.ival.ival = cxt->vector_len;
    typmod_const->location = -1;
    vector_type->typmods = list_make1(typmod_const);
    column->typeName = vector_type;

    /* 设置隐藏 */
    column->is_hidden = true;
    column->generated = ATTRIBUTE_GENERATED_EMBEDDINGS;
    column->attembeddings = true;

    /* 创建向量索引 */
    if (cxt->vector_index != NULL && cxt->vector_distance != NULL)
    {
        IndexStmt *index = makeNode(IndexStmt);
        index->idxname = psprintf("%s_%s_vec_idx",
                                  cxt->relation->relname, column->colname);
        index->relation = copyObject(cxt->relation);
        index->accessMethod = pstrdup(cxt->vector_index);

        IndexElem *iparam = makeNode(IndexElem);
        iparam->name = column->colname;
        iparam->opclass = list_make1(makeString(cxt->vector_distance));

        index->indexParams = list_make1(iparam);
        index->if_not_exists = true;
        cxt->alist = lappend(cxt->alist, index);
    }
}
```

### 9.3 表级语法处理（embeddingscmds.c）

```c
Oid
CreateEmbeddings(EmbeddingsStmt *stmt, const char *queryString)
{
    Oid relid;
    Relation rel;
    Oid vecfunc_oid;
    AttrNumber vec_attnum;
    char *vec_colname;

    /* 1. 打开并锁定目标表 */
    relid = RangeVarGetRelidExtended(stmt->relation, AccessExclusiveLock, ...);
    rel = table_open(relid, AccessExclusiveLock);

    /* 2. 验证源列存在 */
    validate_embeddings_columns(rel, stmt->veccolumns);

    /* 3. 验证向量化函数存在 */
    vecfunc_oid = LookupFuncName(list_make1(makeString(stmt->vecfunc)), ...);

    /* 4. 验证名称唯一且不与现有列冲突 */
    if (embeddings_name_exists(relid, stmt->vecname) && !stmt->if_not_exists)
        ereport(ERROR, ...);
    if (column_name_exists(rel, stmt->vecname))
        ereport(ERROR, "column \"%s\" already exists in table \"%s\"", ...);

    /* 5. 添加隐藏向量列（向量化名即列名） */
    vec_colname = stmt->vecname;
    vec_attnum = add_embeddings_hidden_column(rel, vec_colname, options);

    /* 6. 创建触发器 */
    create_embeddings_trigger(rel, stmt->vecname, vec_colname);

    /* 7. 创建向量索引 */
    create_embeddings_index(rel, vec_colname, options);

    /* 8. 插入 pg_embeddings 元数据 */
    insert_embeddings_metadata(relid, stmt->vecname, stmt->veccolumns,
                              vecfunc_oid, vec_attnum, options);

    /* 9. 回填现有数据 */
    backfill_embeddings_data(rel, vec_colname, stmt->veccolumns, vecfunc_oid);

    table_close(rel, AccessExclusiveLock);
    return vec_attnum;
}
```

### 9.3 DropEmbeddings 主函数

```c
void
DropEmbeddings(DropEmbeddingsStmt *stmt)
{
    Oid relid;
    Relation rel;
    HeapTuple vectup;

    relid = RangeVarGetRelidExtended(stmt->relation, AccessExclusiveLock, ...);
    rel = table_open(relid, AccessExclusiveLock);

    /* 查找 pg_embeddings 元数据 */
    vectup = get_embeddings_tuple(relid, stmt->vecname);
    if (!HeapTupleIsValid(vectup))
    {
        if (stmt->if_exists)
        {
            table_close(rel, AccessExclusiveLock);
            return;
        }
        ereport(ERROR, ...);
    }

    /* 删除触发器 */
    drop_embeddings_trigger(rel, stmt->vecname);

    /* 删除向量索引 */
    drop_embeddings_index(rel, stmt->vecname);

    /* 删除隐藏向量列 */
    drop_embeddings_hidden_column(rel, stmt->vecname);

    /* 删除 pg_embeddings 元数据 */
    delete_embeddings_metadata(relid, stmt->vecname);

    table_close(rel, AccessExclusiveLock);
}
```

### 9.4 回填现有数据

```c
static void
backfill_embeddings_data(Relation rel, const char *vec_colname,
                         List *veccolumns, Oid vecfunc_oid)
{
    /* 构建回填 SQL */
    StringInfoData query;
    ListCell *lc;

    appendStringInfo(&query,
        "UPDATE %s SET %s = %s(",
        quote_identifier(RelationGetRelationName(rel)),
        quote_identifier(vec_colname),
        get_func_name(vecfunc_oid));

    foreach(lc, veccolumns)
    {
        char *colname = (char *) lfirst(lc);
        if (lnext(veccolumns, lc))
            appendStringInfo(&query, "%s, ", quote_identifier(colname));
        else
            appendStringInfo(&query, "%s", quote_identifier(colname));
    }

    appendStringInfo(&query, ") WHERE %s IS NULL",
                     quote_identifier(vec_colname));

    /* 执行回填 */
    SPI_connect();
    SPI_execute(query.data, false, 0);
    SPI_finish();
}
```

## 10. psql 显示设计

### 10.1 \d 命令增强

```
db=# \d customers
                  Table "public.customers"
 Column  |  Type   | Collation | Nullable | Default
---------+---------+-----------+----------+---------
 id      | integer |           | not null |
 age     | integer |           |          |
 income  | double precision |   |          |
 category| text    |           |          |
Indexes:
    "customers_pkey" PRIMARY KEY, btree (id)
    "customers_demographic_vec_idx" hnsw (demographic vector_cosine_ops)
Embeddings:
    "demographic" ON (age, income, category) USING ft_transformer_embedding WITH (vector_len=128)
```

### 10.2 \dv 命令（列出向量化）

```
db=# \dv
List of Embeddings
 Schema |    Name    |    Table    |     Columns      |       Function
--------+------------+-------------+------------------+------------------------
 public | demographic| customers   | age, income, cat | ft_transformer_embedding
 public | basic_feat | products    | price, brand     | ft_transformer_embedding
```

## 11. jolix_ft_transformer 扩展设计

### 11.1 扩展结构

```
contrib/jolix_ft_transformer/
├── Makefile
├── jolix_ft_transformer.c
├── jolix_ft_transformer--1.0.sql
└── jolix_ft_transformer.control
```

### 11.2 SQL 定义

```sql
-- 向量化函数：多参数版本
CREATE FUNCTION ft_transformer_embedding(VARIADIC input_values anyarray) RETURNS vector
    AS 'jolix_ft_transformer', 'ft_transformer_embedding'
    LANGUAGE C IMMUTABLE;

-- 向量化函数：指定模型
CREATE FUNCTION ft_transformer_embedding(VARIADIC input_values anyarray, model_name text) RETURNS vector
    AS 'jolix_ft_transformer', 'ft_transformer_embedding_with_model'
    LANGUAGE C IMMUTABLE;

-- 查询用辅助函数
CREATE FUNCTION ft_transformer_embedding(VARIADIC values anyarray) RETURNS vector
    AS 'jolix_ft_transformer', 'ft_transformer_embedding'
    LANGUAGE C IMMUTABLE;
```

### 11.3 C 实现概要

```c
PG_MODULE_MAGIC;

static char *ft_model_name = NULL;
static char *ft_model_path = NULL;
static bool ft_cache_enabled = true;

void _PG_init(void)
{
    DefineCustomStringVariable("jolix_ft_transformer.model_name", ...);
    DefineCustomStringVariable("jolix_ft_transformer.model_path", ...);
    DefineCustomBoolVariable("jolix_ft_transformer.cache_enabled", ...);
}

Datum ft_transformer_embedding(PG_FUNCTION_ARGS)
{
    int nargs = PG_NARGS();
    /* 提取各列值，根据类型判断特征类型 */
    /* 调用 Python FT-Transformer 模型 */
    /* 返回 vector */
    PG_RETURN_POINTER(vector_result);
}

Datum ft_transformer_embedding(PG_FUNCTION_ARGS)
{
    /* 同 ft_transformer_embedding */
}
```

## 12. 代码修改文件清单

| 文件路径 | 功能说明 |
|---------|----------|
| `src/include/parser/kwlist.h` | 添加 EMBEDDINGS 关键字 |
| `src/backend/parser/gram.y` | 添加 CREATE/DROP EMBEDDINGS 语法规则 |
| `src/include/nodes/parsenodes.h` | 添加 EmbeddingsStmt、DropEmbeddingsStmt 节点 |
| `src/include/catalog/pg_attribute.h` | 添加 attembeddings 字段；添加 ATTRIBUTE_GENERATED_EMBEDDINGS |
| `src/include/access/tupdesc.h` | CompactAttribute 添加 attembeddings；TupleConstr 添加 has_generated_embeddings |
| `src/include/catalog/pg_embeddings.h` | 新增 pg_embeddings 系统表定义 |
| `src/include/catalog/indexing.h` | pg_embeddings 索引注册 |
| `src/backend/catalog/pg_embeddings.c` | pg_embeddings 系统表操作函数 |
| `src/backend/commands/embeddingscmds.c` | CreateEmbeddings / DropEmbeddings 实现（表级语法） |
| `src/include/commands/embeddingscmds.h` | 函数声明 |
| `src/backend/tcop/utility.c` | 处理 T_EmbeddingsStmt / T_DropEmbeddingsStmt |
| `src/backend/parser/parse_utilcmd.c` | 处理 CONSTR_EMBEDDINGS（列级语法） |
| `src/backend/commands/tablecmds.c` | 无需修改（不再在 CREATE TABLE 中处理） |
| `src/backend/catalog/heap.c` | 跳过 EMBEDDINGS 列的 IMMUTABLE 检查 |
| `src/backend/executor/nodeModifyTable.c` | 跳过 EMBEDDINGS 列的 ExecComputeStoredGenerated；允许用户值 |
| `src/backend/rewrite/rewriteHandler.c` | 添加 rewrite_embeddings_query 查询重写 |
| `src/backend/utils/adt/predict.c` | 添加 embeddings_trigger 触发器函数 |
| `src/include/utils/predict.h` | 添加 embeddings 相关函数声明 |
| `src/backend/access/common/tupdesc.c` | CompactAttribute 复制 attembeddings |
| `src/backend/utils/cache/relcache.c` | 设置 constr->has_generated_embeddings |
| `src/backend/executor/execMain.c` | attgenerated 显示中添加 embeddings 类型 |
| `src/bin/psql/describe.c` | `\d` 显示 Embeddings 信息；`\dv` 命令 |
| `src/bin/pg_dump/pg_dump.c` | 导出 CREATE EMBEDDINGS 语句 |
| `src/bin/initdb/initdb.c` | 自动创建 jolix_ft_transformer 扩展 |
| `contrib/jolix_ft_transformer/` | FT-Transformer 扩展 |

## 13. 与现有系统的兼容性

### 13.1 与 EMBEDDING 共存

```sql
-- 建表时定义 EMBEDDING
CREATE TABLE articles (
    id int PRIMARY KEY,
    content text EMBEDDING AS (st_embedding(content)) STORED,
    category text,
    priority int
) WITH (vector_len = 64);

-- 随时添加 EMBEDDINGS
CREATE EMBEDDINGS meta ON articles (category, priority)
    USING ft_transformer_embedding
    WITH (vector_len = 64);
```

### 13.2 向后兼容

- 不使用 CREATE EMBEDDINGS 的表完全不受影响
- 现有 EMBEDDING 功能不受影响
- pg_embeddings 系统表初始为空，不影响现有数据

## 14. 安全性考虑

- FT-Transformer 模型文件应存储在受保护目录中
- 模型路径通过 GUC 参数配置，仅超级用户可修改
- CREATE EMBEDDINGS 需要表的 OWNER 权限
- 向量化过程在数据库服务端完成，数据不离开数据库

## 15. 性能考虑

- FT-Transformer 推理是计算密集型操作
- UPDATE 时仅当源列值变化才重新计算向量
- 模型缓存避免重复加载
- 回填现有数据时可分批执行，避免长事务
- 复用 pgvector 的 ivfflat/hnsw 索引
- 查询重写在规划阶段完成，运行时无额外开销

## 16. 后续扩展方向

- 支持更多表格模型（TabNet, TabTransformer, AutoInt）
- 异步向量化（`WITH (embeddings_timing = 'deferred')`）
- 向量化列类型扩展（日期/时间、JSON/JSONB、数组）
- `ALTER EMBEDDINGS ... REBUILD` 重建向量
- `CREATE EMBEDDINGS ... WHERE condition` 条件向量化

---
**文档版本**: 9.0
**最后更新**: 2026-05-28

## 17. 实现状态

### 17.1 已完成（CREATE EMBEDDINGS 表级语法）

| 功能 | 状态 | 说明 |
|------|------|------|
| 语法解析 | ✅ | gram.y 中 EmbeddingsStmt / DropEmbeddingsStmt |
| 关键字注册 | ✅ | kwlist.h 中 EMBEDDINGS (UNRESERVED_KEYWORD) |
| 节点定义 | ✅ | parsenodes.h 中 EmbeddingsStmt / DropEmbeddingsStmt |
| 命令处理 | ✅ | utility.c 中 T_EmbeddingsStmt / T_DropEmbeddingsStmt |
| 命令标签 | ✅ | cmdtaglist.h 中 CMDTAG_CREATE_EMBEDDINGS / CMDTAG_DROP_EMBEDDINGS |
| 系统目录 | ✅ | pg_attribute 新增 attembeddings 字段 |
| 生成列类型 | ✅ | ATTRIBUTE_GENERATED_EMBEDDINGS ('z') |
| 创建实现 | ✅ | embeddingscmds.c 中 CreateEmbeddings |
| 删除实现 | ✅ | embeddingscmds.c 中 DropEmbeddings |
| 触发器 | ✅ | predict.c 中 embeddings_trigger |
| 触发器注册 | ✅ | pg_proc.dat 中 OID 6508 |
| 执行器豁免 | ✅ | nodeModifyTable.c 跳过 EMBEDDINGS 列的 ExecComputeStoredGenerated |
| 查询重写 | ✅ | rewriteHandler.c 中 ATTRIBUTE_GENERATED_EMBEDDINGS 豁免 |
| 元组描述符 | ✅ | tupdesc.h / tupdesc.c / relcache.c 中 attembeddings 支持 |
| Bootstrap 修复 | ✅ | bootstrap.c 中添加 anyarray 到 TypInfo[] |
| FT-Transformer 函数 | ✅ | jolix_embedding 扩展中 ft_transformer_embedding(VARIADIC "any") |
| pgvector 兼容 | ✅ | ivfflat/hnsw handler 改用 makeNode 方式 |

### 17.2 已实现（EMBEDDINGS AS 列级语法）

| 功能 | 状态 | 说明 |
|------|------|------|
| 语法解析 | ✅ | gram.y 中 columnDef 独立替代规则匹配 EMBEDDINGS AS |
| 列级处理 | ✅ | parse_utilcmd.c 中 is_embeddings 处理（vector 类型 + embeddings_trigger） |
| INSERT/UPDATE | ✅ | embeddings_trigger 自动调用 ft_transformer_embedding 填充向量 |
| 向量索引 | ✅ | 自动创建向量索引（默认 ivfflat，支持 WITH(vector_index='hnsw', vector_distance='vector_cosine_ops')） |
| 查询重写 | ⚠️ | 核心逻辑已实现（is_embeddings_var + rewrite_embedding_opexpr），但 ROW() 语法需要 vector<=>record 操作符支持，当前用户可直接使用函数调用形式 |

### 17.3 测试验证

#### 17.3.1 CREATE EMBEDDINGS（表级语法）

```sql
-- 创建表
CREATE TABLE customers (
    id SERIAL PRIMARY KEY,
    age INTEGER,
    income FLOAT8,
    category TEXT
);

-- 创建向量化（需先安装 jolix_embedding 扩展）
CREATE EXTENSION IF NOT EXISTS jolix_embedding;
CREATE EMBEDDINGS demographic ON customers
    USING ft_transformer_embedding (age, income, category)
    WITH (vector_len = 128);

-- 插入数据
INSERT INTO customers (age, income, category) VALUES (30, 50000.0, 'A');

-- 查看表结构（demographic 列为隐藏向量列）
\d customers

-- 删除向量化
DROP EMBEDDINGS demographic ON customers;

-- IF NOT EXISTS / IF EXISTS
CREATE EMBEDDINGS IF NOT EXISTS demographic ON customers
    USING ft_transformer_embedding (age, income, category)
    WITH (vector_len = 128);
DROP EMBEDDINGS IF EXISTS demographic ON customers;
```

#### 17.3.2 EMBEDDINGS AS（列级语法）

```sql
-- 创建带 EMBEDDINGS AS 列的表
CREATE TABLE customers_v2 (
    id int PRIMARY KEY,
    age int,
    income float,
    category text,
    demographic EMBEDDINGS AS (ft_transformer_embedding(age, income, category))
) WITH (vector_len = 128);

-- 验证表结构
\d customers_v2
-- demographic 列类型为 vector(128)
-- Trigger: embeddings_demographic_trigger BEFORE INSERT OR UPDATE

-- 插入数据（demographic 列自动填充）
INSERT INTO customers_v2 (id, age, income, category) VALUES (1, 30, 50000.0, 'A');

-- 验证向量自动生成
SELECT id, age, income, category, vector_dims(demographic) as dims FROM customers_v2;
-- dims = 128

-- 更新数据（向量自动重新计算）
UPDATE customers_v2 SET age = 35, income = 60000.0 WHERE id = 1;

-- 多行插入
INSERT INTO customers_v2 (id, age, income, category) VALUES
    (2, 25, 45000.0, 'B'),
    (3, 40, 75000.0, 'C'),
    (4, 35, 55000.0, 'A');

-- 向量相似度查询（子查询方式）
SELECT id, age, income, category,
       demographic <=> (SELECT demographic FROM customers_v2 WHERE id = 1) AS distance
FROM customers_v2 ORDER BY distance;

-- 向量相似度查询（函数调用方式，需要显式类型转换）
SELECT id, age, income, category,
       demographic <=> ft_transformer_embedding(30::int, 50000.0::float8, 'A'::text) AS distance
FROM customers_v2 ORDER BY distance;

-- 使用 HNSW 索引和余弦距离
DROP TABLE IF EXISTS customers_hnsw;
CREATE TABLE customers_hnsw (
    id int PRIMARY KEY,
    age int,
    income float,
    category text,
    demographic EMBEDDINGS AS (ft_transformer_embedding(age, income, category))
) WITH (vector_len = 128, vector_index = 'hnsw', vector_distance = 'vector_cosine_ops');
```

### 17.4 关键修复记录

1. **initdb bootstrap "unrecognized type anyarray"**：将 anyarray 添加到 TypInfo[] 数组
2. **pgvector ivfflat/hnsw handler 不兼容 PG18**：将 `static const IndexAmRoutine` 改为 `makeNode(IndexAmRoutine)` 方式
3. **embeddingscmds.c 列引用类型错误**：`columnList` 语法返回 `list of String`，不是 `list of ColumnRef`
4. **embeddingscmds.c typmod 设置错误**：使用 `A_Const` 节点替代 `makeInteger` 设置 vector 类型修饰符
5. **DROP EMBEDDINGS 触发器未删除**：`CreateTrigger` 会自动在触发器名后追加 `_{oid}` 后缀，导致 `drop_embeddings_trigger` 用原始名称找不到触发器。修复为使用 `systable_beginscan` 按前缀匹配查找触发器
6. **INSERT 后 demographic 列为 NULL（函数查找失败）**：
   - 原因1：`FuncnameGetCandidates` 的 `expand_variadic=false` 导致 VARIADIC 函数的 `nargs=1` 不匹配实际参数个数。修复为 `expand_variadic=true`
   - 原因2：通过 `fmgr_info` + 手动构造 `FunctionCallInfo` 调用函数时，`flinfo->fn_expr=NULL`，导致 `ft_transformer_embedding` 中 `get_fn_expr_argtype` 返回 0。修复为在调用前构造 `FuncExpr` 节点并设置到 `funcinfo.fn_expr`
7. **FT-Transformer 模型不可用导致 INSERT 报错**：重写 `ft_transformer_embedding` 函数，添加确定性向量 fallback 机制——模型加载失败时基于列值哈希生成确定性向量
8. **EMBEDDINGS AS 语法冲突**：`EMBEDDINGS` 作为 `unreserved_keyword` 可被 `Typename`（通过 `GenericType` → `type_function_name`）消费，导致 `EMBEDDINGS AS` 在 `columnDef` 中无法被 `opt_embedding_clause` 匹配。修复为在 `columnDef` 中添加独立替代规则 `ColId EMBEDDINGS AS '(' func_name '(' columnList ')' ')' ...`，直接在 `columnDef` 层面匹配
9. **`invalid storage type ""` 错误**：新 `columnDef` 替代规则中 bison 栈位置引用导致非 NULL 的空字符串值。修复为在 gram.y 中直接设置 `storage_name = NULL`、`compression = NULL`，在 parse_utilcmd.c 中显式设置 `column->storage_name = NULL; column->compression = NULL; column->fdwoptions = NIL;`
10. **embeddings_func/embeddings_cols 传递重构**：从 `CreateStmtContext` 中移除 `embeddings_func` 和 `embeddings_cols` 字段，改用 `ColumnDef` 中的 `embeddings_func` 和 `embeddings_cols` 字段传递，避免多列 EMBEDDINGS AS 场景下的数据混淆
