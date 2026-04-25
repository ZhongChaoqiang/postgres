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

-- 语义搜索（直接使用 EMBEDDING 列和文本，系统自动重写为向量查询）
SELECT id, title, content
FROM documents
ORDER BY content <=> 'database'
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

`pg_embedding_st.model_name` 参数的上下文级别为 `PGC_USERSET`，可以在会话级、数据库级、角色级等多个层级设置。当表的 `embedding_function` 设置为 `sentence_transformers_embedding`（单参数版本）时，触发器调用该函数会读取当前生效的 `pg_embedding_st.model_name` 参数值来决定使用哪个模型。

```sql
-- 会话级设置（仅影响当前会话）
SET pg_embedding_st.model_name = 'sentence-transformers/all-MiniLM-L6-v2';

-- 数据库级设置（影响该数据库的所有连接）
ALTER DATABASE mydb SET pg_embedding_st.model_name = 'sentence-transformers/all-mpnet-base-v2';

-- 角色级设置（影响特定用户的所有连接）
ALTER ROLE myuser SET pg_embedding_st.model_name = 'BAAI/bge-large-en-v1.5';

-- 设置模型存储路径（需要超级用户权限，需重载配置生效）
ALTER SYSTEM SET pg_embedding_st.model_path = '/path/to/models';
SELECT pg_reload_conf();
```

#### 通过参数切换模型作用于 embedding_function

当表使用 `sentence_transformers_embedding` 作为 `embedding_function` 时，修改 `pg_embedding_st.model_name` 参数即可切换模型，无需重建表或修改函数定义。

**注意**：切换模型时，新模型的输出维度必须与建表时指定的 `vector_len` 一致，否则会报错。如果维度不同，需要创建新表。

```sql
-- 示例：使用默认模型创建表
CREATE TABLE docs_with_default_model (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    vector_len = 384,
    embedding_function = 'sentence_transformers_embedding'
);

-- 此时插入数据使用默认模型 all-MiniLM-L6-v2（384维）
INSERT INTO docs_with_default_model (content) VALUES ('Hello world');

-- 切换到另一个384维的模型（会话级）
SET pg_embedding_st.model_name = 'sentence-transformers/paraphrase-multilingual-MiniLM-L12-v2';

-- 后续插入的数据使用多语言模型生成 embedding
INSERT INTO docs_with_default_model (content) VALUES ('你好世界');

-- 查询时也需要使用相同的模型参数
SET pg_embedding_st.model_name = 'sentence-transformers/paraphrase-multilingual-MiniLM-L12-v2';
SELECT id, content FROM docs_with_default_model
ORDER BY content <=> '数据库'
LIMIT 5;
```

#### 数据库级设置模型（推荐生产环境）

在生产环境中，建议在数据库级别设置模型参数，确保所有连接使用一致的模型：

```sql
-- 为数据库设置768维的高质量模型
ALTER DATABASE mydb SET pg_embedding_st.model_name = 'sentence-transformers/all-mpnet-base-v2';

-- 创建对应的表（vector_len 必须匹配模型维度）
CREATE TABLE production_docs (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    vector_len = 768,
    embedding_function = 'sentence_transformers_embedding'
);

-- 所有连接到 mydb 的会话都会自动使用 all-mpnet-base-v2 模型
INSERT INTO production_docs (content) VALUES ('Production document');
```

#### 为不同角色设置不同模型

```sql
-- 英文内容处理角色使用英文模型
ALTER ROLE en_user SET pg_embedding_st.model_name = 'sentence-transformers/all-MiniLM-L6-v2';

-- 中文内容处理角色使用中文模型
ALTER ROLE zh_user SET pg_embedding_st.model_name = 'BAAI/bge-large-zh-v1.5';

-- en_user 连接后自动使用英文模型
-- zh_user 连接后自动使用中文模型
-- 注意：不同模型的维度不同，需要使用不同的表
```

#### 固定模型的 embedding_function（不受 GUC 参数影响）

如果希望表的 embedding_function 使用固定模型，不受 `pg_embedding_st.model_name` 参数变化的影响，可以创建一个包装函数，内部调用双参数版本：

```sql
-- 创建固定使用特定模型的 embedding 函数
CREATE OR REPLACE FUNCTION chinese_embedding(input_text text)
RETURNS vector
LANGUAGE sql
IMMUTABLE
AS $$
    SELECT sentence_transformers_embedding(input_text, 'sentence-transformers/paraphrase-multilingual-MiniLM-L12-v2')
$$;

-- 在表中使用固定模型函数
CREATE TABLE chinese_docs (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    vector_len = 384,
    embedding_function = 'chinese_embedding'
);

-- 即使修改了 GUC 参数，此表始终使用多语言模型
SET pg_embedding_st.model_name = 'sentence-transformers/all-MiniLM-L6-v2';
INSERT INTO chinese_docs (content) VALUES ('中文内容');
-- 仍然使用 paraphrase-multilingual-MiniLM-L12-v2 模型
```

#### 参数设置层级与优先级

PostgreSQL GUC 参数的优先级从高到低为：

| 优先级 | 设置方式 | 作用范围 | 持久性 |
|--------|---------|---------|--------|
| 1（最高） | `SET pg_embedding_st.model_name` | 当前会话 | 会话结束即失效 |
| 2 | `ALTER ROLE ... SET` | 特定角色的所有连接 | 永久 |
| 3 | `ALTER DATABASE ... SET` | 特定数据库的所有连接 | 永久 |
| 4（最低） | `postgresql.conf` / `ALTER SYSTEM SET` | 全局默认 | 永久 |

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

## 距离操作符

pg_embedding_st 扩展为 `text` 类型注册了以下距离操作符，使得 EMBEDDING 列可以直接使用文本进行语义搜索：

| 操作符 | 含义 | 示例 |
|--------|------|------|
| `<=>` | 余弦距离 | `content <=> 'search text'` |
| `<->` | L2 距离 | `content <-> 'search text'` |
| `<#>` | 内积距离 | `content <#> 'search text'` |
| `<+>` | L1 距离 | `content <+> 'search text'` |

这些操作符是占位符，仅用于让 SQL 解析器接受 `text <=> text` 语法。在查询执行时，重写器会自动将 EMBEDDING 列的距离表达式重写为向量距离表达式：

- `content <=> 'database'` → `content_embedding <=> sentence_transformers_embedding('database')`
- EMBEDDING 列的 Var 节点被替换为 `_embedding` 辅助列
- 文本常量通过 `embedding_function` 转换为 vector 常量
- text 操作符被替换为对应的 vector 操作符

如果对非 EMBEDDING 列使用这些操作符，执行时会报错提示仅支持 EMBEDDING 列。

## 配置参数

| 参数名 | 默认值 | 上下文级别 | 描述 |
|--------|--------|-----------|------|
| `pg_embedding_st.model_name` | sentence-transformers/all-MiniLM-L6-v2 | PGC_USERSET | 默认模型名称，可在会话级/数据库级/角色级设置 |
| `pg_embedding_st.model_path` | /usr/local/pgsql/models | PGC_SIGHUP | 模型存储路径，需重载配置生效 |

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

-- 5. 语义搜索（直接使用 EMBEDDING 列，系统自动重写）
SELECT title, content
FROM articles
ORDER BY content <=> 'database programming'
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

-- 中文语义搜索（系统自动使用 chinese_embedding 函数转换查询文本）
SELECT content
FROM chinese_docs
ORDER BY content <=> '数据库技术'
LIMIT 5;
```

### 通过 GUC 参数设置模型（生产环境推荐）

```sql
-- 1. 安装扩展
CREATE EXTENSION vector;
CREATE EXTENSION pg_embedding_st;

-- 2. 为数据库设置多语言模型（384维）
ALTER DATABASE mydb SET pg_embedding_st.model_name = 'sentence-transformers/paraphrase-multilingual-MiniLM-L12-v2';

-- 3. 重新连接数据库使设置生效，然后创建表
\c mydb

CREATE TABLE multilingual_docs (
    id SERIAL PRIMARY KEY,
    title TEXT,
    content TEXT EMBEDDING
) WITH (
    vector_len = 384,
    embedding_function = 'sentence_transformers_embedding'
);

-- 4. 插入中英文混合数据，自动使用多语言模型生成 embedding
INSERT INTO multilingual_docs (title, content) VALUES
    ('数据库指南', 'PostgreSQL是一个功能强大的开源关系型数据库管理系统'),
    ('AI Tutorial', 'Machine learning is transforming the technology industry'),
    ('混合内容', 'This is a mixed 中英文 document');

-- 5. 语义搜索（系统自动使用数据库级设置的模型转换查询文本）
SELECT title, content
FROM multilingual_docs
ORDER BY content <=> '数据库技术'
LIMIT 5;

-- 6. 临时切换模型进行对比测试（仅当前会话）
SET pg_embedding_st.model_name = 'sentence-transformers/all-MiniLM-L6-v2';
SELECT title, content
FROM multilingual_docs
ORDER BY content <=> 'database technology'
LIMIT 5;

-- 7. 恢复数据库级设置
RESET pg_embedding_st.model_name;
```

### 多表多模型场景

在同一数据库中，不同表可以使用不同的模型策略：

```sql
-- 方式一：通过 GUC 参数 + 单参数函数（模型可动态切换）
-- 适合：模型可能需要升级切换的场景
ALTER DATABASE mydb SET pg_embedding_st.model_name = 'sentence-transformers/all-MiniLM-L6-v2';

CREATE TABLE en_docs (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    vector_len = 384,
    embedding_function = 'sentence_transformers_embedding'
);

-- 语义搜索时系统自动使用 GUC 参数指定的模型
SELECT * FROM en_docs ORDER BY content <=> 'search text' LIMIT 10;

-- 方式二：通过包装函数固定模型（模型不可动态切换）
-- 适合：需要确保模型永远不变的场景
CREATE OR REPLACE FUNCTION zh_embedding(input_text text)
RETURNS vector LANGUAGE sql IMMUTABLE AS $$
    SELECT sentence_transformers_embedding(input_text, 'BAAI/bge-large-zh-v1.5')
$$;

CREATE TABLE zh_docs (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (
    vector_len = 1024,
    embedding_function = 'zh_embedding'
);

-- 方式三：多列 EMBEDDING，不同列使用不同模型
CREATE OR REPLACE FUNCTION title_embedding(input_text text)
RETURNS vector LANGUAGE sql IMMUTABLE AS $$
    SELECT sentence_transformers_embedding(input_text, 'sentence-transformers/all-MiniLM-L6-v2')
$$;

CREATE OR REPLACE FUNCTION body_embedding(input_text text)
RETURNS vector LANGUAGE sql IMMUTABLE AS $$
    SELECT sentence_transformers_embedding(input_text, 'sentence-transformers/paraphrase-multilingual-MiniLM-L12-v2')
$$;

CREATE TABLE articles_multi_embed (
    id SERIAL PRIMARY KEY,
    title TEXT EMBEDDING,
    body TEXT EMBEDDING
) WITH (
    vector_len = 384,
    embedding_function = 'title:title_embedding;body:body_embedding'
);

INSERT INTO articles_multi_embed (title, body) VALUES
    ('AI发展', '人工智能正在改变各个行业的发展方向');
-- title_embedding 列使用 all-MiniLM-L6-v2 模型
-- body_embedding 列使用 paraphrase-multilingual-MiniLM-L12-v2 模型
```

## 注意事项

1. **首次使用**：首次调用时会自动下载模型，可能需要较长时间
2. **网络要求**：模型下载使用 hf-mirror.com 镜像，适合国内网络环境
3. **模型缓存**：模型会缓存在会话中，避免重复加载
4. **pgvector 依赖**：必须先安装 pgvector 扩展才能使用此扩展
5. **维度匹配**：创建表时 `vector_len` 参数必须与模型的输出维度匹配
6. **GUC 参数与 embedding_function**：当使用 `sentence_transformers_embedding`（单参数版本）作为 `embedding_function` 时，模型由 `pg_embedding_st.model_name` GUC 参数决定。切换模型前请确认新模型的维度与表的 `vector_len` 一致
7. **模型一致性**：同一张表的 embedding 数据应由同一模型生成。如果在会话中临时切换模型后插入数据，会导致同一张表中存在不同模型生成的 embedding，可能影响搜索准确性
8. **固定模型建议**：对于生产环境，建议使用包装函数（调用双参数版本）固定模型，避免因 GUC 参数变更导致 embedding 不一致
9. **语义搜索语法**：推荐使用 `content <=> 'search text'` 语法，系统自动重写为向量查询。也可以使用 `_embedding` 辅助列直接查询
10. **text 距离操作符**：`<=>`、`<->`、`<#>`、`<+>` 操作符仅支持 EMBEDDING 列，对非 EMBEDDING 列使用会报错

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
- pgvector 扩展：0.7.4
- pg_embedding_st 扩展：1.0（含 text 距离操作符）

### 测试结果

| 测试项 | 测试内容 | 结果 |
|--------|---------|------|
| 测试1 | sentence_transformers_embedding 基本调用 | ✅ 通过 |
| 测试2 | 返回类型为 vector | ✅ 通过 |
| 测试3 | sentence_transformers_embedding_text 函数 | ✅ 通过 |
| 测试4 | st_embedding_list_models 列出模型 | ✅ 通过 |
| 测试5 | GUC 参数会话级 SET | ✅ 通过 |
| 测试6 | EMBEDDING 列使用 sentence_transformers_embedding | ✅ 通过 |
| 测试7 | EMBEDDING 列 <=> 文本常量（查询重写） | ✅ 通过 |
| 测试8 | EMBEDDING 列 <-> 文本常量（L2距离重写） | ✅ 通过 |
| 测试9 | 文本常量 <=> EMBEDDING 列（反向顺序重写） | ✅ 通过 |
| 测试10 | _embedding 列直接查询（兼容性） | ✅ 通过 |
| 测试11 | 自定义模型（双参数版本） | ✅ 通过 |
| 测试12 | GUC 参数切换模型 + 查询重写 | ✅ 通过 |
| 测试13 | 固定模型包装函数 + 查询重写 | ✅ 通过 |
| 测试14 | 多列 EMBEDDING 不同列不同模型 + 查询重写 | ✅ 通过 |

### 测试详情

**测试1**: 基本 embedding 生成
```
SELECT sentence_transformers_embedding('Hello, world!');
-- 结果: 返回非空 vector 值 ✅
```

**测试2**: 返回类型验证
```
SELECT pg_typeof(sentence_transformers_embedding('Hello, world!'))::regtype::text;
-- 结果: vector ✅
```

**测试3**: text 格式输出
```
SELECT sentence_transformers_embedding_text('Hello, world!');
-- 结果: 返回非空 text 值 ✅
```

**测试4**: 列出可用模型
```
SELECT * FROM st_embedding_list_models();
-- 结果: 返回模型列表 ✅
```

**测试5**: GUC 参数会话级设置
```
SET pg_embedding_st.model_name = 'sentence-transformers/all-MiniLM-L6-v2';
SHOW pg_embedding_st.model_name;
-- 结果: sentence-transformers/all-MiniLM-L6-v2 ✅
```

**测试6**: EMBEDDING 列配合使用
```sql
CREATE TABLE documents (
    id SERIAL PRIMARY KEY,
    title TEXT,
    content TEXT EMBEDDING
) WITH (
    vector_len = 384,
    embedding_function = 'sentence_transformers_embedding'
);
INSERT INTO documents (title, content) VALUES
    ('PostgreSQL Guide', 'PostgreSQL is a powerful open source database.'),
    ('AI Tutorial', 'Machine learning is transforming technology.');
SELECT id, title, content IS NOT NULL AS content_not_null FROM documents;
-- 结果: 自动生成 content_embedding 列，自动创建 ivfflat 索引 ✅
```

**测试7**: EMBEDDING 列 <=> 文本常量（余弦距离查询重写）
```sql
SELECT id, title, content FROM documents
ORDER BY content <=> 'database' LIMIT 10;
-- 结果: 正常返回，PostgreSQL Guide 排名靠前（语义最相关）✅
-- 系统自动将 content <=> 'database' 重写为向量距离查询
```

**测试8**: EMBEDDING 列 <-> 文本常量（L2 距离查询重写）
```sql
SELECT id, title, content FROM documents
ORDER BY content <-> 'database' LIMIT 10;
-- 结果: L2 距离计算正确 ✅
```

**测试9**: 文本常量 <=> EMBEDDING 列（反向顺序）
```sql
SELECT id, title, content FROM documents
ORDER BY 'database' <=> content LIMIT 10;
-- 结果: 反向顺序也能正常工作 ✅
```

**测试10**: _embedding 列直接查询（向后兼容）
```sql
SELECT id, title, content FROM documents
ORDER BY content_embedding <=> sentence_transformers_embedding('database')
LIMIT 10;
-- 结果: 直接使用 _embedding 列仍然正常工作 ✅
```

**测试11**: 自定义模型（双参数版本）
```sql
SELECT sentence_transformers_embedding('Hello, world!', 'sentence-transformers/paraphrase-multilingual-MiniLM-L12-v2') IS NOT NULL;
-- 结果: t ✅
```

**测试12**: GUC 参数切换模型 + 查询重写
```sql
CREATE TABLE docs_with_default_model (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (vector_len = 384, embedding_function = 'sentence_transformers_embedding');

INSERT INTO docs_with_default_model (content) VALUES ('Hello world');
SET pg_embedding_st.model_name = 'sentence-transformers/paraphrase-multilingual-MiniLM-L12-v2';
INSERT INTO docs_with_default_model (content) VALUES ('你好世界');
SELECT id, content FROM docs_with_default_model ORDER BY content <=> '数据库' LIMIT 5;
-- 结果: 切换模型后插入的数据使用新模型，查询重写也使用新模型转换查询文本 ✅
```

**测试13**: 固定模型包装函数 + 查询重写
```sql
CREATE OR REPLACE FUNCTION chinese_embedding(input_text text)
RETURNS vector LANGUAGE sql IMMUTABLE AS $$
    SELECT sentence_transformers_embedding(input_text, 'sentence-transformers/paraphrase-multilingual-MiniLM-L12-v2')
$$;
CREATE TABLE chinese_docs (
    id SERIAL PRIMARY KEY,
    content TEXT EMBEDDING
) WITH (vector_len = 384, embedding_function = 'chinese_embedding');
SET pg_embedding_st.model_name = 'sentence-transformers/all-MiniLM-L6-v2';
INSERT INTO chinese_docs (content) VALUES ('中文内容');
SELECT id, content FROM chinese_docs ORDER BY content <=> '中文搜索' LIMIT 5;
-- 结果: 即使 GUC 参数设置为不同模型，仍使用固定模型进行插入和查询转换 ✅
```

**测试14**: 多列 EMBEDDING 不同列不同模型 + 查询重写
```sql
CREATE TABLE articles_multi_embed (
    id SERIAL PRIMARY KEY,
    title TEXT EMBEDDING,
    body TEXT EMBEDDING
) WITH (vector_len = 384, embedding_function = 'title:title_embedding;body:body_embedding');
INSERT INTO articles_multi_embed (title, body) VALUES ('AI发展', '人工智能正在改变各个行业的发展方向');
SELECT * FROM articles_multi_embed ORDER BY title <=> 'AI' LIMIT 5;
SELECT * FROM articles_multi_embed ORDER BY body <=> '行业技术' LIMIT 5;
-- 结果: 两列分别使用不同模型进行查询转换 ✅
```

### 测试结论

**总体评价：优秀** ✅

全部 14 项测试通过。核心发现：

1. **用户友好的语义搜索语法**：`content <=> 'search text'` 语法完全可用。系统在查询执行阶段自动完成以下转换：
   - 将 EMBEDDING 列的 Var 节点替换为 `_embedding` 辅助列
   - 将文本常量通过 `embedding_function` 转换为 vector 常量
   - 将 text 距离操作符替换为对应的 vector 距离操作符

2. **GUC 参数与查询重写的联动**：当 `embedding_function` 为 `sentence_transformers_embedding` 时，查询重写会读取当前生效的 `pg_embedding_st.model_name` 参数来决定使用哪个模型转换查询文本。

3. **固定模型的查询重写**：当使用包装函数固定模型时，查询重写会调用该包装函数转换查询文本，不受 GUC 参数影响。

4. **多列多模型的查询重写**：每列独立使用各自的 `embedding_function` 进行查询文本转换。

5. **向后兼容**：直接使用 `_embedding` 辅助列和 `sentence_transformers_embedding()` 函数的方式仍然可用。
