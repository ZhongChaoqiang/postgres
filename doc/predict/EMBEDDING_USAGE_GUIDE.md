# PostgreSQL EMBEDDING 功能完整使用指南

## 环境要求

- PostgreSQL 自定义版本（支持EMBEDDING功能）
- PL/Python3 扩展
- sentence-transformers 库
- 本地模型文件（/root/sentence_transformers_model）

## 快速开始

### 1. 创建Embedding函数

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

```sql
CREATE TABLE documents (
    id SERIAL PRIMARY KEY,
    title TEXT,
    content TEXT EMBEDDING
) WITH (
    vector_len = 384,
    embedding_function = 'local_embedding',
    vector_index = hnsw,
    vector_distance = vector_cosine_ops,
    m = 16,
    ef_construction = 64
);
```

建表时自动创建：
- **隐藏列** `{column}_embedding`：类型 `vector(vector_len)`
- **触发器** `pg_embedding_{column}_{oid}`：自动调用嵌入函数
- **向量索引** `{table}_{column}_embedding_idx`：自动创建

### 3. 插入数据（自动向量化）

```sql
INSERT INTO documents (title, content) VALUES 
    ('PostgreSQL Guide', 'PostgreSQL is a powerful open source relational database.'),
    ('AI Tutorial', 'Machine learning and deep learning are transforming technology.');
```

### 4. 语义搜索

```sql
-- L2距离
SELECT * FROM documents ORDER BY content <-> 'search query' LIMIT 10;

-- 余弦距离
SELECT * FROM documents ORDER BY content <=> 'search query' LIMIT 10;

-- 内积
SELECT * FROM documents ORDER BY content <#> 'search query' LIMIT 10;

-- L1距离
SELECT * FROM documents ORDER BY content <+> 'search query' LIMIT 10;
```

### 5. 向量函数

EMBEDDING 列可直接用于 pgvector 向量函数，系统自动将列替换为 `_embedding` 列：

```sql
-- 距离函数
SELECT id, l2_distance(content, 'search text') AS dist FROM documents ORDER BY dist;
SELECT id, cosine_distance(content, 'search text') AS dist FROM documents ORDER BY dist;
SELECT id, inner_product(content, 'search text') AS ip FROM documents ORDER BY ip;
SELECT id, l1_distance(content, 'search text') AS dist FROM documents ORDER BY dist;

-- 工具函数
SELECT id, vector_dims(content) AS dims FROM documents;
SELECT id, vector_norm(content) AS norm FROM documents;
SELECT id, l2_normalize(content) AS normalized FROM documents;
```

### 6. 聚合函数

```sql
-- 平均向量
SELECT avg(content) AS avg_vec FROM documents;

-- 分组平均向量
SELECT category, avg(content) AS avg_vec FROM documents GROUP BY category;

-- 向量求和
SELECT sum(content) AS sum_vec FROM documents;
```

### 7. 高级查询

```sql
-- WHERE + ORDER BY + SELECT距离
SELECT id, content, content <=> 'search' AS dist
FROM documents WHERE category = 'tech' ORDER BY dist LIMIT 5;

-- 距离过滤
SELECT * FROM documents WHERE content <-> 'search' < 0.5;

-- CASE表达式
SELECT id, CASE WHEN content <=> 'search' < 0.5 THEN 'close' ELSE 'far' END AS proximity
FROM documents;

-- JOIN条件
SELECT a.id, b.id FROM documents a, documents b
WHERE a.id != b.id AND a.content <-> b.content < 1.0;

-- 子查询
SELECT * FROM documents WHERE id IN (
    SELECT id FROM documents WHERE content <-> 'search' < 0.5
);
```

### 8. 创建向量索引（可选）

建表时已自动创建向量索引。如需自定义：

```sql
DROP INDEX documents_content_embedding_idx;

CREATE INDEX idx_embedding ON documents 
USING ivfflat (content_embedding) WITH (lists = 100);

CREATE INDEX idx_embedding_hnsw ON documents 
USING hnsw (content_embedding) WITH (m = 16, ef_construction = 64);
```

## 查询重写机制

EMBEDDING 功能使用**通用 Var 节点替换 walker**，自动遍历整个查询树，将 EMBEDDING 列（text 类型）替换为 `_embedding` 列（vector 类型）。

### 重写范围

| SQL 构造 | 支持状态 | 示例 |
|----------|---------|------|
| SELECT 目标列表 | ✅ | `SELECT content <-> 'text' AS dist` |
| WHERE 子句 | ✅ | `WHERE content <-> 'text' < 0.5` |
| ORDER BY 子句 | ✅ | `ORDER BY content <=> 'text'` |
| HAVING 子句 | ✅ | `HAVING avg(content) <-> '[0]' < 1` |
| JOIN 条件 | ✅ | `ON a.content <-> b.content < 0.5` |
| CASE 表达式 | ✅ | `CASE WHEN content <=> 'text' < 0.5` |
| 子查询 | ✅ | `WHERE id IN (SELECT ... WHERE content <-> ...)` |
| GROUP BY | ✅ | `GROUP BY category` |

### 重写模式

| 模式 | 说明 | 处理方式 |
|------|------|---------|
| 距离操作符 | `<->`, `<=>`, `<#>`, `<+>` | 替换列 + 替换操作符 + 转换常量 |
| 向量函数 | `l2_distance`, `cosine_distance` 等 | 替换列 + 转换常量 |
| 向量工具函数 | `vector_dims`, `vector_norm` 等 | 替换列 |
| 向量聚合 | `avg`, `sum` | 替换列 |
| Var-Var 距离 | `a.content <-> b.content` | 替换两侧列 + 替换操作符 |

## 支持的操作符

| 操作符 | 名称 | 说明 |
|--------|------|------|
| `<->` | L2距离 | 欧几里得距离，最常用 |
| `<=>` | 余弦距离 | 适合文本相似度 |
| `<#>` | 内积 | 负内积，适合归一化向量 |
| `<+>` | L1距离 | 曼哈顿距离 |

## 支持的向量函数

### 距离函数

| 函数 | 说明 |
|------|------|
| `l2_distance(vector, vector)` | L2/欧几里得距离 |
| `cosine_distance(vector, vector)` | 余弦距离 |
| `inner_product(vector, vector)` | 内积 |
| `l1_distance(vector, vector)` | L1/曼哈顿距离 |

### 工具函数

| 函数 | 说明 |
|------|------|
| `vector_dims(vector)` | 向量维度数 |
| `vector_norm(vector)` | 欧几里得范数 |
| `l2_normalize(vector)` | L2归一化 |
| `subvector(vector, int, int)` | 子向量提取 |
| `binary_quantize(vector)` | 二值量化 |

### 聚合函数

| 函数 | 说明 |
|------|------|
| `avg(vector)` | 向量平均值 |
| `sum(vector)` | 向量求和 |

## WITH 参数完整参考

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `vector_len` | int | 10 | 向量维度，必须与embedding函数输出一致 |
| `embedding_function` | string | 无 | embedding函数名称 |
| `vector_index` | enum | ivfflat | 向量索引类型：ivfflat 或 hnsw |
| `vector_distance` | enum | vector_l2_ops | 距离度量：vector_l2_ops, vector_cosine_ops, vector_ip_ops |
| `lists` | int | 无 | ivfflat索引的倒排列表数量 |
| `m` | int | 无 | hnsw索引每层最大连接数 |
| `ef_construction` | int | 无 | hnsw索引构建时动态候选列表大小 |

## 性能优化

### 会话级缓存

使用SD字典缓存模型，每个会话只加载一次：

```python
if 'embedding_model' not in SD:
    SD['embedding_model'] = SentenceTransformer(model_path)
model = SD['embedding_model']
```

### 批量插入

```sql
INSERT INTO documents (title, content)
SELECT 'Doc' || i, 'Content ' || i
FROM generate_series(1, 1000) AS i;
```

## 工作原理

### 插入/更新时

1. 触发器自动调用embedding_function
2. 将文本转换为向量
3. 存储在`{column}_embedding`列

### 查询时（通用 Var 节点替换 Walker）

1. 遍历查询树的所有节点（SELECT、WHERE、ORDER BY、HAVING、JOIN、子查询等）
2. 检测 EMBEDDING 列（text 类型）出现在期望 vector 类型的上下文中
3. 自动将 EMBEDDING 列替换为 `_embedding` 列（vector 类型）
4. 将距离操作符替换为 vector 版本
5. 将文本常量通过 embedding 函数转换为向量常量

## 注意事项

1. **模型路径**：确保模型文件存在于指定路径
2. **向量维度**：表定义的vector_len必须与模型输出维度一致
3. **内存使用**：模型会缓存在每个会话的内存中
4. **网络**：首次下载模型需要网络连接（或使用本地模型）
5. **向量函数参数**：使用向量函数时，EMBEDDING 列会自动替换为 `_embedding` 列，文本常量也会自动转换

## 故障排除

### 问题：函数创建卡住

**原因**：每次调用都重新加载模型

**解决**：使用SD字典缓存模型（见上面的示例）

### 问题：向量维度不匹配

**原因**：vector_len与模型输出维度不一致

**解决**：all-MiniLM-L6-v2输出384维，设置vector_len=384

### 问题：模型加载失败

**原因**：模型文件不存在或路径错误

**解决**：
```bash
ls -la /root/sentence_transformers_model/
python3 download_model_mirror.py
```

## 示例场景

### 文档搜索

```sql
CREATE TABLE articles (
    id SERIAL PRIMARY KEY,
    title TEXT,
    body TEXT EMBEDDING
) WITH (vector_len = 384, embedding_function = 'local_embedding',
        vector_index = hnsw, vector_distance = vector_cosine_ops,
        m = 16, ef_construction = 64);

SELECT title, body
FROM articles
ORDER BY body <=> 'machine learning applications'
LIMIT 10;
```

### 问答系统

```sql
CREATE TABLE qa_pairs (
    id SERIAL PRIMARY KEY,
    question TEXT EMBEDDING,
    answer TEXT
) WITH (vector_len = 384, embedding_function = 'local_embedding');

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
    description TEXT EMBEDDING
) WITH (vector_len = 384, embedding_function = 'local_embedding',
        vector_index = hnsw, vector_distance = vector_ip_ops);

SELECT name, description
FROM products
WHERE id != :current_product_id
ORDER BY description <-> (SELECT description FROM products WHERE id = :current_product_id)
LIMIT 5;
```

## 总结

PostgreSQL EMBEDDING功能提供了：

- ✅ 自动向量化（触发器）
- ✅ 通用查询重写（Var节点替换Walker）
- ✅ 全查询树覆盖（SELECT/WHERE/ORDER BY/HAVING/JOIN/子查询）
- ✅ 支持距离操作符（`<->`/`<=>`/`<#>`/`<+>`）
- ✅ 支持向量函数（l2_distance/cosine_distance/vector_dims等）
- ✅ 支持向量聚合（avg/sum）
- ✅ 自动创建向量索引（支持 ivfflat/hnsw + 多种距离度量）
- ✅ 索引参数自动传递（lists/m/ef_construction）
- ✅ 支持真实ML模型
- ✅ 简单易用的SQL接口

让向量搜索像普通SQL查询一样简单！
