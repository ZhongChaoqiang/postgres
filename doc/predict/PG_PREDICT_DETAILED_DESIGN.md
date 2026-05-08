# pg_predict 扩展详细设计文档

## 1. 概述

pg_predict 是一个 PostgreSQL 扩展，提供大语言模型（LLM）推理功能。它允许用户在 SQL 中直接调用 LLM API，并与 PREDICT 列属性深度集成，实现自动化的 LLM 推理。设计参考了 MindsDB 的 LLM 推理模式，将整行数据发送到 LLM 进行推理。

### 1.1 核心功能

1. **LLM 推理函数**：`llm_infer()` 和 `llm_predict()`，支持系统提示词、历史对话、模板填充
2. **RAG 推理函数**：`llm_rag_infer()` 和 `llm_rag_predict()`，支持向量检索增强生成
3. **配置管理**：`set_predict_config()` 和 `get_predict_config()`，支持系统级和表级配置
4. **模板填充**：支持 `{{column_name}}` 语法的 prompt_template（参考 MindsDB）
5. **自动行格式化**：无模板时自动将整行格式化为键值对文本
6. **类型转换**：predict_trigger 自动将 LLM 文本输出转换为目标列类型
7. **历史对话**：自动从表中查询历史记录作为对话上下文
8. **RAG 检索**：基于 EMBEDDING 列的向量相似度检索，自动注入上下文

## 2. 架构设计

### 2.1 模块结构

```
contrib/pg_predict/
├── pg_predict.c          # C 语言核心实现
├── pg_predict--1.0.sql   # SQL 函数和表定义
├── pg_predict.control    # 扩展控制文件
├── Makefile          # PGXS 构建脚本
└── meson.build       # Meson 构建脚本
```

### 2.2 依赖

- **libcurl**：用于 HTTP 请求调用 LLM API
- **PostgreSQL SPI**：用于 SQL 查询（配置读取、历史查询、JSON 解析）
- **PostgreSQL GUC**：用于系统级默认配置

### 2.3 配置层次

```
表级配置 (pg_predict_config WHERE scope='table' AND relid=<oid>)
    ↓ 未找到时回退
系统级配置 (pg_predict_config WHERE scope='system')
    ↓ 未找到时回退
GUC 默认值 (pg_predict.* 参数)
```

## 3. 数据结构

### 3.1 pg_predict_config 配置表

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| id | serial | - | 主键 |
| scope | text | 'system' | 配置范围：'system' 或 'table' |
| relid | oid | NULL | 表 OID（系统级为 NULL） |
| api_url | text | - | LLM API 地址 |
| api_key | text | '' | API 密钥 |
| model_name | text | 'gpt-3.5-turbo' | 模型名称 |
| temperature | float8 | 0.7 | 温度参数 |
| max_tokens | integer | 1024 | 最大 token 数 |
| system_prompt | text | '' | 系统提示词 |
| prompt_template | text | NULL | 提示词模板（`{{column_name}}` 语法） |
| history_count | integer | 0 | 历史对话条数 |
| rag_table | oid | NULL | RAG 检索表 OID（需有 EMBEDDING 列） |
| rag_similarity | float8 | 0.5 | RAG 最小相似度阈值（余弦距离） |
| rag_topn | integer | 5 | RAG 检索返回的最大行数 |
| created_at | timestamptz | now() | 创建时间 |
| updated_at | timestamptz | now() | 更新时间 |

唯一约束：`(scope, relid)`

### 3.2 GUC 参数

| 参数名 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| pg_predict.api_url | string | '' | 默认 API 地址 |
| pg_predict.api_key | string | '' | 默认 API 密钥 |
| pg_predict.model | string | 'gpt-3.5-turbo' | 默认模型 |
| pg_predict.temperature | real | 0.7 | 默认温度 |
| pg_predict.max_tokens | integer | 1024 | 默认最大 token |
| pg_predict.timeout | integer | 60 | 请求超时（秒） |

### 3.3 C 语言内部结构

```c
typedef struct LLMConfig
{
    char       *api_url;
    char       *api_key;
    char       *model;
    double      temperature;
    int         max_tokens;
    char       *system_prompt;
    char       *prompt_template;
    int         history_count;
    Oid         rag_table;
    double      rag_similarity;
    int         rag_topn;
} LLMConfig;

typedef struct ColValue
{
    char       *name;
    char       *value;
} ColValue;
```

## 4. Prompt 模板设计

### 4.1 模板语法

参考 MindsDB 的 `get_completed_prompts()` 函数，pg_predict 使用 `{{column_name}}` 双花括号语法：

- `{{column_name}}` 会被替换为当前行中对应列的值
- 列名区分大小写
- 如果列值为 NULL，该占位符替换为空字符串
- 非 PREDICT 列、非隐藏列（`_predict`、`_actual`、`_embedding`）都可作为占位符

### 4.2 Prompt 构建流程

```
输入行数据 → 提取所有非 PREDICT/非隐藏列的值
                    │
                    ▼
        ┌─────────────────────────────┐
        │  是否配置了 prompt_template？  │
        └──────┬──────────────┬───────┘
               │ 是           │ 否
               ▼              ▼
    ┌──────────────┐  ┌──────────────────┐
    │ apply_prompt_ │  │ format_row_as_   │
    │ template()   │  │ text()           │
    │              │  │                  │
    │ 替换 {{col}} │  │ 自动格式化为      │
    │ 为列值       │  │ 键值对文本        │
    └──────┬───────┘  └────────┬─────────┘
           │                   │
           ▼                   ▼
        user_input = 格式化后的提示词文本
                    │
                    ▼
        ┌─────────────────────────────┐
        │  构建 Chat Completion 请求   │
        │  messages = [               │
        │    {system: system_prompt}, │
        │    {user: 历史行1},          │
        │    {assistant: 历史预测1},    │
        │    ...                      │
        │    {user: user_input}       │
        │  ]                          │
        └─────────────────────────────┘
```

### 4.3 有模板时的 Prompt 格式

当配置了 `prompt_template` 时，使用 `apply_prompt_template()` 函数替换占位符：

**配置示例**：
```sql
prompt_template = 'Classify this text: {{content}}'
```

**行数据**：
| content | category (PREDICT) |
|---------|-------------------|
| AI is transforming software | (待预测) |

**生成的 user message**：
```
Classify this text: AI is transforming software
```

**多列模板示例**：
```sql
prompt_template = 'Review of {{product_name}}: {{review_text}}'
```

**行数据**：
| product_name | review_text | sentiment (PREDICT) |
|-------------|------------|---------------------|
| Widget Pro | This product is amazing! | (待预测) |

**生成的 user message**：
```
Review of Widget Pro: This product is amazing!
```

### 4.4 无模板时的自动 Prompt 格式

当未配置 `prompt_template` 时，使用 `format_row_as_text()` 函数自动将整行格式化为键值对文本：

**格式**：
```
Input data:
column1: value1
column2: value2
...
predict_column_name: 
```

**行数据**：
| content | author | category (PREDICT) |
|---------|--------|-------------------|
| AI is transforming software | John | (待预测) |

**生成的 user message**：
```
Input data:
content: AI is transforming software
author: John
category: 
```

注意：
- PREDICT 列本身也会出现在格式化文本中，但值为空（等待 LLM 填充）
- 隐藏列（`_predict`、`_actual`、`_embedding`）不会出现
- NULL 值的列不会出现
- 非 text 类型列自动使用类型的输出函数转换为文本

### 4.5 历史对话的 Prompt 格式

当 `history_count > 0` 时，历史行也使用相同的格式化方式：

**历史 user message**（每行一条）：
```
Input data:
content: <历史行的content值>
author: <历史行的author值>
category: 
```

**历史 assistant message**（该行的 PREDICT 列值）：
```
technology
```

**完整的 Chat Completion 请求**：
```json
{
  "model": "gpt-3.5-turbo",
  "messages": [
    {"role": "system", "content": "Classify the text..."},
    {"role": "user", "content": "Input data:\ncontent: Previous text 1\nauthor: Alice\ncategory: "},
    {"role": "assistant", "content": "technology"},
    {"role": "user", "content": "Input data:\ncontent: Previous text 2\nauthor: Bob\ncategory: "},
    {"role": "assistant", "content": "sports"},
    {"role": "user", "content": "Input data:\ncontent: AI is transforming software\nauthor: John\ncategory: "}
  ],
  "temperature": 0.7,
  "max_tokens": 1024
}
```

### 4.6 与 MindsDB 的对比

| 特性 | MindsDB | pg_predict |
|------|---------|--------|
| 模板语法 | `{{column_name}}` | `{{column_name}}` |
| 默认模板 | `Answer the following question: {{text}}` | 自动格式化为键值对 |
| 模板填充函数 | `get_completed_prompts()` (Python) | `apply_prompt_template()` (C) |
| 无模板时行为 | 报错 | 自动格式化整行为键值对 |
| 多列支持 | 是 | 是 |
| 历史对话 | conversational 模式 | history_count 配置 |
| 列值转换 | Python str() | PostgreSQL 类型输出函数 |

## 5. 函数设计

### 5.1 llm_infer() - 独立推理函数

**签名1**（2参数）：
```sql
llm_infer(system_prompt text, user_input text) returns text
```

**签名2**（3参数）：
```sql
llm_infer(system_prompt text, history_count integer, user_input text) returns text
```

**行为**：
1. 从 GUC 参数获取 API 配置
2. 构建 OpenAI 兼容的 Chat Completion 请求
3. 使用 libcurl 发送 HTTP POST 请求
4. 解析 JSON 响应，提取 content 字段
5. 返回文本结果

### 5.2 llm_predict() - PREDICT 列默认推理函数

**签名**：
```sql
llm_predict(input_row record) returns text
```

**行为**：
1. 从输入记录的类型 OID 获取表 OID
2. 从 `pg_predict_config` 表读取配置（表级 → 系统级 → GUC 默认值）
3. 提取行中所有非 PREDICT/非隐藏列的值到 `ColValue` 数组
4. 构建 user_input：
   - 如果配置了 `prompt_template`，使用 `apply_prompt_template()` 替换 `{{column_name}}`
   - 否则使用 `format_row_as_text()` 自动格式化整行为键值对文本
5. 如果 `history_count > 0`，使用 SPI 查询历史记录：
   - 查询 `SELECT * FROM {table} ORDER BY ctid DESC LIMIT N`
   - 每行格式化为 user/assistant 对话历史
6. 调用 LLM API
7. 返回文本结果（predict_trigger 负责类型转换）

### 5.3 llm_rag_infer() - RAG 增强推理函数

**签名**：
```sql
llm_rag_infer(
    system_prompt text,
    history_count integer,
    user_input text,
    rag_table regclass,
    rag_similarity float8,
    rag_topn integer
) returns text
```

**参数说明**：

| 参数 | 类型 | 说明 |
|------|------|------|
| system_prompt | text | 系统提示词，指导 LLM 的行为 |
| history_count | integer | 历史对话条数（0 表示不使用历史） |
| user_input | text | 最新用户输入 |
| rag_table | regclass | RAG 检索表（必须有 EMBEDDING 列） |
| rag_similarity | float8 | 最小相似度阈值（余弦距离，0.0-2.0，越小越相似） |
| rag_topn | integer | RAG 检索返回的最大行数 |

**行为**：
1. 从 GUC 参数获取 API 配置
2. 查找 RAG 表中的 EMBEDDING 列
3. 使用 SPI 执行向量相似度查询：`SELECT * FROM rag_table ORDER BY embedding_col <=> user_input LIMIT rag_topn`
4. 查询重写器自动将文本距离查询转换为向量距离查询
5. 格式化 RAG 检索结果为上下文文本
6. 将 RAG 上下文与用户输入组合：`{rag_context}\n\n{user_input}`
7. 构建 Chat Completion 请求（含系统提示词、历史对话、组合后的用户输入）
8. 调用 LLM API，返回文本结果

**RAG 检索流程**：
```
user_input → EMBEDDING 列 <=> 'user_input' 查询
                    │
                    ▼
        ┌─────────────────────────────┐
        │  查询重写器自动处理：         │
        │  1. 识别 EMBEDDING 列        │
        │  2. 替换为 _embedding 列      │
        │  3. 调用 embedding_function  │
        │     将文本转为向量            │
        │  4. 替换为 pgvector 距离操作符 │
        └─────────────────────────────┘
                    │
                    ▼
        向量相似度搜索 → Top-N 结果
                    │
                    ▼
        格式化为上下文文本：
        "Retrieved context:
        --- Result 1 ---
        col1: val1
        col2: val2
        --- Result 2 ---
        ..."
```

### 5.4 llm_rag_predict() - RAG 增强 PREDICT 列推理函数

**签名**：
```sql
llm_rag_predict(input_row record) returns text
```

**行为**：
1. 从输入记录的类型 OID 获取表 OID
2. 从 `pg_predict_config` 表读取配置（含 RAG 配置）
3. 提取行中所有非 PREDICT/非隐藏列的值
4. 构建 user_input（与 llm_predict 相同的逻辑）
5. 如果配置了 `rag_table`，执行 RAG 检索：
   - 在 RAG 表上执行向量相似度搜索
   - 将检索结果格式化为上下文
   - 将上下文与 user_input 组合
6. 如果未配置 `rag_table`，退化为 llm_predict 行为
7. 处理历史对话（与 llm_predict 相同的逻辑）
8. 调用 LLM API，返回文本结果

### 5.5 set_predict_config() - 配置函数

**系统级配置**：
```sql
set_predict_config(
    p_api_url text,
    p_api_key text DEFAULT '',
    p_model_name text DEFAULT 'gpt-3.5-turbo',
    p_temperature float8 DEFAULT 0.7,
    p_max_tokens integer DEFAULT 1024,
    p_system_prompt text DEFAULT '',
    p_history_count integer DEFAULT 0,
    p_rag_table regclass DEFAULT NULL,
    p_rag_similarity float8 DEFAULT 0.5,
    p_rag_topn integer DEFAULT 5
) returns void
```

**表级配置**：
```sql
set_predict_config(
    p_table_name regclass,
    p_api_url text,
    p_api_key text DEFAULT '',
    p_model_name text DEFAULT 'gpt-3.5-turbo',
    p_temperature float8 DEFAULT 0.7,
    p_max_tokens integer DEFAULT 1024,
    p_system_prompt text DEFAULT '',
    p_prompt_template text DEFAULT NULL,
    p_history_count integer DEFAULT 0,
    p_rag_table regclass DEFAULT NULL,
    p_rag_similarity float8 DEFAULT 0.5,
    p_rag_topn integer DEFAULT 5
) returns void
```

### 5.6 get_predict_config() - 查询函数

**系统级查询**：
```sql
get_predict_config(
    OUT api_url text,
    OUT api_key text,
    OUT model_name text,
    OUT temperature float8,
    OUT max_tokens integer,
    OUT system_prompt text,
    OUT history_count integer,
    OUT rag_table oid,
    OUT rag_similarity float8,
    OUT rag_topn integer
) returns record
```

**表级查询**：
```sql
get_predict_config(
    p_table_name regclass,
    OUT api_url text,
    OUT api_key text,
    OUT model_name text,
    OUT temperature float8,
    OUT max_tokens integer,
    OUT system_prompt text,
    OUT prompt_template text,
    OUT history_count integer,
    OUT rag_table oid,
    OUT rag_similarity float8,
    OUT rag_topn integer
) returns record
```

## 6. 关键实现细节

### 6.1 HTTP 请求

使用 libcurl 的 easy 接口进行同步 HTTP POST 请求：
- Content-Type: application/json
- Authorization: Bearer {api_key}
- 超时：由 `pg_predict.timeout` GUC 控制
- SSL 验证：默认关闭（适用于自签名证书环境）

### 6.2 JSON 构建

手动构建 JSON 请求体，支持：
- JSON 字符串转义（引号、反斜杠、控制字符）
- OpenAI Chat Completion API 格式
- 系统消息 + 历史消息 + 用户消息

### 6.3 JSON 解析

使用 PostgreSQL 内置的 `jsonb_extract_path_text` 函数通过 SPI 解析响应：
```sql
SELECT jsonb_extract_path_text(response::jsonb, 'choices', '0', 'message', 'content')
```

### 6.4 内存管理

- SPI 上下文中的字符串使用 `SPI_palloc` 分配（在调用者上下文中），确保 `SPI_finish` 后仍有效
- 历史查询使用独立的内存上下文，查询完成后立即释放
- `llm_predict` 中的 `LLMConfig` 结构体必须使用 `memset` 初始化

### 6.5 类型转换

在 `predict_trigger` 中添加了自动类型转换：
- 当 predict 函数返回 `text` 类型但 PREDICT 列期望其他类型时
- 使用目标类型的输入函数进行转换
- 例如：LLM 返回 "499"，PREDICT 列为 INTEGER，自动转换为整数 499

### 6.6 模板填充

`apply_prompt_template()` 函数实现 `{{column_name}}` 语法替换：
1. 逐字符扫描模板字符串
2. 遇到 `{{` 时，查找对应的 `}}`
3. 提取中间的列名
4. 在 `ColValue` 数组中查找匹配的列值
5. 替换占位符为列值
6. 未找到匹配列名时，占位符替换为空字符串

### 6.7 自动行格式化

`format_row_as_text()` 函数将整行格式化为键值对文本：
1. 遍历所有非 PREDICT、非隐藏列
2. 跳过 NULL 值的列
3. 非 text 类型使用类型输出函数转换为文本
4. 格式：`column_name: value\n`
5. 最后追加 PREDICT 列名和冒号（等待 LLM 填充）

### 6.8 历史对话

当 `history_count > 0` 时：
1. 查找 PREDICT 列名（通过 `attpredict` 属性）
2. 使用 SPI 查询 `SELECT * FROM {table} ORDER BY ctid DESC LIMIT N`
3. 每行格式化为 user/assistant 对话历史
4. user 消息包含整行数据（键值对格式）
5. assistant 消息为该行的 PREDICT 列值
6. 按时间正序排列（查询使用 DESC，然后逆序遍历）

### 6.9 RAG 检索

RAG（Retrieval-Augmented Generation）检索增强生成功能：

**查找 EMBEDDING 列**：
`find_embedding_column_name()` 函数遍历表的所有属性，查找 `attembedding` 标志为 true 的列。

**执行向量搜索**：
`do_rag_retrieval()` 函数：
1. 查找 RAG 表的 EMBEDDING 列名
2. 构建 `_embedding` 隐藏列名（`{colname}_embedding`）
3. 调用 `compute_embedding_for_text()` 将用户输入文本转为向量：
   - 通过 `get_embedding_function_oid()` 获取 embedding 函数 OID
   - 调用 embedding 函数（如 `sentence_transformers_embedding`）生成向量 Datum
4. 通过 `vector_datum_to_string()` 将向量 Datum 转为字符串表示
5. 构建 SQL 查询：`SELECT * FROM rag_table ORDER BY {colname}_embedding <=> $1::vector LIMIT rag_topn`
6. 使用 `SPI_execute_with_args` 执行参数化查询（避免 SQL 注入）
7. 格式化检索结果为上下文文本，跳过 EMBEDDING 列、隐藏列和 `_embedding` 后缀列

**上下文注入**：
RAG 检索结果以文本形式注入到用户输入之前：
```
Retrieved context:

--- Result 1 ---
question: What is PostgreSQL?
answer: PostgreSQL is a powerful open source relational database system.

--- Result 2 ---
question: How to create a table?
answer: Use CREATE TABLE statement to define a new table.

用户原始输入
```

**类型转换**：
当 `llm_rag_predict` 作为 PREDICT 列的 predict_function 使用时，`predict_trigger` 自动将 LLM 返回的文本结果转换为目标列类型（与 `llm_predict` 相同的类型转换逻辑）。

## 7. 错误处理

| 场景 | llm_infer 行为 | llm_predict 行为 | llm_rag_infer 行为 |
|------|---------------|-----------------|-------------------|
| API URL 未配置 | ERROR | ERROR | ERROR |
| HTTP 请求失败 | ERROR | WARNING + 返回 NULL | WARNING + 返回 NULL |
| HTTP 状态码 >= 400 | ERROR | WARNING + 返回 NULL | WARNING + 返回 NULL |
| JSON 解析失败 | ERROR | WARNING + 返回 NULL | WARNING + 返回 NULL |
| 配置表不存在 | 使用 GUC 默认值 | 使用 GUC 默认值 | 使用 GUC 默认值 |
| RAG 表无 EMBEDDING 列 | - | - | ERROR |
| RAG 表 OID 无效 | - | - | ERROR |
| rag_table 未配置 | - | 退化为 llm_predict | 退化为 llm_predict |

## 8. 安全考虑

- `pg_predict_config` 表默认 REVOKE ALL FROM PUBLIC，仅创建者可访问
- API 密钥以明文存储在配置表中，建议通过 GRANT/REVOKE 限制访问
- GUC 参数 `pg_predict.api_key` 可能在日志或 `pg_settings` 视图中暴露
- SSL 证书验证默认关闭，生产环境建议启用

## 9. 兼容性

- API 格式兼容 OpenAI Chat Completion API
- 支持任何兼容 OpenAI API 格式的服务（如 vLLM、Ollama、LM Studio 等）
- 需要 PostgreSQL 编译时启用 libcurl 支持（`--with-libcurl`）
