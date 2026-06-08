# EMBEDDING / EMBEDDINGS 使用文档

**最后更新**: 2026-06-02

## 1. 概述

Jolix PostgreSQL 提供两种向量嵌入语法，满足不同场景需求：

| 语法 | 关键字 | 输入 | 适用场景 | 类比 |
|------|--------|------|---------|------|
| **EMBEDDING** | `col text EMBEDDING AS (func(col))` | 单列文本自身 | 文本→向量（语义搜索） | 列约束 |
| **EMBEDDINGS** | `name EMBEDDINGS AS (func(col1, col2, ...))` | 多列值组合 | 表格数据→向量（相似性查询） | 列约束 |

**两者共享**：
- 相同的触发器机制（`embeddings_trigger`）
- 相同的向量索引（ivfflat/hnsw）
- 相同的 GUC 参数（`jolix_embedding.*`）
- 相同的降级策略（模型不可用时使用确定性哈希向量）

**核心区别**：

| 特性 | EMBEDDING | EMBEDDINGS |
|------|-----------|------------|
| 输入 | 单列自身值 | 多列值组合 |
| 隐藏列 | `{col}_embedding`（伴随列） | `name`（直接命名） |
| 原始列 | 可见 text 列 | 无（直接是 vector 隐藏列） |
| 查询 | `col <=> 'text'`（自动重写） | `name <=> func(v1,v2)`（函数调用） |
| 查询重写 | 3步（换列+换操作符+转换右操作数） | 1步（仅转换右操作数） |

## 2. 环境要求

- PostgreSQL 自定义版本（支持 EMBEDDING / EMBEDDINGS 功能）
- pgvector 扩展（向量类型和索引支持，initdb 时自动安装）
- jolix_embedding 扩展（内置 Sentence Transformers 函数，initdb 时自动安装）
- Python 3.8+ 和 sentence-transformers 库（如使用内置 st_embedding / ft_transformer_embedding 函数）

## 3. 快速开始

### 3.1 EMBEDDING：文本语义搜索

```sql
-- 创建表：content 列自动生成向量嵌入
CREATE TABLE articles (
    id SERIAL PRIMARY KEY,
    content text EMBEDDING AS (st_embedding(content))
) WITH (vector_len = 384);

-- 插入数据（向量自动生成）
INSERT INTO articles (content) VALUES ('machine learning basics');
INSERT INTO articles (content) VALUES ('database system design');

-- 语义搜索（查询文本自动转换为向量）
SELECT id, content, content <=> 'artificial intelligence' AS distance
FROM articles
ORDER BY distance;
```

### 3.2 EMBEDDINGS：多列组合向量化

```sql
-- 创建表：多列组合生成向量
CREATE TABLE customers (
    id int PRIMARY KEY,
    age int,
    income float,
    category text,
    demographic EMBEDDINGS AS (ft_transformer_embedding(age, income, category))
);

-- 插入数据（向量自动生成）
INSERT INTO customers (id, age, income, category) VALUES (1, 25, 30000.0, 'basic');
INSERT INTO customers (id, age, income, category) VALUES (2, 35, 50000.0, 'premium');

-- 相似性查询（子查询方式）
SELECT id, age, income, category,
       demographic <=> (SELECT demographic FROM customers WHERE id = 1) AS cosine_dist
FROM customers WHERE id != 1 ORDER BY cosine_dist;

-- 相似性查询（函数调用方式，需要显式类型转换）
SELECT id, age, income, category,
       demographic <=> ft_transformer_embedding(30::int, 50000.0::float8, 'premium'::text) AS query_dist
FROM customers ORDER BY query_dist LIMIT 3;
```

## 4. EMBEDDING 列使用

### 4.1 语法

```sql
column_name type EMBEDDING AS (embedding_expression)
```

- `column_name`：列名，存储原始文本值（可见列）
- `type`：列数据类型（通常为 text）
- `EMBEDDING AS`：关键字
- `embedding_expression`：嵌入表达式，引用列自身值
- `STORED`：可选关键字（默认行为），表示物理存储

建表时自动创建：
- **可见列** `column_name`：存储原始文本值
- **隐藏列** `{column_name}_embedding`：存储向量嵌入值，类型为 `vector(vector_len)`
- **触发器** `jolix_predict_{column}_{oid}`：BEFORE INSERT OR UPDATE，自动调用嵌入函数
- **向量索引** `{table}_{column}_embedding_idx`：在 `_embedding` 列上创建向量索引

### 4.2 创建表

```sql
-- 使用内置 st_embedding 函数（推荐）
CREATE TABLE articles (
    id SERIAL PRIMARY KEY,
    content text EMBEDDING AS (st_embedding(content))
) WITH (vector_len = 384);

-- 使用指定模型
CREATE TABLE articles (
    id SERIAL PRIMARY KEY,
    content text EMBEDDING AS (st_embedding(content, 'BAAI/bge-small-en-v1.5'))
) WITH (vector_len = 384);
```

### 4.3 插入与查询

```sql
-- 插入数据（向量自动生成到隐藏列）
INSERT INTO articles (content) VALUES ('machine learning basics');
INSERT INTO articles (content) VALUES ('database system design');

-- SELECT * 不显示隐藏列
SELECT * FROM articles;

-- 显式查询隐藏列
SELECT id, content, content_embedding FROM articles;

-- 语义搜索（自动重写：content <=> 'text' → content_embedding <=> st_embedding('text')::vector）
SELECT id, content, content <=> 'artificial intelligence' AS distance
FROM articles
ORDER BY distance
LIMIT 10;

-- 余弦距离搜索
SELECT id, content, content <=> 'search query' AS distance
FROM articles
WHERE content <=> 'search query' < 1.0
ORDER BY content <=> 'search query'
LIMIT 10;
```

### 4.4 查询重写机制

当 EMBEDDING 列出现在向量距离操作符中时，查询自动重写（3步）：

```
用户写的:                          content <=> 'search text'
  │
  ├── 1. 换列:    content → content_embedding
  ├── 2. 换操作符: text版 <=> → vector版 <=>
  └── 3. 转换右操作数: 'search text' → st_embedding('search text')::vector
  │
重写后:  content_embedding <=> st_embedding('search text')::vector
```

验证重写：

```sql
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM articles WHERE content <=> 'search' < 1.0;
```

### 4.5 多个 EMBEDDING 列

```sql
CREATE TABLE documents (
    id int PRIMARY KEY,
    title text EMBEDDING AS (st_embedding(title)),
    content text EMBEDDING AS (st_embedding(content))
) WITH (vector_len = 384);
```

### 4.6 ALTER TABLE 添加 EMBEDDING 列

```sql
ALTER TABLE my_table ADD COLUMN embedding_col text EMBEDDING AS (st_embedding(content));
```

自动创建 `_embedding` 伴随列、触发器和向量索引。

## 5. EMBEDDINGS 列使用

### 5.1 语法

```sql
name EMBEDDINGS AS (embedding_expression)
```

- `name`：向量化名称，同时作为隐藏向量列的列名
- `EMBEDDINGS AS`：关键字
- `embedding_expression`：向量化表达式，可引用同表其他列
- `STORED`：可选关键字（默认行为），表示物理存储

建表时自动创建：
- **隐藏列** `name`：直接使用向量化名，类型为 `vector(vector_len)`（`atthidden=true, attembeddings=true`）
- **触发器** `embeddings_{name}_trigger`：BEFORE INSERT OR UPDATE，自动调用嵌入函数
- **向量索引** `{table}_{name}_vec_idx`：在向量列上创建索引

### 5.2 列级语法：EMBEDDINGS AS

```sql
-- 基本用法
CREATE TABLE customers (
    id int PRIMARY KEY,
    age int,
    income float,
    category text,
    demographic EMBEDDINGS AS (ft_transformer_embedding(age, income, category))
);

-- 指定向量维度（需与模型输出维度匹配）
CREATE TABLE customers (
    id int PRIMARY KEY,
    age int,
    income float,
    category text,
    demographic EMBEDDINGS AS (ft_transformer_embedding(age, income, category))
) WITH (vector_len = 384);

-- 多组 EMBEDDINGS
CREATE TABLE products (
    id int PRIMARY KEY,
    name text,
    price float,
    brand text,
    basic_features EMBEDDINGS AS (ft_transformer_embedding(price, brand)),
    sales int,
    rating float,
    performance EMBEDDINGS AS (ft_transformer_embedding(sales, rating))
) WITH (vector_len = 384);

-- EMBEDDING 与 EMBEDDINGS 共存
CREATE TABLE articles (
    id int PRIMARY KEY,
    content text EMBEDDING AS (st_embedding(content)),
    category text,
    priority int,
    meta EMBEDDINGS AS (ft_transformer_embedding(category, priority))
) WITH (vector_len = 384);
```

### 5.3 表级语法：CREATE EMBEDDINGS

```sql
-- 先建表
CREATE TABLE customers (
    id SERIAL PRIMARY KEY,
    age INTEGER,
    income FLOAT8,
    category TEXT
);

-- 添加向量化
CREATE EMBEDDINGS demographic ON customers
    USING ft_transformer_embedding (age, income, category)
    WITH (vector_len = 384);

-- 删除向量化
DROP EMBEDDINGS demographic ON customers;

-- IF NOT EXISTS / IF EXISTS
CREATE EMBEDDINGS IF NOT EXISTS demographic ON customers
    USING ft_transformer_embedding (age, income, category)
    WITH (vector_len = 384);

DROP EMBEDDINGS IF EXISTS demographic ON customers;
```

### 5.4 向量相似性查询

```sql
-- 方式一：子查询（查找与某行最相似的行）
SELECT id, age, income, category,
       demographic <=> (SELECT demographic FROM customers WHERE id = 1) AS cosine_dist
FROM customers
WHERE id != 1
ORDER BY cosine_dist;

-- 方式二：函数调用（实时计算查询向量，需要显式类型转换）
SELECT id, age, income, category,
       demographic <=> ft_transformer_embedding(30::int, 50000.0::float8, 'premium'::text) AS query_dist
FROM customers
ORDER BY query_dist
LIMIT 3;

-- 方式三：向量字面量（预先计算好向量）
SELECT id, age, income, category,
       demographic <=> '[0.1, 0.2, ..., 0.384]'::vector AS distance
FROM customers
ORDER BY distance
LIMIT 10;
```

### 5.5 EMBEDDINGS 查询重写机制

EMBEDDINGS 的查询重写比 EMBEDDING 更简单（1步），因为列名本身就是 vector 类型：

```
用户写的:                          demographic <=> ROW(35, 50000.0, 'premium')
  │
  └── 1. 转换右操作数: ROW(v1,v2) → ft_transformer_embedding(v1,v2)
  │
重写后:  demographic <=> ft_transformer_embedding(35, 50000.0, 'premium')
```

> 注意：`ROW(...)` 语法的查询重写需要 `vector <=> record` 操作符支持，当前建议使用函数调用形式。

## 6. 内置 Embedding 函数

jolix_embedding 扩展提供了基于 Sentence Transformers 的内置嵌入函数，无需手动创建 PL/Python 函数即可使用。

### 6.1 st_embedding 函数（单列文本嵌入）

使用默认模型生成文本嵌入向量，返回 `vector` 类型。

```sql
-- 单参数版本（使用 GUC 默认模型）
st_embedding(input_text text) RETURNS vector

-- 两参数版本（指定模型）
st_embedding(input_text text, model_name text) RETURNS vector

-- 示例
SELECT st_embedding('hello world');
-- 返回: [0.056, -0.023, 0.089, ...]（384维向量）

SELECT st_embedding('hello world', 'BAAI/bge-small-en-v1.5');
-- 返回: 384维向量
```

**参数说明**：

| 参数 | 类型 | 说明 |
|------|------|------|
| `input_text` | text | 输入文本，NULL 时返回 NULL |
| `model_name` | text | 模型名称（可选），NULL 时使用 GUC 默认模型 |

**用途**：EMBEDDING 列表达式（单列文本→向量）

### 6.2 ft_transformer_embedding 函数（多列组合嵌入）

接受可变参数（VARIADIC "any"），将多列值拼接后通过 SentenceTransformer 模型生成向量。

```sql
ft_transformer_embedding(VARIADIC "any") RETURNS vector

-- 示例
SELECT ft_transformer_embedding(30, 50000.0, 'A'::text);
-- 返回: 384维向量

-- 在 EMBEDDINGS 列中使用
CREATE TABLE customers (
    id int, age int, income float, category text,
    demographic EMBEDDINGS AS (ft_transformer_embedding(age, income, category))
);
```

**参数说明**：

| 参数 | 类型 | 说明 |
|------|------|------|
| 可变参数 | any | 多列值，支持 int/float/text 等类型 |

**工作原理**：将所有列值拼接为字符串（如 `"30, 50000.000000, A"`），然后调用 `SentenceTransformer.encode()` 生成向量。

**降级机制**：当模型加载失败时（如网络不可达），函数自动降级为基于列值哈希的确定性向量生成，不会报错。降级向量维度由 `jolix_embedding.ft_vector_len` 控制。

**用途**：EMBEDDINGS 列表达式（多列组合→向量）

### 6.3 函数对比

| 函数 | 返回类型 | 输入 | 模型指定 | 用途 |
|------|---------|------|---------|------|
| `st_embedding(text)` | vector | 单列文本 | GUC 默认模型 | EMBEDDING 列（推荐） |
| `st_embedding(text, text)` | vector | 单列文本 | 参数指定模型 | 多模型场景 |
| `ft_transformer_embedding(VARIADIC "any")` | vector | 多列值 | GUC 默认模型 | EMBEDDINGS 列（推荐） |

### 6.4 常用模型参考

| 模型名称 | 维度 | 大小 | 说明 |
|---------|------|------|------|
| `sentence-transformers/all-MiniLM-L6-v2` | 384 | 80MB | **默认模型**，速度快，适合通用场景 |
| `sentence-transformers/all-mpnet-base-v2` | 768 | 420MB | 高质量，适合精度要求高的场景 |
| `BAAI/bge-small-en-v1.5` | 384 | 130MB | BGE 小模型，英文 |
| `BAAI/bge-base-en-v1.5` | 768 | 420MB | BGE 基础模型，英文 |
| `BAAI/bge-large-en-v1.5` | 1024 | 1.2GB | BGE 大模型，英文 |
| `shibing624/text2vec-base-chinese` | 768 | 400MB | 中文文本向量化 |

**选择建议**：
- 通用场景：`all-MiniLM-L6-v2`（默认，384维，速度快）
- 高精度场景：`all-mpnet-base-v2`（768维，质量高）
- 中文场景：`text2vec-base-chinese`（768维，中文优化）

### 6.5 模型自动下载

首次调用嵌入函数时，如果本地不存在指定模型，系统会自动从 HuggingFace 镜像下载：

- 默认镜像：`https://hf-mirror.com`（国内加速）
- 存储路径：`/usr/local/pgsql/models/`（可通过 `jolix_embedding.model_path` 修改）
- 下载后缓存，后续调用直接使用本地文件

### 6.6 会话级模型缓存

模型在数据库会话中缓存：
- 同一会话中首次调用加载模型到内存
- 后续调用直接使用缓存的模型实例
- 不同会话各自独立缓存
- 连接断开后缓存释放

## 7. 支持的距离操作符

| 操作符 | 名称 | 说明 |
|--------|------|------|
| `<->` | L2距离 | 欧几里得距离，最常用 |
| `<=>` | 余弦距离 | 适合文本相似度 |
| `<#>` | 内积 | 负内积，适合归一化向量 |
| `<+>` | L1距离 | 曼哈顿距离 |

## 8. WITH 参数完整参考

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `vector_len` | int | 384 | 嵌入向量维度（需与模型输出匹配） |
| `vector_index` | enum | hnsw | 向量索引类型：ivfflat 或 hnsw |
| `vector_distance` | enum | vector_cosine_ops | 向量距离类型：vector_l2_ops, vector_cosine_ops, vector_ip_ops |
| `lists` | int | - | ivfflat 索引的 lists 参数 |
| `m` | int | - | hnsw 索引的 m 参数 |
| `ef_construction` | int | - | hnsw 索引的 ef_construction 参数 |

## 9. GUC 参数

### 9.1 jolix_embedding 参数

| 参数 | 类型 | 默认值 | 作用域 | 说明 |
|------|------|--------|--------|------|
| `jolix_embedding.model_name` | string | `sentence-transformers/all-MiniLM-L6-v2` | USERSET | st_embedding 默认模型 |
| `jolix_embedding.model_path` | string | `/usr/local/pgsql/models` | SIGHUP | 模型缓存目录 |
| `jolix_embedding.ft_model_name` | string | `sentence-transformers/all-MiniLM-L6-v2` | USERSET | ft_transformer_embedding 默认模型 |
| `jolix_embedding.ft_vector_len` | integer | `384` | USERSET | 模型不可用时的降级向量维度 |

```sql
-- 修改默认模型
SET jolix_embedding.model_name = 'BAAI/bge-small-en-v1.5';

-- 修改 ft_transformer_embedding 默认模型
SET jolix_embedding.ft_model_name = 'sentence-transformers/all-mpnet-base-v2';

-- 修改模型存储路径（需重启生效）
ALTER SYSTEM SET jolix_embedding.model_path = '/data/models';
SELECT pg_reload_conf();
```

**切换模型时注意**：不同模型输出不同维度的向量，`vector_len` 必须与模型输出维度一致。

## 10. 隐藏列机制

### 10.1 EMBEDDING 的隐藏列

| 列 | 类型 | 说明 |
|----|------|------|
| `{column}` | text | EMBEDDING 列，存储原始文本值（可见） |
| `{column}_embedding` | vector(N) | 隐藏列，存储嵌入向量 |

- `SELECT *` 不显示 `_embedding` 隐藏列
- 显式引用隐藏列可以查询：`SELECT content_embedding FROM articles`

### 10.2 EMBEDDINGS 的隐藏列

| 列 | 类型 | 说明 |
|----|------|------|
| `name` | vector(N) | 直接使用向量化名，隐藏列 |

- `SELECT *` 不显示隐藏列
- 显式引用可以查询：`SELECT demographic FROM customers`
- 列本身就是 vector 类型，无需额外伴随列

## 11. 示例场景

### 11.1 文档语义搜索（EMBEDDING）

```sql
CREATE TABLE documents (
    id SERIAL PRIMARY KEY,
    title TEXT,
    body TEXT EMBEDDING AS (st_embedding(body))
) WITH (vector_len = 384);

INSERT INTO documents (title, body) VALUES ('ML Guide', 'machine learning basics and applications');
INSERT INTO documents (title, body) VALUES ('DB Design', 'database normalization and indexing');

-- 搜索相关文档
SELECT title, body, body <=> 'artificial intelligence' AS distance
FROM documents
ORDER BY distance
LIMIT 10;
```

### 11.2 问答系统（EMBEDDING）

```sql
CREATE TABLE qa_pairs (
    id SERIAL PRIMARY KEY,
    question TEXT EMBEDDING AS (st_embedding(question)),
    answer TEXT
) WITH (vector_len = 384);

-- 找到最相似的问题
SELECT question, answer, question <=> 'user query here' AS distance
FROM qa_pairs
ORDER BY distance
LIMIT 1;
```

### 11.3 客户分群（EMBEDDINGS）

```sql
CREATE TABLE customers (
    id int PRIMARY KEY,
    age int,
    income float,
    category text,
    demographic EMBEDDINGS AS (ft_transformer_embedding(age, income, category))
);

INSERT INTO customers (id, age, income, category) VALUES
    (1, 25, 30000.0, 'basic'),
    (2, 35, 50000.0, 'premium'),
    (3, 45, 80000.0, 'premium'),
    (4, 25, 35000.0, 'basic'),
    (5, 55, 120000.0, 'vip');

-- 查找与客户1最相似的客户
SELECT id, age, income, category,
       demographic <=> (SELECT demographic FROM customers WHERE id = 1) AS cosine_dist
FROM customers WHERE id != 1 ORDER BY cosine_dist;
-- 结果：客户4（25岁, 35000, basic）距离最近（0.061），语义最相似
```

### 11.4 推荐系统（EMBEDDING + EMBEDDINGS 共存）

```sql
CREATE TABLE products (
    id SERIAL PRIMARY KEY,
    name TEXT,
    description TEXT EMBEDDING AS (st_embedding(description)),
    price FLOAT,
    rating FLOAT,
    features EMBEDDINGS AS (ft_transformer_embedding(price, rating))
) WITH (vector_len = 384);

-- 文本语义搜索
SELECT name, description, description <=> 'wireless headphones' AS text_dist
FROM products ORDER BY text_dist LIMIT 5;

-- 特征相似性搜索
SELECT name, price, rating,
       features <=> (SELECT features FROM products WHERE id = 1) AS feature_dist
FROM products WHERE id != 1 ORDER BY feature_dist LIMIT 5;
```

### 11.5 RAG 知识库（EMBEDDING + PREDICT）

```sql
-- 配置 LLM
SELECT set_llm_config(
    p_api_url := 'https://api.openai.com/v1/chat/completions',
    p_api_key := 'sk-xxx',
    p_model_name := 'gpt-4',
    p_rag_similarity := 2.0,
    p_rag_topn := 3
);

-- 创建 RAG 表
CREATE TABLE rag_knowledge (
    id serial PRIMARY KEY,
    content text EMBEDDING AS (st_embedding(content)),
    answer text PREDICT AS (llm_rag_infer(
        'Answer questions based on the provided context. Reply in one short sentence.',
        content
    ))
) WITH (predict_timing = immediate, vector_len = 384);

-- 插入知识数据
INSERT INTO rag_knowledge (content) VALUES ('PostgreSQL is a powerful open source database system');
INSERT INTO rag_knowledge (content) VALUES ('Python is a popular programming language');

-- 插入问题（自动触发 RAG 推理）
INSERT INTO rag_knowledge (content) VALUES ('What is PostgreSQL?');
-- answer 列自动填充 LLM 回答
```

## 12. 配合向量索引

```sql
-- 使用 ivfflat 索引
CREATE TABLE articles_ivfflat (
    id int PRIMARY KEY,
    content text EMBEDDING AS (st_embedding(content))
) WITH (vector_len = 384, vector_index = ivfflat, lists = 100);

-- 使用 HNSW 索引（推荐，更高性能）
CREATE TABLE articles_hnsw (
    id int PRIMARY KEY,
    content text EMBEDDING AS (st_embedding(content))
) WITH (vector_len = 384, vector_index = hnsw, vector_distance = vector_cosine_ops, m = 16, ef_construction = 64);

-- EMBEDDINGS 使用 HNSW 索引
CREATE TABLE customers (
    id int PRIMARY KEY,
    age int,
    income float,
    category text,
    demographic EMBEDDINGS AS (ft_transformer_embedding(age, income, category))
) WITH (vector_len = 384, vector_index = 'hnsw', vector_distance = 'vector_cosine_ops');
```

## 13. 故障排除

### 问题：operator does not exist: text <-> text

**原因**：EMBEDDING 列的距离操作符重写未生效，或扩展未安装到 public schema

**解决**：
```sql
-- 检查扩展安装位置
SELECT extname, extnamespace::regnamespace FROM pg_extension;

-- 确保扩展在 public schema
DROP EXTENSION jolix_embedding;
CREATE EXTENSION jolix_embedding SCHEMA public;
```

### 问题：向量维度不匹配

**原因**：`vector_len` 与模型输出维度不一致

**解决**：
| 模型 | vector_len |
|------|-----------|
| all-MiniLM-L6-v2 | 384 |
| all-mpnet-base-v2 | 768 |
| bge-small-en-v1.5 | 384 |
| bge-base-en-v1.5 | 768 |
| bge-large-en-v1.5 | 1024 |
| text2vec-base-chinese | 768 |

### 问题：st_embedding 报错 "could not import sentence_transformers module"

**原因**：Python 环境中未安装 sentence-transformers 库

**解决**：
```bash
pip install sentence-transformers
```

### 问题：st_embedding 报错 "type vector does not exist"

**原因**：pgvector 扩展未安装

**解决**：
```sql
CREATE EXTENSION IF NOT EXISTS vector SCHEMA public;
```

### 问题：embedding 列为空

确保建表时使用 `EMBEDDING AS` 语法指定嵌入函数：

```sql
CREATE TABLE my_table (
    content text EMBEDDING AS (st_embedding(content))
) WITH (vector_len = 384);
```

### 问题：ft_transformer_embedding 向量维度不匹配

`vector_len` 必须与模型输出维度匹配：
- `all-MiniLM-L6-v2`：384 维（默认）
- `all-mpnet-base-v2`：768 维

```sql
-- 使用默认模型（384维）
CREATE TABLE t1 (...,
    vec EMBEDDINGS AS (ft_transformer_embedding(col1, col2))
);  -- vector_len 默认 384

-- 使用 768 维模型
SET jolix_embedding.ft_model_name = 'sentence-transformers/all-mpnet-base-v2';
CREATE TABLE t2 (...,
    vec EMBEDDINGS AS (ft_transformer_embedding(col1, col2))
) WITH (vector_len = 768);
```

### 问题：模型下载缓慢或失败

**原因**：HuggingFace 网络连接问题

**解决**：
```bash
# 手动下载模型到本地
pip install huggingface-hub
huggingface-cli download --local-dir /usr/local/pgsql/models/sentence-transformers_all-MiniLM-L6-v2 \
    sentence-transformers/all-MiniLM-L6-v2

# 或设置代理
export HTTPS_PROXY=http://your-proxy:port
```

### 问题：如何从 Windows 客户端连接 WSL 中的 PostgreSQL？

1. 确保 `postgresql.conf` 中 `listen_addresses = '*'`
2. 确保 `pg_hba.conf` 中添加了 `host all all 0.0.0.0/0 md5`
3. 设置 postgres 用户密码：`ALTER USER postgres WITH PASSWORD 'PG';`
4. 获取 WSL IP：`wsl -d Ubuntu-22.04 -- bash -c "hostname -I"`
5. 使用该 IP 和端口 5432 连接

---
**文档版本**: 2.0
**最后更新**: 2026-06-02
