# pg_embedding_st 使用指南

## 简介

pg_embedding_st 是一个 PostgreSQL 扩展，提供内置的 Sentence Transformers embedding 函数。用户无需手动创建 PL/Python 函数，只需安装扩展即可使用。

## 功能特点

- **内置函数**：无需手动创建 PL/Python 函数
- **直接返回 vector 类型**：可以直接用作 `embedding_function` 参数
- **自动下载模型**：如果本地不存在模型文件，自动从 HuggingFace 镜像下载
- **会话级缓存**：模型在会话中缓存，避免重复加载
- **支持自定义模型**：可以指定任意 HuggingFace 模型
- **国内镜像支持**：默认使用 hf-mirror.com 加速下载

## 安装

### 前置条件

1. PostgreSQL 已编译并安装
2. Python 3 已安装
3. sentence-transformers 库已安装：

```bash
pip install sentence-transformers
```

4. pgvector 扩展已安装（用于 vector 类型）

### 安装扩展

```sql
CREATE EXTENSION vector;  -- 必须先安装 pgvector
CREATE EXTENSION pg_embedding_st;
```

## 使用方法

### 基本用法

```sql
-- 生成 embedding（直接返回 vector 类型）
SELECT sentence_transformers_embedding('Hello, world!');

-- 查看返回类型
SELECT pg_typeof(sentence_transformers_embedding('Hello, world!'));
-- 结果: vector
```

### 直接用于 EMBEDDING 列

```sql
-- 创建带 EMBEDDING 列的表，直接使用内置函数
CREATE TABLE documents (
    id SERIAL PRIMARY KEY,
    title TEXT,
    content TEXT EMBEDDING
) WITH (
    vector_len = 384,
    embedding_function = 'sentence_transformers_embedding'
);

-- 插入数据时自动生成 embedding
INSERT INTO documents (title, content) VALUES 
    ('PostgreSQL Guide', 'PostgreSQL is a powerful open source database.'),
    ('AI Tutorial', 'Machine learning is transforming technology.');

-- 语义搜索
SELECT id, title, content
FROM documents
ORDER BY content <=> sentence_transformers_embedding('database')
LIMIT 10;
```

### 使用自定义模型

```sql
-- 指定模型名称（返回 vector 类型）
SELECT sentence_transformers_embedding('Hello, world!', 'BAAI/bge-large-en-v1.5');

-- 使用中文模型
SELECT sentence_transformers_embedding('你好，世界！', 'sentence-transformers/paraphrase-multilingual-MiniLM-L12-v2');
```

### 获取 text 格式输出

如果需要 text 格式的输出，可以使用 `sentence_transformers_embedding_text` 函数：

```sql
-- 返回 text 格式 '[0.1, 0.2, ...]'
SELECT sentence_transformers_embedding_text('Hello, world!');
```

### 配置参数

```sql
-- 设置默认模型名称
SET pg_embedding_st.model_name = 'sentence-transformers/all-MiniLM-L6-v2';

-- 设置模型存储路径（需要超级用户权限）
ALTER SYSTEM SET pg_embedding_st.model_path = '/path/to/models';
SELECT pg_reload_conf();
```

### 查看可用模型

```sql
SELECT * FROM st_embedding_list_models();
```

## 函数列表

| 函数名 | 参数 | 返回类型 | 描述 |
|--------|------|----------|------|
| `sentence_transformers_embedding(text)` | input_text | vector | 使用默认模型生成 embedding |
| `sentence_transformers_embedding(text, text)` | input_text, model_name | vector | 使用指定模型生成 embedding |
| `sentence_transformers_embedding_text(text)` | input_text | text | 返回 text 格式的 embedding |
| `st_embedding_list_models()` | 无 | SETOF text | 列出本地可用的模型 |

## 配置参数

| 参数名 | 默认值 | 描述 |
|--------|--------|------|
| `pg_embedding_st.model_name` | sentence-transformers/all-MiniLM-L6-v2 | 默认模型名称 |
| `pg_embedding_st.model_path` | /usr/local/pgsql/models | 模型存储路径 |

## 推荐模型

| 模型名称 | 维度 | 语言 | 描述 |
|----------|------|------|------|
| sentence-transformers/all-MiniLM-L6-v2 | 384 | 英文 | 快速、轻量级 |
| sentence-transformers/all-mpnet-base-v2 | 768 | 英文 | 高质量 |
| BAAI/bge-large-en-v1.5 | 1024 | 英文 | 高质量、开源 |
| sentence-transformers/paraphrase-multilingual-MiniLM-L12-v2 | 384 | 多语言 | 支持中文 |
| BAAI/bge-large-zh-v1.5 | 1024 | 中文 | 中文专用 |

## 完整示例

### 创建语义搜索应用

```sql
-- 1. 安装扩展
CREATE EXTENSION vector;
CREATE EXTENSION pg_embedding_st;

-- 2. 创建表
CREATE TABLE articles (
    id SERIAL PRIMARY KEY,
    title TEXT,
    content TEXT EMBEDDING
) WITH (
    vector_len = 384,
    embedding_function = 'sentence_transformers_embedding'
);

-- 3. 插入数据
INSERT INTO articles (title, content) VALUES
    ('PostgreSQL Guide', 'PostgreSQL is a powerful open source relational database management system.'),
    ('Python Tutorial', 'Python is a popular programming language for data science and machine learning.'),
    ('Machine Learning Basics', 'Machine learning algorithms can learn patterns from data automatically.');

-- 4. 创建向量索引
CREATE INDEX idx_articles_embedding ON articles 
USING ivfflat (content_embedding vector_l2_ops) WITH (lists = 100);

-- 5. 语义搜索
SELECT title, content
FROM articles
ORDER BY content_embedding <=> sentence_transformers_embedding('database programming')
LIMIT 5;
```

### 使用自定义模型

```sql
-- 创建使用中文模型的 embedding 函数
CREATE OR REPLACE FUNCTION chinese_embedding(text) RETURNS vector AS $$
    SELECT sentence_transformers_embedding($1, 'sentence-transformers/paraphrase-multilingual-MiniLM-L12-v2')
$$ LANGUAGE SQL IMMUTABLE;

-- 创建中文文档表
CREATE TABLE chinese_docs (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    vector_len = 384,
    embedding_function = 'chinese_embedding'
);

INSERT INTO chinese_docs (content) VALUES 
    ('PostgreSQL是一个功能强大的开源数据库'),
    ('机器学习正在改变技术行业');

-- 中文语义搜索
SELECT content
FROM chinese_docs
ORDER BY content_embedding <=> sentence_transformers_embedding('数据库技术', 'sentence-transformers/paraphrase-multilingual-MiniLM-L12-v2')
LIMIT 5;
```

## 注意事项

1. **首次使用**：首次调用时会自动下载模型，可能需要较长时间
2. **网络要求**：模型下载使用 hf-mirror.com 镜像，适合国内网络环境
3. **模型缓存**：模型会缓存在会话中，避免重复加载
4. **pgvector 依赖**：必须先安装 pgvector 扩展才能使用此扩展
5. **维度匹配**：创建表时 `vector_len` 参数必须与模型的输出维度匹配

## 错误排查

### 错误：type "vector" does not exist

```sql
-- 解决方案：先安装 pgvector 扩展
CREATE EXTENSION vector;
```

### 错误：could not import sentence_transformers module

```bash
# 解决方案：安装 sentence-transformers
pip install sentence-transformers
```

### 错误：could not load sentence-transformers model

检查网络连接，确保可以访问 HuggingFace 或 hf-mirror.com。

## 测试报告

### 测试环境
- PostgreSQL 版本：18.3（带 PREDICT/EMBEDDING 功能）
- 操作系统：WSL Ubuntu 22.04
- Python 版本：3.10
- sentence-transformers 版本：已安装
- pgvector 扩展：已安装

### 测试结果

| 测试项 | 测试内容 | 结果 |
|--------|---------|------|
| 测试1 | sentence_transformers_embedding基本调用 | ✅ 通过 |
| 测试2 | 返回类型为vector | ✅ 通过 |
| 测试3 | sentence_transformers_embedding_text函数 | ✅ 通过 |
| 测试4 | EMBEDDING列使用sentence_transformers_embedding | ✅ 通过 |
| 测试5 | 向量距离查询 | ✅ 通过 |
| 测试6 | st_embedding_list_models列出模型 | ✅ 通过 |

### 测试详情

**测试1**: 基本embedding生成
```
SELECT sentence_transformers_embedding('Hello, world!');
-- 结果: [-0.03817715,0.03291111,-0.005459366,0.014369936,...]  (384维vector)
```

**测试2**: 返回类型验证
```
SELECT pg_typeof(sentence_transformers_embedding('Hello, world!'));
-- 结果: vector
```

**测试3**: text格式输出
```
SELECT sentence_transformers_embedding_text('Hello, world!');
-- 结果: [-0.03817715,0.03291111,-0.00545937,0.01436994,...]  (text格式)
```

**测试4**: EMBEDDING列配合使用
```sql
CREATE TABLE test_st_table (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    vector_len = 384,
    embedding_function = 'sentence_transformers_embedding'
);
INSERT INTO test_st_table (content) VALUES ('Hello, world!');
-- 结果: 自动生成content_embedding列，自动创建ivfflat索引
```

**测试5**: 向量距离查询
```
SELECT id, content FROM test_st_table
ORDER BY content_embedding <=> sentence_transformers_embedding('Hello')
LIMIT 5;
-- 结果: 正常返回，距离计算正确
```

**测试6**: 列出可用模型
```
SELECT * FROM st_embedding_list_models();
-- 结果: models--sentence-transformers--all-MiniLM-L6-v2
```

### 测试结论

**总体评价：优秀** ✅

所有6项测试全部通过。`sentence_transformers_embedding` 函数可以直接返回 vector 类型，无需用户手动创建 PL/Python 函数，可以直接作为 `embedding_function` 参数使用。
