# EMBEDDING 功能使用文档

## 环境要求

- PostgreSQL 自定义版本（支持EMBEDDING功能）
- pgvector 扩展（向量类型和索引支持）
- jolix_embedding 扩展（内置 Sentence Transformers 函数，initdb 时自动安装）
- Python 3.8+ 和 sentence-transformers 库（如使用内置 st_embedding 函数）
- PL/Python3 扩展（如使用自定义 Python embedding 函数）

## 快速开始

### 1. 创建 Embedding 函数

有三种方式创建 embedding 函数：

#### 方式一：使用内置 st_embedding 函数（推荐）

jolix_embedding 扩展提供了内置的 Sentence Transformers 嵌入函数，安装后自动可用，无需手动创建：

```sql
-- 使用默认模型（all-MiniLM-L6-v2，输出384维向量）
SELECT st_embedding('hello world');

-- 使用指定模型
SELECT st_embedding('hello world', 'BAAI/bge-small-en-v1.5');
```

#### 方式二：创建简单的 PL/pgSQL 测试函数

```sql
CREATE OR REPLACE FUNCTION simple_embedding(input text) RETURNS vector
LANGUAGE plpgsql IMMUTABLE AS $$
BEGIN
    RETURN '[0.1, 0.2, 0.3]'::vector;
END;
$$;
```

#### 方式三：使用 PL/Python 自定义函数

```sql
CREATE OR REPLACE FUNCTION local_embedding(input_text text)
RETURNS vector
AS $$
import os
os.environ['HF_ENDPOINT'] = 'https://hf-mirror.com'

if 'embedding_model' not in SD:
    from sentence_transformers import SentenceTransformer
    model_path = '/root/sentence_transformers_model'
    SD['embedding_model'] = SentenceTransformer(model_path)

model = SD['embedding_model']
embedding = model.encode(input_text)
return '[' + ','.join(map(str, embedding)) + ']'
$$ LANGUAGE plpython3u IMMUTABLE;
```

### 2. 创建表

使用 `EMBEDDING AS (expr) STORED` 语法创建嵌入列：

```sql
CREATE TABLE articles (
    id int PRIMARY KEY,
    content text,
    category text EMBEDDING AS (simple_embedding(content)) STORED
) WITH (vector_len=3);
```

建表时自动创建：
- **隐藏列** `{column}_embedding`：存储向量嵌入值，类型为 `vector(vector_len)`
- **触发器** `jolix_predict_{column}_{oid}`：BEFORE INSERT OR UPDATE，自动调用嵌入函数
- **向量索引** `{table}_{column}_embedding_idx`：在 `_embedding` 列上创建向量索引

### 3. 插入数据

```sql
INSERT INTO articles (id, content) VALUES (1, 'hello world');
INSERT INTO articles (id, content) VALUES (2, 'machine learning');

-- 查看自动生成的向量
SELECT id, content, category_embedding FROM articles;
```

### 4. 语义搜索

```sql
-- L2距离搜索
SELECT id, content, category <-> 'search query' AS distance
FROM articles
ORDER BY category <-> 'search query'
LIMIT 10;

-- 余弦距离搜索
SELECT id, content, category <=> 'search query' AS distance
FROM articles
WHERE category <=> 'search query' < 1.0
ORDER BY category <=> 'search query'
LIMIT 10;
```

## 支持的距离操作符

| 操作符 | 名称 | 说明 |
|--------|------|------|
| `<->` | L2距离 | 欧几里得距离，最常用 |
| `<=>` | 余弦距离 | 适合文本相似度 |
| `<#>` | 内积 | 负内积，适合归一化向量 |
| `<+>` | L1距离 | 曼哈顿距离 |

## 向量距离查询重写

当 EMBEDDING 列出现在向量距离操作符中时，查询会自动重写：

- `category <-> 'search text'` → `category_embedding <-> simple_embedding('search text')`
- EMBEDDING 列引用自动替换为 `_embedding` 伴随列
- 文本常量自动通过嵌入函数转换为向量

### 重写验证

```sql
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM articles WHERE category <=> 'search' < 1.0 ORDER BY category <=> 'search';
```

输出中可以看到 `category_embedding <=> '[0.1,0.2,0.3]'::vector`，确认重写正确。

## WITH 参数完整参考

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `vector_len` | int | 1536 | 嵌入向量维度 |
| `vector_index` | enum | ivfflat | 向量索引类型：ivfflat 或 hnsw |
| `vector_distance` | enum | vector_l2_ops | 向量距离类型：vector_l2_ops, vector_cosine_ops, vector_ip_ops |
| `lists` | int | - | ivfflat 索引的 lists 参数 |
| `m` | int | - | hnsw 索引的 m 参数 |
| `ef_construction` | int | - | hnsw 索引的 ef_construction 参数 |

## 多个 EMBEDDING 列

每个 EMBEDDING 列使用独立的嵌入表达式：

```sql
CREATE OR REPLACE FUNCTION title_embedding(input text) RETURNS vector
LANGUAGE plpgsql IMMUTABLE AS $$
BEGIN
    RETURN '[0.4, 0.5, 0.6]'::vector;
END;
$$;

CREATE TABLE documents (
    id int PRIMARY KEY,
    title text,
    content text,
    title_vec text EMBEDDING AS (title_embedding(title)) STORED,
    content_vec text EMBEDDING AS (simple_embedding(content)) STORED
) WITH (vector_len=3);
```

## 配合向量索引

```sql
-- 使用 ivfflat 索引（默认）
CREATE TABLE articles_ivfflat (
    id int PRIMARY KEY,
    content text,
    category text EMBEDDING AS (simple_embedding(content)) STORED
) WITH (vector_len=3, vector_index=ivfflat, lists=100);

-- 使用 HNSW 索引（更高性能）
CREATE TABLE articles_hnsw (
    id int PRIMARY KEY,
    content text,
    category text EMBEDDING AS (simple_embedding(content)) STORED
) WITH (vector_len=3, vector_index=hnsw, vector_distance=vector_cosine_ops, m=16, ef_construction=64);
```

## 隐藏列机制

| 列 | 类型 | 说明 |
|----|------|------|
| `{column}` | text | EMBEDDING 列，存储原始文本值 |
| `{column}_embedding` | vector(N) | 隐藏列，存储嵌入向量 |

- `SELECT *` 不显示 `_embedding` 隐藏列
- 显式引用隐藏列可以查询：`SELECT category_embedding FROM t`

## 工作原理

### 插入/更新时

1. BEFORE 触发器检测 EMBEDDING 列
2. 调用嵌入函数计算向量
3. 将向量结果存储到 `_embedding` 伴随列

### 查询时

1. 检测查询中的距离操作符和 EMBEDDING 列
2. 自动将 EMBEDDING 列替换为 `_embedding` 伴随列
3. 将文本常量通过嵌入函数转换为向量
4. 替换 text 操作符为 vector 操作符
5. 执行向量搜索

## ALTER TABLE 操作

### 添加 EMBEDDING 列

```sql
ALTER TABLE my_table ADD COLUMN embedding_col text EMBEDDING AS (simple_embedding(content)) STORED;
```

自动创建 `_embedding` 伴随列、触发器和向量索引。

## 内置 Embedding 函数

jolix_embedding 扩展提供了基于 Sentence Transformers 的内置嵌入函数，无需手动创建 PL/Python 函数即可使用。

### 前提条件

使用内置 embedding 函数需要：

1. **Python 3.8+** 已安装
2. **sentence-transformers** 库已安装：`pip install sentence-transformers`
3. **jolix_embedding** 扩展已加载：`LOAD 'jolix_embedding'`（initdb 时自动安装）

### GUC 参数配置

| 参数 | 类型 | 默认值 | 级别 | 说明 |
|------|------|--------|------|------|
| `jolix_embedding.model_name` | string | sentence-transformers/all-MiniLM-L6-v2 | USERSET | 默认模型名称 |
| `jolix_embedding.model_path` | string | /usr/local/pgsql/models | SIGHUP | 模型文件存储路径 |

```sql
-- 修改默认模型
SET jolix_embedding.model_name = 'BAAI/bge-small-en-v1.5';

-- 修改模型存储路径（需重启生效）
ALTER SYSTEM SET jolix_embedding.model_path = '/data/models';
SELECT pg_reload_conf();
```

### st_embedding 函数

使用默认模型生成文本嵌入向量，返回 `vector` 类型。

```sql
-- 语法
st_embedding(input_text text) RETURNS vector

-- 示例
SELECT st_embedding('hello world');
-- 返回: [0.056, -0.023, 0.089, ...]（384维向量）
```

**参数说明**：

| 参数 | 类型 | 说明 |
|------|------|------|
| `input_text` | text | 输入文本，NULL 时返回 NULL |

**特点**：
- 使用 `jolix_embedding.model_name` GUC 参数指定的默认模型
- 默认模型为 `sentence-transformers/all-MiniLM-L6-v2`（384维）
- 模型在会话中缓存，首次调用加载，后续调用复用
- 自动从 HuggingFace 镜像（hf-mirror.com）下载模型

**在 EMBEDDING 列中使用**：

```sql
CREATE TABLE articles (
    id SERIAL PRIMARY KEY,
    content text,
    category text EMBEDDING AS (st_embedding(content)) STORED
) WITH (vector_len=384);

INSERT INTO articles (content) VALUES ('machine learning basics');
-- category_embedding 自动生成384维向量
```

### st_embedding 两参数版本

使用指定模型生成文本嵌入向量。

```sql
-- 语法
st_embedding(input_text text, model_name text) RETURNS vector

-- 示例
SELECT st_embedding('hello world', 'BAAI/bge-small-en-v1.5');
-- 返回: 384维向量

SELECT st_embedding('hello world', 'sentence-transformers/all-mpnet-base-v2');
-- 返回: 768维向量
```

**参数说明**：

| 参数 | 类型 | 说明 |
|------|------|------|
| `input_text` | text | 输入文本，NULL 时返回 NULL |
| `model_name` | text | 模型名称，NULL 时使用默认模型 |

**注意**：不同模型输出不同维度的向量，`vector_len` 必须与模型输出维度一致。

### st_embedding_text 函数

使用默认模型生成文本嵌入向量，返回 `text` 类型（JSON 数组格式）。

```sql
-- 语法
st_embedding_text(input_text text) RETURNS text

-- 示例
SELECT st_embedding_text('hello world');
-- 返回: "[0.05600000,-0.02300000,0.08900000,...]"
```

**用途**：当需要将嵌入向量作为文本存储或传输时使用。通常建议使用 `st_embedding` 直接返回 vector 类型。

### st_embedding_list_models 函数

列出本地已下载的模型。

```sql
-- 语法
st_embedding_list_models() RETURNS SETOF text

-- 示例
SELECT * FROM st_embedding_list_models();
-- 返回本地模型目录下的所有模型名称
```

### 常用模型参考

| 模型名称 | 维度 | 大小 | 说明 |
|---------|------|------|------|
| `sentence-transformers/all-MiniLM-L6-v2` | 384 | 80MB | 默认模型，速度快，适合通用场景 |
| `sentence-transformers/all-mpnet-base-v2` | 768 | 420MB | 高质量，适合精度要求高的场景 |
| `BAAI/bge-small-en-v1.5` | 384 | 130MB | BGE 小模型，英文 |
| `BAAI/bge-base-en-v1.5` | 768 | 420MB | BGE 基础模型，英文 |
| `BAAI/bge-large-en-v1.5` | 1024 | 1.2GB | BGE 大模型，英文 |
| `shibing624/text2vec-base-chinese` | 768 | 400MB | 中文文本向量化 |

**选择建议**：
- 通用场景：`all-MiniLM-L6-v2`（默认，384维，速度快）
- 高精度场景：`all-mpnet-base-v2`（768维，质量高）
- 中文场景：`text2vec-base-chinese`（768维，中文优化）

### 模型自动下载

首次调用 `st_embedding` 时，如果本地不存在指定模型，系统会自动从 HuggingFace 镜像下载：

- 默认镜像：`https://hf-mirror.com`（国内加速）
- 存储路径：`/usr/local/pgsql/models/`（可通过 `jolix_embedding.model_path` 修改）
- 下载后缓存，后续调用直接使用本地文件

### 会话级模型缓存

模型在数据库会话中缓存：
- 同一会话中首次调用加载模型到内存
- 后续调用直接使用缓存的模型实例
- 不同会话各自独立缓存
- 连接断开后缓存释放

### 内置函数对比

| 函数 | 返回类型 | 模型指定 | 用途 |
|------|---------|---------|------|
| `st_embedding(text)` | vector | GUC 默认模型 | EMBEDDING 列表达式（推荐） |
| `st_embedding(text, text)` | vector | 参数指定模型 | 多模型场景 |
| `st_embedding_text(text)` | text | GUC 默认模型 | 文本格式输出 |
| `st_embedding_list_models()` | SETOF text | - | 列出本地模型 |

## 示例场景

### 文档搜索

```sql
CREATE TABLE documents (
    id SERIAL PRIMARY KEY,
    title TEXT,
    body TEXT EMBEDDING AS (st_embedding(body)) STORED
) WITH (vector_len=384);

-- 搜索相关文档
SELECT title, body
FROM documents
ORDER BY body <=> 'machine learning applications'
LIMIT 10;
```

### 问答系统

```sql
CREATE TABLE qa_pairs (
    id SERIAL PRIMARY KEY,
    question TEXT EMBEDDING AS (st_embedding(question)) STORED,
    answer TEXT
) WITH (vector_len=384);

-- 找到最相似的问题
SELECT question, answer
FROM qa_pairs
ORDER BY question <=> 'user query here'
LIMIT 1;
```

### 推荐系统

```sql
CREATE TABLE products (
    id SERIAL PRIMARY KEY,
    name TEXT,
    description TEXT EMBEDDING AS (st_embedding(description)) STORED
) WITH (vector_len=384);

-- 相似产品推荐
SELECT name, description
FROM products
WHERE id != 1
ORDER BY description <=> (SELECT description FROM products WHERE id = 1)
LIMIT 5;
```

## 注意事项

1. **向量维度**：表定义的 `vector_len` 必须与嵌入函数输出维度一致
2. **扩展安装**：确保 vector 扩展已安装（initdb 时自动创建）
3. **内存使用**：使用 PL/Python 函数时，模型会缓存在每个会话的内存中
4. **距离操作符**：text 类型的距离操作符只能用于 EMBEDDING 列，非 EMBEDDING 列使用会报错
5. **查询重写**：只有 SELECT 查询会触发重写，INSERT/UPDATE 不受影响

## 故障排除

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

### 问题：模型加载失败

**原因**：模型文件不存在或路径错误

**解决**：
```sql
-- 检查模型路径
LOAD 'jolix_embedding';
SHOW jolix_embedding.model_path;

-- 列出已下载的模型
SELECT * FROM st_embedding_list_models();
```

---
**文档版本**: 1.0  
**最后更新**: 2026-05-13
