# PostgreSQL 18 安装包功能验证测试报告

**测试日期**: 2026-06-08
**测试环境**: WSL Ubuntu 22.04, PostgreSQL 18.3 deb 安装包
**LLM API**: 火山引擎（豆包）- OpenAI 兼容协议
**模型端点**: ep-20251128103853-pp9jw
**数据库端口**: 5433
**数据目录**: /var/lib/postgresql/18/verify_test

## 测试结果概要

| 测试项 | 结果 | 说明 |
|--------|------|------|
| E1: st_embedding 函数 | ✅ 通过 | 384维向量，距离计算正确 |
| E2: 创建表+手动向量列 | ✅ 通过 | 标准 CREATE TABLE |
| E3: 插入数据+手动生成向量 | ✅ 通过 | 5条数据，st_embedding 在 INSERT 中正常 |
| E4: 语义搜索 | ✅ 通过 | ML Intro(0.4439), Deep Learning(0.4885) 排名正确 |
| E5: EMBEDDING 列自动向量生成 | ✅ 通过 | 自动创建 _embedding 列和 ivfflat 索引 |
| E6: EMBEDDINGS 列 | ✅ 通过 | ft_transformer_embedding 多列向量化正常 |
| P1: LLM 推理 | ✅ 通过 | 中文回答正确 |
| P2: PREDICT 列 | ✅ 通过 | 自动生成回答 |
| P3: 对话历史 | ✅ 通过 | 记住数字42并正确回答 |
| R1: RAG 推理 | ✅ 通过 | 基于知识库上下文正确回答 |

## 修复的问题

### EMBEDDING 列自动设置 embedding_function reloption

**问题**: 创建带 `EMBEDDING AS (st_embedding(content))` 的表时，`embedding_function` reloption 没有自动设置，导致 RAG 推理（`llm_rag_infer`）无法找到 embedding 函数。

**修复**: 在 `src/backend/parser/parse_utilcmd.c` 中，当处理 `column->is_embedding` 时，从 `raw_default`（FuncCall 节点）中提取函数名，并自动添加 `embedding_function` reloption 到表的 WITH 选项中。

**修改文件**: `src/backend/parser/parse_utilcmd.c`
- 添加 `embedding_function` 字段到 `CreateStmtContext` 结构体
- 在 `column->is_embedding` 处理块中提取函数名
- 在 `transformCreateStmt` 结束时将 `embedding_function` reloption 添加到 `stmt->options`

## 详细测试结果

### E1: st_embedding 函数

```sql
SET jolix_embedding.model_name = 'sentence-transformers/all-MiniLM-L6-v2';
SELECT vector_dims(st_embedding('Hello, world!')) AS dims,
       round((st_embedding('Hello, world!') <=> st_embedding('Hi there'))::numeric, 4) AS dist_hello_hi,
       round((st_embedding('Hello, world!') <=> st_embedding('Database system'))::numeric, 4) AS dist_hello_db;
```

结果：
| dims | dist_hello_hi | dist_hello_db |
|------|---------------|---------------|
| 384  | 0.5680        | 0.8996        |

语义相似度正确：hello/hi 距离(0.568) < hello/database 距离(0.900)

### E4: 语义搜索

```sql
SELECT id, title,
       round((content_vec <=> st_embedding('artificial intelligence and neural networks'))::numeric, 4) AS distance
FROM articles ORDER BY distance LIMIT 3;
```

结果：
| id | title         | distance |
|----|---------------|----------|
| 1  | ML Intro      | 0.4439   |
| 5  | Deep Learning | 0.4885   |
| 3  | Python Tips   | 0.8822   |

语义搜索排名正确：ML Intro 和 Deep Learning 与 "artificial intelligence and neural networks" 最相关。

### E5: EMBEDDING 列

```sql
CREATE TABLE docs (
    id SERIAL PRIMARY KEY,
    content text EMBEDDING AS (st_embedding(content))
);
INSERT INTO docs (content) VALUES ('Hello world');
SELECT id, content, vector_dims(content_embedding) AS dims FROM docs;
```

结果：自动创建 `content_embedding` 列（vector(384)），INSERT 时自动生成向量。

### P1: LLM 推理

```sql
SELECT llm_infer('请用中文简短回答，不超过两句话。', '什么是PostgreSQL？') AS predict_response;
```

结果：`PostgreSQL是一款开源的对象-关系型数据库管理系统，支持复杂查询、事务处理等，广泛用于数据存储与管理场景。`

### P2: PREDICT 列

```sql
CREATE TABLE qa_test (
    id serial PRIMARY KEY,
    question text,
    answer text PREDICT AS (llm_infer('请简短回答以下问题，用中文回答，不超过两句话。', question))
) WITH (predict_timing = immediate);
INSERT INTO qa_test (question) VALUES ('什么是向量数据库？');
```

结果：answer 列自动生成回答。

### P3: 对话历史

```sql
SELECT llm_infer('请用中文回答。', '请记住这个数字：42', 3, 'hist_test') AS resp1;
SELECT llm_infer('请用中文回答。', '我刚才让你记住的数字是什么？', 3, 'hist_test') AS resp2;
```

结果：resp2 正确回答 "42"，对话历史功能正常。

### R1: RAG 推理

```sql
CREATE TABLE rag_kb (
    id SERIAL PRIMARY KEY,
    content text EMBEDDING AS (st_embedding(content))
);
INSERT INTO rag_kb (content) VALUES ('PostgreSQL is a powerful open source relational database system');
...
SET jolix_predict.current_table = 'rag_kb';
SELECT llm_rag_infer('根据提供的上下文回答问题。用中文回答。', 'PostgreSQL是什么？', 0) AS rag_response;
```

结果：`根据检索到的信息，PostgreSQL是一个强大的开源关系型数据库系统。`

RAG 推理基于知识库上下文正确回答，`embedding_function` reloption 自动设置功能正常。

## LLM 配置

```sql
SELECT set_llm_config(
    p_api_url := 'https://ark.cn-beijing.volces.com/api/v3/chat/completions',
    p_api_key := 'acc96ba1-d743-45d7-9b0e-a415bd96a046',
    p_model_name := 'ep-20251128103853-pp9jw',
    p_temperature := 0.7,
    p_max_tokens := 256,
    p_system_prompt := 'You are a helpful assistant. Reply in Chinese briefly.'
);
```

## 内置扩展

安装包内置以下扩展，初始化数据库时自动创建：
- `vector` (pgvector v0.8.2) - 向量数据类型和索引
- `jolix_predict` - LLM 推理功能
- `jolix_embedding` - 文本向量化功能

---

## EMBEDDINGS 功能详细测试

### 测试结果概要

| 测试项 | 结果 | 说明 |
|--------|------|------|
| T1: CREATE EMBEDDINGS | ✅ 通过 | 隐藏列、触发器、索引自动创建 |
| T2: INSERT 自动计算向量 | ✅ 通过 | 384维向量自动生成，SELECT * 不显示隐藏列 |
| T3: UPDATE 重新计算向量 | ✅ 通过 | 更新数据后向量自动重新计算 |
| T4: DROP EMBEDDINGS | ✅ 通过 | 隐藏列、触发器自动删除 |
| T5: CREATE IF NOT EXISTS | ✅ 通过 | 重复创建给出 NOTICE 并跳过 |
| T6: DROP IF EXISTS | ✅ 通过 | 重复删除给出 NOTICE 并跳过 |
| T7: Cosine 距离查询 | ✅ 通过 | 同类别客户距离更近 |
| T8: L2 距离查询 | ✅ 通过 | 排序结果与 Cosine 一致 |
| T9: Top-K 相似性搜索 | ✅ 通过 | 正确返回最相似的 K 条 |
| T10: 条件过滤+向量搜索 | ✅ 通过 | 过滤后正确排序 |
| T11: 函数调用形式查询 | ✅ 通过 | 精确匹配距离为 0.0000 |
| T12: 跨表向量相似性查询 | ✅ 通过 | 跨表查询正常 |
| T13: EMBEDDINGS AS 列级语法 | ✅ 通过 | 建表时直接定义 EMBEDDINGS 列 |
| T14: EMBEDDING AS (ft_transformer) | ✅ 通过 | ft_transformer_embedding 作为 EMBEDDING 函数 |
| T15: EMBEDDING AS STORED | ✅ 通过 | STORED 关键字向后兼容 |
| T16: 同表多 EMBEDDINGS | ✅ 通过 | 两个向量列独立计算和查询 |
| T17: vector_len=768 | ⚠️ 已知限制 | 列类型正确但模型返回384维（见说明） |
| T18: 多表多 EMBEDDINGS | ✅ 通过 | 不同表独立创建多个 EMBEDDINGS |
| T19: GUC 参数 | ✅ 通过 | ft_model_name, ft_vector_len 可查询 |
| T20: 单列 EMBEDDINGS | ✅ 通过 | 单列向量化正常 |

### 已知限制：vector_len WITH 选项

`CREATE EMBEDDINGS ... WITH (vector_len = N)` 中的 `vector_len` 仅控制隐藏列的类型定义（`vector(N)`），不影响 `ft_transformer_embedding` 函数返回的向量维度。实际向量维度取决于：
- 当 Python 模型可用时：由模型决定（如 all-MiniLM-L6-v2 固定输出 384 维）
- 当模型不可用时：由 `jolix_embedding.ft_vector_len` GUC 参数控制

建议：使用默认 `vector_len = 384`，与模型输出维度一致。

### 关键测试结果示例

#### Cosine 距离查询（与 id=1 客户比较）

| id | age | income | category | cosine_distance |
|----|-----|--------|----------|-----------------|
| 5  | 50  | 95000  | premium  | 0.1915          |
| 3  | 45  | 80000  | premium  | 0.2780          |
| 2  | 25  | 35000  | basic    | 0.3019          |
| 4  | 28  | 42000  | basic    | 0.4010          |

同类别(premium)的客户与 id=1 客户距离更近，语义合理。

#### 函数调用形式查询

```sql
SELECT id, age, income, category,
       round((demographic <=> ft_transformer_embedding(30::int, 50000.0::float8, 'premium'::text))::numeric, 4) AS query_dist
FROM t_customers ORDER BY query_dist LIMIT 3;
```

| id | age | income | category | query_dist |
|----|-----|--------|----------|------------|
| 1  | 30  | 50000  | premium  | 0.0000     |
| 5  | 50  | 95000  | premium  | 0.1915     |
| 3  | 45  | 80000  | premium  | 0.2780     |

精确匹配距离为 0.0000，验证函数调用形式查询正确。

#### 同表多 EMBEDDINGS

```sql
CREATE EMBEDDINGS vec1 ON t_multi USING ft_transformer_embedding (age) WITH (vector_len = 384);
CREATE EMBEDDINGS vec2 ON t_multi USING ft_transformer_embedding (income) WITH (vector_len = 384);
```

两个向量列独立计算，可分别进行相似性查询。
