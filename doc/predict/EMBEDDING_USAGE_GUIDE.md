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

# 使用会话级缓存，避免重复加载模型
if 'embedding_model' not in SD:
    from sentence_transformers import SentenceTransformer
    model_path = '/root/sentence_transformers_model'
    SD['embedding_model'] = SentenceTransformer(model_path)

# 获取缓存的模型
model = SD['embedding_model']

# 生成embedding
embedding = model.encode(input_text)

# 转换为PostgreSQL vector格式
return '[' + ','.join(map(str, embedding)) + ']'
$$ LANGUAGE plpython3u IMMUTABLE;
```

### 2. 创建表

```sql
CREATE TABLE documents (
    id SERIAL PRIMARY KEY,
    title TEXT,
    content TEXT EMBEDDING  -- 标记为embedding列
) WITH (
    vector_len = 384,  -- all-MiniLM-L6-v2输出384维
    embedding_function = 'local_embedding'
);
```

### 3. 插入数据（自动向量化）

```sql
INSERT INTO documents (title, content) VALUES 
    ('PostgreSQL Guide', 'PostgreSQL is a powerful open source relational database.'),
    ('AI Tutorial', 'Machine learning and deep learning are transforming technology.');
```

### 4. 语义搜索

```sql
-- 搜索与"database"相关的内容
SELECT id, title, content
FROM documents
ORDER BY content <-> 'database management system'
LIMIT 10;

-- 使用不同的距离度量
-- L2距离
SELECT * FROM documents ORDER BY content <-> 'search query' LIMIT 10;

-- 余弦距离
SELECT * FROM documents ORDER BY content <=> 'search query' LIMIT 10;

-- 内积
SELECT * FROM documents ORDER BY content <#> 'search query' LIMIT 10;
```

### 5. 创建向量索引（加速查询）

```sql
-- ivfflat索引
CREATE INDEX idx_embedding ON documents 
USING ivfflat (content_embedding) WITH (lists = 100);

-- HNSW索引（更高性能）
CREATE INDEX idx_embedding_hnsw ON documents 
USING hnsw (content_embedding) WITH (m = 16, ef_construction = 64);
```

## 性能优化

### 会话级缓存

使用SD字典缓存模型，每个会话只加载一次：

```python
if 'embedding_model' not in SD:
    SD['embedding_model'] = SentenceTransformer(model_path)
model = SD['embedding_model']
```

### 批量插入

对于大量数据，考虑批量插入：

```sql
INSERT INTO documents (title, content)
SELECT 'Doc' || i, 'Content ' || i
FROM generate_series(1, 1000) AS i;
```

## 支持的操作符

| 操作符 | 名称 | 说明 |
|--------|------|------|
| `<->` | L2距离 | 欧几里得距离，最常用 |
| `<=>` | 余弦距离 | 适合文本相似度 |
| `<#>` | 内积 | 负内积，适合归一化向量 |

## 工作原理

### 插入/更新时

1. 触发器自动调用embedding_function
2. 将文本转换为向量
3. 存储在`{column}_embedding`列

### 查询时

1. 检测ORDER BY中的embedding列和向量操作符
2. 自动将查询文本转换为向量
3. 将操作符替换为vector类型的对应操作符
4. 执行向量搜索

## 注意事项

1. **模型路径**：确保模型文件存在于指定路径
2. **向量维度**：表定义的vector_len必须与模型输出维度一致
3. **内存使用**：模型会缓存在每个会话的内存中
4. **网络**：首次下载模型需要网络连接（或使用本地模型）

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
# 检查模型是否存在
ls -la /root/sentence_transformers_model/

# 重新下载模型
python3 download_model_mirror.py
```

## 示例场景

### 文档搜索

```sql
CREATE TABLE articles (
    id SERIAL PRIMARY KEY,
    title TEXT,
    body TEXT EMBEDDING
) WITH (vector_len = 384, embedding_function = 'local_embedding');

-- 搜索相关文章
SELECT title, body
FROM articles
ORDER BY body <-> 'machine learning applications'
LIMIT 10;
```

### 问答系统

```sql
CREATE TABLE qa_pairs (
    id SERIAL PRIMARY KEY,
    question TEXT EMBEDDING,
    answer TEXT
) WITH (vector_len = 384, embedding_function = 'local_embedding');

-- 找到最相似的问题
SELECT question, answer
FROM qa_pairs
ORDER BY question <-> 'user query here'
LIMIT 1;
```

### 推荐系统

```sql
CREATE TABLE products (
    id SERIAL PRIMARY KEY,
    name TEXT,
    description TEXT EMBEDDING
) WITH (vector_len = 384, embedding_function = 'local_embedding');

-- 相似产品推荐
SELECT name, description
FROM products
WHERE id != :current_product_id
ORDER BY description <-> (SELECT description FROM products WHERE id = :current_product_id)
LIMIT 5;
```

## 总结

PostgreSQL EMBEDDING功能提供了：

- ✅ 自动向量化（触发器）
- ✅ 自动查询重写
- ✅ 支持真实ML模型
- ✅ 高性能向量索引
- ✅ 简单易用的SQL接口

让向量搜索像普通SQL查询一样简单！
