# 时序向量表（Timeseries Vector Table）使用指南

> 版本: 1.1  
> 适用: PostgreSQL 18.3 + pgvector + tsvector_funcs 扩展  
> 更新: 2026-09-16

> **前置条件**：安装步骤请参考 [UBUNTU_INSTALL_GUIDE.md](UBUNTU_INSTALL_GUIDE.md)，本指南假设扩展已正确编译安装并加载。

## 目录

- [1. 功能简介](#1-功能简介)
- [2. 快速上手](#2-快速上手)
  - [2.1 第一步：创建源表（TimescaleDB 超表）](#21-第一步创建源表timescaledb-超表)
  - [2.2 第二步：创建向量表](#22-第二步创建向量表)
  - [2.3 写入数据](#23-写入数据)
  - [2.4 手动触发计算](#24-手动触发计算)
  - [2.5 向量相似度查询](#25-向量相似度查询)
- [3. 完整语法参考](#3-完整语法参考)
  - [3.1 创建向量表](#31-创建向量表)
  - [3.1.1 向量表列定义（自动注入，无需手动声明）](#311-向量表列定义自动注入无需手动声明)
  - [3.2 表选项（reloptions）](#32-表选项reloptions)
  - [3.3 向量函数说明](#33-向量函数说明)
  - [3.4 ts2v_moment 输出语义](#34-ts2v_moment-输出语义)
  - [3.5 自定义向量化函数示例](#35-自定义向量化函数示例)
- [4. 函数接口详解](#4-函数接口详解)
  - [4.1 ts2v_moment](#41-ts2v_moment)
  - [4.2 timeseries_vector_run](#42-timeseries_vector_run)
  - [4.3 timeseries_vector_search](#43-timeseries_vector_search)
- [5. GUC 参数](#5-guc-参数)
- [6. 最佳实践](#6-最佳实践)
  - [6.1 bucket_interval 选择](#61-bucket_interval-选择)
  - [6.2 carry_columns 设计](#62-carry_columns-设计)
  - [6.3 vector_len 选择](#63-vector_len-选择)
  - [6.4 性能调优](#64-性能调优)
- [7. 常见问题](#7-常见问题)
- [8. 完整示例](#8-完整示例)

---

## 1. 功能简介

时序向量表功能可以自动按固定时间间隔（时间片）对时序数据进行**向量化计算**，将一段时间窗口内的原始数据转换为高维向量，并存储到独立的向量表中。结合 pgvector 的向量相似度搜索能力，可用于：

- **异常检测**：通过向量相似度找到异常时间段
- **模式识别**：识别历史上的相似走势模式
- **故障定位**：匹配历史故障的指标向量
- **数据压缩**：将海量时序数据压缩为向量表征

核心特性：

| 特性 | 说明 |
|------|------|
| 零侵入 | 基于标准 `CREATE TABLE ... WITH (...)` 语法，无需自定义 DDL |
| 异步触发 | INSERT 时自动触发，通过 LISTEN/NOTIFY + Background Worker 异步计算 |
| 自动列注入 | 指定 `timeseries.source` 后自动注入 `slice_start`、carry 列、向量列等 |
| HNSW 索引 | 自动在向量列上创建 HNSW 向量索引 |
| 幂等计算 | 基于 `(slice_start, carry_columns)` 主键的 ON CONFLICT DO UPDATE |
| 迟到数据处理 | 规则③：已完成时间片收到迟到数据时，延迟 bucket_interval 触发重算 |
| 一键相似度搜索 | `timeseries_vector_search` 封装 bucket 对齐 + 向量查询，未计算的 bucket 自动返回空集 |

---

## 2. 快速上手

### 2.1 第一步：创建源表（TimescaleDB 超表）

本特性主要针对时序数据场景，源表推荐使用 TimescaleDB 的**超表（hypertable）**，它会按时间自动分区，对大批量时序数据有更好的查询性能。

```sql
-- 启用 TimescaleDB 扩展（每个数据库执行一次）
CREATE EXTENSION IF NOT EXISTS timescaledb;

-- 创建源表：IoT 传感器数据（时间 + 设备 + 指标）
CREATE TABLE sensor_data (
    time timestamptz NOT NULL,
    device_id text NOT NULL,
    metric_type text NOT NULL,
    value double precision,
    location text,
    PRIMARY KEY (time, device_id, metric_type)
);

-- 转为 TimescaleDB 超表，按 time 列自动分区（7 天一个 chunk）
SELECT create_hypertable('sensor_data', 'time',
                         chunk_time_interval => INTERVAL '7 days');

-- 可选：按 device_id 做二级维度分区，进一步优化多设备查询
SELECT add_dimension('sensor_data', 'device_id', number_partitions => 4);
```

如果你的时序数据量较小，也可以使用普通 PostgreSQL 表，功能完全一致：

```sql
-- 普通 PostgreSQL 表（小数据量场景）
CREATE TABLE sensor_data (
    time timestamptz NOT NULL,
    device_id text NOT NULL,
    temperature double precision,
    humidity double precision,
    PRIMARY KEY (time, device_id)
);
```

### 2.2 第二步：创建向量表

使用标准 `CREATE TABLE ... WITH (...)` 语法，指定 `timeseries.source` 触发自动列注入。系统自动注入必需列（`slice_start`、carry 列、`embedding`、`_row_count`、`_processed` 等）、主键 `(slice_start, device_id)` 和 HNSW 索引：

```sql
-- 创建向量表（value_column 必需，指定源表中用于向量化的数值列）
CREATE TABLE sensor_vector () WITH (
    timeseries.source = 'sensor_data',
    timeseries.bucket_interval = 3600,
    timeseries.vector_len = 384,
    timeseries.carry_columns = 'device_id',
    timeseries.value_column = 'value'
);

-- 创建后可随时用 ALTER TABLE 调整参数
ALTER TABLE sensor_vector SET (timeseries.bucket_interval = 1800);
ALTER TABLE sensor_vector RESET (timeseries.bucket_interval);  -- 恢复默认

-- 查看当前配置
SELECT opt FROM pg_class, LATERAL unnest(reloptions) AS opt
WHERE relname = 'sensor_vector';
```

### 2.3 写入数据

向源表插入数据，触发器会自动在每条 INSERT 后触发异步向量化计算：

```sql
-- 向传感器源表写入数据（2.1 第一个源表，有 value 列）
INSERT INTO sensor_data (time, device_id, metric_type, value, location)
VALUES 
    ('2025-01-01 10:00:00', 'dev-001', 'temperature', 20.5, 'factory-a'),
    ('2025-01-01 10:01:00', 'dev-001', 'temperature', 20.7, 'factory-a'),
    ('2025-01-01 10:02:00', 'dev-001', 'temperature', 21.0, 'factory-a'),
    ('2025-01-01 10:00:00', 'dev-002', 'temperature', 18.2, 'factory-b');
```

触发器在 INSERT 后自动执行，发送 `pg_notify('tsvector_task', payload_json)` 通知。如果 Background Worker 正在运行，几秒后向量表就会有数据；否则可手动触发（下一步）。

### 2.4 手动触发计算

如果没有运行 Background Worker，或者需要立即计算历史数据：

```sql
SELECT timeseries_vector_run('sensor_vector'::regclass::oid);
```

### 2.5 向量相似度查询

系统提供 `timeseries_vector_search` 封装函数，只需传入目标时间片和 Top-N 即可返回相似时间片：

```sql
-- 查看向量化结果
SELECT slice_start, device_id, _row_count
FROM sensor_vector
ORDER BY slice_start;

-- 相似度搜索：找 2025-01-01 10:00 这个 bucket 下最相似的 5 个其它时间片
SELECT * FROM timeseries_vector_search(
    'sensor_vector',               -- 向量表名
    '2025-01-01 10:00:00',        -- 目标时间片起点（会自动对齐 bucket 边界）
    NULL,                          -- 可选 carry_filter；NULL 表示跨 device 搜索
    5                              -- Top-N
);

-- 限定在某个 device_id 内搜索
SELECT * FROM timeseries_vector_search(
    'sensor_vector',
    '2025-01-01 10:30:45',        -- 10:30:45 会自动对齐到 10:00:00 bucket
    'device_id = ''dev-001''',
    10
);
```

> 若目标时间片还没有计算过向量，函数直接返回空集（不报错、不阻塞）。

---

## 3. 完整语法参考

### 3.1 创建向量表

使用标准 `CREATE TABLE ... WITH (...)` 语法。系统自动注入必需列（`slice_start`、carry 列、向量列、`_processed`、`_row_count` 等）并创建主键和 HNSW 索引。

```sql
CREATE TABLE <vector_table_name> () WITH (
    timeseries.source = '<source_table>',
    timeseries.bucket_interval = <seconds>,
    timeseries.vector_len = <N>,
    timeseries.vector_column = 'embedding',
    timeseries.carry_columns = 'c1,c2',
    timeseries.value_column = 'v'
);
```

#### 3.1.1 向量表列定义（自动注入，无需手动声明）

创建向量表后，系统自动注入以下列。所有列都是**系统自动填充**，用户无需（也不应该）手动 INSERT：

| 列名 | 类型 | 语义 | 何时有值 | 填充方式 |
|---|---|---|---|---|
| **`slice_start`** | timestamptz | 时间片窗口起点（含） | 每次 INSERT 都有 | C 层 SQL 生成：bucket 模式 = `FLOOR(time/bucket_interval)*bucket_interval`；slide 模式 = source 每行真实时间戳 |
| **`slice_end`** | timestamptz | 时间片窗口终点（不含），= `slice_start + bucket_interval` | 每次 INSERT 都有 | 同 slice_start 一起计算 |
| **`[carry_columns...]`** | 与源表同类型 | 用户声明的 carry 列分组值（如 `device_id`、`k`） | 每次 INSERT 都有 | 直接从 source 表 select DISTINCT 带入 |
| **`embedding`**（或 `vector_column` 指定名） | vector(N) | 本时间片聚合后的向量 | 每次 INSERT 都有 | 由 `vectorize_function` 计算（默认 `ts2v_moment`；二级/三级用 `ts2v_pool`） |
| **`_row_count`** | int | 本时间片窗口内实际包含的 source 行数 | 每次 INSERT 都有 | C 层 SQL 里的 `count(*)` |
| **`_processed`** | bool | 是否已完成计算（用于状态标记，默认 `true`） | 每次 INSERT 都有 | 建表 DEFAULT `true` |
| **`_created_at`** | timestamptz | 本行首次写入时间 | 每次 INSERT 都有 | 建表 DEFAULT `now()` |
| **`_data_watermark`** | timestamptz | 本窗口已处理到 source 表的哪个时间点（增量水位线） | **v1.2 占位 NULL，v3 增量重算时填充** | 预留 |
| **`_coverage`** | float8 | 窗口内有效源数据覆盖率（0.0 ~ 1.0），用于检测迟到/缺失 | **v1.2 占位 NULL，v3 质量监控时填充** | 预留 |
| **`_gap_count`** | int | 窗口内时间轴空缺数（比如 18:30-18:40 无数据） | **v1.2 占位 NULL，v3 质量监控时填充** | 预留 |

> **主键**：自动在 `(slice_start, [carry_columns...])` 上创建 UNIQUE 约束。  
> **索引**：自动在向量列上创建 HNSW 索引（用于 `timeseries_vector_search`）。  
> **触发器**：当 `timeseries.enabled = true`（默认）时，在 source 表上注册 AFTER INSERT 触发器，数据写入自动触发重算。

##### 列值示例（verify_l1，bucket=3600s, carry=k）

```
slice_start            slice_end              k  _row_count  embedding
2026-09-29 18:00:00+08 2026-09-29 19:00:00+08 0  50           [0.9906, 0.7427, 0.9901]
2026-09-29 19:00:00+08 2026-09-29 20:00:00+08 0  50           [0.9914, 0.7427, 0.9910]
...
2026-09-29 18:00:00+08 2026-09-29 19:00:00+08 1  50           [0.9906, 0.7427, 0.9901]
...
```

- 同一 `slice_start` 出现两次是因为 carry 列 `k` 有两个值（0 和 1），**不是重复行**
- bucket 模式下 `slice_start` 被 FLOOR 对齐到 bucket_interval；slide 模式下取 source 真实时间戳（不对齐）
- `_row_count = 50` 表示该 bucket 内有 50 条源表记录被聚合进 embedding

### 3.2 表选项（reloptions）

| 选项 | 类型 | 必需 | 默认值 | 说明 |
|------|------|------|--------|------|
| `timeseries.source` | text | ✅ | — | 关联的原始时序表名 |
| `timeseries.time_column` | text | — | `time` | 源表中表示时间的列名。把向量表当下一级 source 时需设为 `slice_start` |
| `timeseries.bucket_interval` | int | — | 3600 | 时间片间隔（秒），如 60/3600/86400 |
| `timeseries.vector_len` | int | — | 384 | 输出向量维度 |
| `timeseries.vector_column` | text | — | `embedding` | 向量列名 |
| `timeseries.carry_columns` | text | — | 空 | 分组列名（逗号分隔） |
| `timeseries.value_column` | text | ✅ | — | 源表中用于向量化的数值列名（如 `value`、`_row_count`） |
| `timeseries.vectorize_function` | text | — | `ts2v_moment` | 向量化函数名，签名 `fn(float8[], int) RETURNS vector`。vector 类型 value_column 用 `ts2v_pool` |
| `timeseries.window_mode` | text | — | `bucket` | 窗口模式：`bucket`（硬切分，默认）或 `slide`（滑动窗口）。**🚫 slide 禁止在原始时序表上使用，必须配合一级 tumbling 向量表** |

### 3.3 向量函数说明

**内置默认函数 `ts2v_moment`**，可通过 `timeseries.vectorize_function` reloption 替换为用户自定义函数。

**函数签名要求**：`RETURNS vector`，接受两个参数：
```sql
my_vectorize(float8[] values, int target_dim) RETURNS vector
```

| 参数 | 类型 | 说明 |
|------|------|------|
| values | float8[] | 同一 bucket 内的数值序列（已按 time 排序） |
| target_dim | int | `timeseries.vector_len` 的值 |

### 3.4 `ts2v_moment` 输出语义

当 reloption 未设置或设为 `ts2v_moment` 时使用。前 6 维：

| 维度 | 含义 | 公式 |
|------|------|------|
| [0] | 均值 | `softsign(mean(v))` |
| [1] | 标准差 | `softsign(stddev(v))` |
| [2] | 最小值 | `softsign(min(v))` |
| [3] | 最大值 | `softsign(max(v))` |
| [4] | 偏度 | `softsign(skewness(v))` |
| [5] | 峰度 | `softsign(kurtosis(v))` |

`softsign(x) = x / (1 + |x|)`，把任意实数映射到 (-1, 1)。

当 `vector_len > 6` 时，剩余维度用**分桶直方图**填充：将 [min, max] 区间等分为 N-6 个桶，统计每个桶内的采样点数量，然后按最大值归一化到 [0, 1]。当 `vector_len < 6` 时截断取前 N 维。

### 3.5 自定义向量化函数示例

```sql
-- 只输出均值（更轻量的向量）
CREATE OR REPLACE FUNCTION vec_mean(vals float8[], target_dim int DEFAULT 3)
RETURNS vector
LANGUAGE SQL IMMUTABLE PARALLEL SAFE
AS $$
  SELECT (ARRAY[
    CASE WHEN target_dim >= 1 THEN (avg(v) / (1 + abs(avg(v)))) ELSE 0 END,
    CASE WHEN target_dim >= 2 THEN 0 ELSE 0 END,
    CASE WHEN target_dim >= 3 THEN 0 ELSE 0 END
  ])::vector
  FROM unnest(vals) AS t(v)
$$;

-- 应用到 vec 表
ALTER TABLE vec SET (
    timeseries.source = 'src',
    timeseries.bucket_interval = 3600,
    timeseries.vector_len = 3,
    timeseries.vector_column = 'embedding',
    timeseries.carry_columns = 'k',
    timeseries.value_column = 'v',
    timeseries.vectorize_function = 'vec_mean'
);
```

### 3.6 内置向量化函数 `ts2v_pool`（向量嵌套）

当 vector table 本身作为 source 建**二级/三级向量表**时，上一级的 `embedding` 列是 `vector` 类型。此时 `array_agg(embedding)` 得到 `vector[]`，无法直接传给 `ts2v_moment`（它只接受 `float8[]`）。系统内置了 `ts2v_pool` 函数来处理这个场景：

```sql
ts2v_pool(vecs vector[], dim int DEFAULT NULL) RETURNS vector
```

| 参数 | 类型 | 说明 |
|------|------|------|
| vecs | vector[] | `array_agg(embedding ORDER BY slice_start)` 的输出 |
| dim | int | 保留签名一致性（当前实现未用于截断） |

实现逻辑（6 行 PL/pgSQL）：

```sql
RETURN (SELECT avg(v) FROM unnest(vecs) AS v);
```

用 pgvector 内置的 `avg(vector)` 聚合把多个向量聚合成一个**中心向量**（centroid）。

#### 完整的向量嵌套建表示例

```sql
-- ============ 第一层：原始时序 → vec ============
CREATE TABLE vec () WITH (
    timeseries.source          = 'src',
    timeseries.bucket_interval = 3600,          -- 1 小时 bucket
    timeseries.carry_columns   = 'k',
    timeseries.value_column    = 'v',            -- 原始数值列
    timeseries.vector_len      = 3
    -- vectorize_function 默认 ts2v_moment
);

-- ============ 第二层：vec → vec_embed ============
-- 把一级向量表当 source，用更大的 bucket 把多个一级 bucket 合并成一个
CREATE TABLE vec_embed () WITH (
    timeseries.source              = 'vec',
    timeseries.time_column         = 'slice_start',    -- ← 指定向量表的时间列
    timeseries.bucket_interval     = 7200,             -- 2 小时 bucket
    timeseries.carry_columns       = 'k',
    timeseries.value_column        = 'embedding',      -- ← vector 类型列
    timeseries.vectorize_function = 'ts2v_pool',       -- ← 关键：pool 而不是 moment
    timeseries.vector_len          = 3
);

SELECT timeseries_vector_run('vec'::regclass);      -- → 5 行（5 个 1h bucket）
SELECT timeseries_vector_run('vec_embed'::regclass); -- → 3 行（合并成 3 个 2h bucket）
```

#### ts2v_moment vs ts2v_pool 选用原则

| 场景 | value_column 类型 | vectorize_function |
|------|------------------|-------------------|
| 原始时序 → 一级向量 | int/bigint/float8（数值） | `ts2v_moment`（默认） |
| 一级向量 → 二级向量 | `vector`（embedding） | **`ts2v_pool`**（必须显式配） |
| 自定义数值向量化 | float8 | 自建 `my_func(float8[], int)` |

如果在建 vec_embed 时**忘了配** `vectorize_function = 'ts2v_pool'`，C 层会报 `function ts2v_moment(vector[], integer) does not exist`，但错误被自动捕获，表现为 `timeseries_vector_run` 返回 0 行而不是崩溃。

### 3.7 滑动窗口（Sliding Window）快速上手

默认硬切分（`window_mode = 'bucket'`）下，时间轴被 `bucket_interval` 整齐切割，每条原始数据只属于一个 bucket。滑动窗口模式（`window_mode = 'slide'`）下，从每条 source 记录各自开一个窗口（大小 = `bucket_interval` 秒），数据可以属于多个窗口（重叠）。

#### 最小示例

```sql
-- 硬切分 vs 滑动窗口对比（src 有 3 个 1h bucket，bucket_interval=7200）
-- bucket: 18:00 → 2行, 20:00 → 1行        = 3 行
-- slide: 18:00 → 2行, 19:00 → 2行, 20:00 → 1行 = 5 行（19:00 窗口重叠！）

CREATE TABLE vec_slide () WITH (
    timeseries.source          = 'src',
    timeseries.bucket_interval = 7200,   -- slide 下即窗口大小
    timeseries.carry_columns   = 'k',
    timeseries.value_column    = 'v',
    timeseries.vector_len      = 3,
    timeseries.window_mode     = 'slide' -- ← 关键参数
);

SELECT timeseries_vector_run('vec_slide'::regclass);
-- 结果: 5 行（而 bucket 只有 3 行）
```

#### 向量表 source + sliding + ts2v_pool

```sql
CREATE TABLE vec2_slide () WITH (
    timeseries.source              = 'vec',
    timeseries.time_column         = 'slice_start',
    timeseries.bucket_interval     = 7200,
    timeseries.carry_columns       = 'k',
    timeseries.value_column        = 'embedding',
    timeseries.vectorize_function = 'ts2v_pool',
    timeseries.vector_len          = 3,
    timeseries.window_mode         = 'slide'
);
SELECT timeseries_vector_run('vec2_slide'::regclass);
```

#### ALTER TABLE 动态切换

```sql
ALTER TABLE vec_slide SET (timeseries.window_mode = 'bucket');  -- 切回硬切分
ALTER TABLE vec_slide SET (timeseries.window_mode = 'slide');   -- 切回滑动
ALTER TABLE vec_slide RESET (timeseries.window_mode);           -- 恢复默认 bucket
```

> 🚫 **严禁在原始时序表上直接使用 sliding 模式**
>
> sliding 的 SQL 基于"每行 source 开一个窗口 + 自连接"，复杂度 O(N²/K)。
> 在 100k 行原始表上跑一次耗时 ~97 秒；而先经过一级 tumbling 聚合
> （bucket_interval 把高频数据压缩到万行级），再在二级向量表上
> sliding，耗时降到 ~1.7 秒（快 **56 倍**）。
>
> **正确姿势链**：
>
> ```
> 原始时序表 ──[tumbling, bucket_interval=3600s]──▶ 一级向量表 (N 行)
>                                                      │
>                                           ┌──────────┴──────────┐
>                                           ▼                     ▼
>                                  二级向量表                 三级向量表
>                                  (sliding, window=7200s)   (sliding, window=86400s)
> ```
>
> ❌ `CREATE TABLE vec () WITH (source = 'raw_timeseries', window_mode = 'slide')` — 禁止
> ✅ `CREATE TABLE vec_l1 () WITH (source = 'raw_timeseries', window_mode = 'bucket', bucket_interval = 3600)`
> ✅ `CREATE TABLE vec_l2 () WITH (source = 'vec_l1', window_mode = 'slide', bucket_interval = 7200, time_column = 'slice_start', vectorize_function = 'ts2v_pool')`

#### 注意事项

| # | 提示 |
|---|------|
| 1 | sliding 模式下 `bucket_interval` 是**窗口大小**，不是切分间隔 |
| 2 | sliding 的 slice_start 来自 source 真实时间戳，**不对齐 FLOOR** |
| 3 | BGW worker 透明支持 sliding，无需额外配置 |
| 4 | source 应为**向量表**（已聚合），禁止原始表直接滑动 |
| 5 | 即使在向量表上，bucket_interval 也应远小于窗口大小以保证 N 足够小 |

---

## 4. 函数接口详解

### 4.1 ts2v_moment

```sql
ts2v_moment(float8[] input_array, int target_dim DEFAULT 384)
RETURNS vector
```

**功能**：将一段时序数据（float8 数组）转换为固定维度的向量。

**算法**：
1. 统计基础量：均值、标准差、最小值、最大值
2. 计算 3/4 阶标准化矩（偏度 skewness、峰度 kurtosis）
3. soft-sign 有界归一化 `f(v) = v / (1 + |v|)`，映射到 (-1, 1)
4. 前 6 维：全局统计特征（均值、标准差、最小值、最大值、偏度、峰度）
5. `target_dim > 6` 时，剩余维度对 [min, max] 区间做等频分桶，统计各桶样本密度并归一化（分桶直方图特征）
6. `target_dim < 6` 时截断取前 N 维

**属性**：`IMMUTABLE`、`PARALLEL SAFE`

**示例**：

```sql
SELECT ts2v_moment(ARRAY[1.0, 2.0, 3.0, 4.0, 5.0], 3);
-- 返回 [0.667, 0.449, 0.5]  ≈[softsign(mean), softsign(stddev), softsign(min)]
```

### 4.2 timeseries_vector_run

```sql
timeseries_vector_run(oid vector_table)
RETURNS integer
```

**功能**：手动触发一个向量表的批量计算，返回实际写入/更新的行数。参数为向量表的 OID，通常用 `'table_name'::regclass` 隐式转换。内部执行流程详见设计文档。

**示例**：

```sql
-- 推荐调用方式（regclass 隐式转为 oid）
SELECT timeseries_vector_run('sensor_vector'::regclass);
-- 返回 3（实际更新了 3 行）

-- 也可直接传 OID
SELECT timeseries_vector_run('sensor_vector'::regclass::oid);
```

### 4.3 timeseries_vector_search

```sql
timeseries_vector_search(
    vec_table          text,
    target_slice_start timestamptz,
    carry_filter       text DEFAULT NULL,
    top_n              int  DEFAULT 10
) RETURNS TABLE (
    slice_start_out   timestamptz,
    slice_end_out     timestamptz,
    cosine_distance   float8
)
```

**功能**：对一个已创建的时序向量表执行"给定目标时间片、返回最相似的 Top-N 时间片"的搜索。内部自动完成 bucket 对齐、向量存在性检查、carry_filter 拼接和 pgvector `<=>` 排序。

**参数说明**：

| 参数 | 类型 | 必需 | 默认值 | 说明 |
|------|------|------|--------|------|
| `vec_table` | text | ✅ | — | 向量表名，如 `'sensor_vector'` |
| `target_slice_start` | timestamptz | ✅ | — | 目标时间片起点，**自动对齐**到最近的 bucket 边界（FLOOR） |
| `carry_filter` | text | — | NULL | carry 列过滤表达式，如 `'device_id = 123'`；NULL 表示跨 carry 值搜索 |
| `top_n` | int | — | 10 | 返回 Top-N 相似时间片 |

**返回列**：

| 列 | 类型 | 说明 |
|----|------|------|
| `slice_start_out` | timestamptz | 相似时间片起点 |
| `slice_end_out` | timestamptz | 相似时间片终点 |
| `cosine_distance` | float8 | 与目标向量的余弦距离（0=完全相同，2=完全相反） |

**核心行为**：

- **bucket 对齐**：从 reloptions 自动读取 `timeseries.bucket_interval`，用 `FLOOR(epoch / bucket) * bucket` 对齐。即使用户传 `10:30:45+00` 也会对齐到 `10:00:00+00`。
- **未计算向量返回空集**：若目标 bucket（加上 carry_filter）在向量表中没有 `_processed IS TRUE` 的记录，函数直接 `RETURN` 空集，**不报错**。
- **自排除**：结果集中不包含目标 bucket 本身（`slice_start <> target_bucket_start`）。
- **HNSW 索引加速**：排序用 `embedding <=> target_vec`，自动命中创建向量表时就存在的 HNSW 索引，Top-N 近似最近邻搜索。

**示例**：

```sql
-- 基础：跨 device 搜索最相似的 5 个 bucket
SELECT * FROM timeseries_vector_search('sensor_vector', '2025-01-01 10:00:00+00', NULL, 5);

-- 限定在同一 device_id 内
SELECT * FROM timeseries_vector_search('sensor_vector', '2025-01-01 10:30:45+00', 'device_id = ''dev-001''', 10);

-- 未计算的 bucket：返回 0 行
SELECT count(*) FROM timeseries_vector_search('sensor_vector', '2099-01-01 00:00:00+00', NULL, 5);
-- count = 0
```

**设计要点**：实现为纯 PL/pgSQL 函数，动态从 reloptions 读取 `bucket_interval` 和 `vector_column`，避免硬编码，`top_n`、`vec_table` 均通过 `quote_identifier` 包裹防止 SQL 注入。详细设计见设计文档 2.5 节。

## 5. GUC 参数

| 参数 | 默认值 | 范围 | 需重启 | 说明 |
|------|--------|------|--------|------|
| `timeseries.workers` | 1 | 1 ~ 16 | ✅ | Background Worker 进程数量 |
| `timeseries.max_in_flight` | 64 | 4 ~ 1024 | — | 每个 Worker 同时执行的任务上限 |
| `timeseries.trigger_cache_size` | 512 | 16 ~ 8192 | — | 每个后端进程的 LRU 缓存条目数 |

在线调整：

```sql
ALTER SYSTEM SET timeseries.max_in_flight = 128;
SELECT pg_reload_conf();
```

---

## 6. 最佳实践

### 6.1 bucket_interval 选择

| 场景 | 推荐间隔 | 说明 |
|------|----------|------|
| 高频传感器（秒级） | 60 ~ 300 秒 | 保证足够样本 |
| 业务指标（分钟级） | 3600 秒（1 小时） | 常用默认值 |
| 日级汇总 | 86400 秒（1 天） | 日报/能耗分析 |

**经验法则**：bucket_interval 至少包含 20~50 个采样点，ts2v_moment 的统计矩才可靠。

### 6.2 carry_columns 设计

```sql
-- 多设备分组
timeseries.carry_columns = 'device_id'
-- 结果: 每个 (slice_start, device_id) 一组向量
```

### 6.3 vector_len 选择

| 维度 | 适用场景 |
|------|----------|
| 384 | 通用、推荐 |
| 128 | 简单指标 |
| 768+ | 复杂模式 |

ts2v_moment 前 6 维是全局统计特征，剩余维度是分桶直方图。

### 6.4 性能调优

```sql
ALTER SYSTEM SET timeseries.workers = 2;
ALTER SYSTEM SET timeseries.max_in_flight = 128;
SET work_mem = '64MB';
SET max_parallel_workers_per_gather = 4;
```

---

## 7. 常见问题

### Q1: INSERT 后没有向量结果？

```sql
-- 1. 检查触发器
SELECT tgname FROM pg_trigger WHERE tgrelid = 'sensor_data'::regclass;

-- 2. 手动触发
SELECT timeseries_vector_run('sensor_vector'::regclass::oid);

-- 3. 检查向量表
SELECT count(*) FROM sensor_vector;
```

### Q2: Background Worker 不启动？

检查 `shared_preload_libraries = 'tsvector_funcs'`，修改后重启数据库。

### Q3: 触发器导致 INSERT 变慢？

触发器内部只做 `SPI_connect → pg_notify → SPI_finish`，耗时亚毫秒级。

### Q4: 如何处理历史数据？

触发器自动注册，只处理**创建向量表之后**的 INSERT。历史数据：

```sql
SELECT timeseries_vector_run('sensor_vector'::regclass::oid);
```

---

## 8. 完整示例

```sql
-- ====================================================================
-- 0. 启用扩展（TimescaleDB + pgvector + tsvector_funcs）
-- ====================================================================
CREATE EXTENSION IF NOT EXISTS timescaledb;
CREATE EXTENSION IF NOT EXISTS vector;

-- ====================================================================
-- 1. 创建源表（TimescaleDB 超表）
-- ====================================================================
CREATE TABLE iot_sensors (
    time timestamptz NOT NULL,
    device_id text NOT NULL,
    metric_type text NOT NULL,
    value double precision,
    location text,
    PRIMARY KEY (time, device_id, metric_type)
);

SELECT create_hypertable('iot_sensors', 'time',
                         chunk_time_interval => INTERVAL '7 days');

-- ====================================================================
-- 2. 创建向量表（系统自动注入列、主键、HNSW 索引和源表触发器）
-- ====================================================================
CREATE TABLE iot_sensor_vector () WITH (
    timeseries.source = 'iot_sensors',
    timeseries.bucket_interval = 3600,
    timeseries.vector_len = 384,
    timeseries.carry_columns = 'device_id',
    timeseries.value_column = 'value'
);

-- ====================================================================
-- 3. 写入数据（模拟 30 分钟，每 30 秒一条）
-- ====================================================================
INSERT INTO iot_sensors (time, device_id, metric_type, value, location)
SELECT
    '2025-01-01 10:00:00'::timestamptz + (generate_series * interval '30 seconds'),
    'device-' || (generate_series % 5 + 1),
    (ARRAY['temperature', 'pressure', 'vibration'])[1 + (generate_series % 3)],
    random() * 100 + 20,
    'factory-a'
FROM generate_series(0, 360);

-- ====================================================================
-- 4. 手动触发向量计算（若 BGW 已运行则自动触发，可跳过此步）
-- ====================================================================
SELECT timeseries_vector_run('iot_sensor_vector'::regclass::oid);

-- ====================================================================
-- 5. 查看结果
-- ====================================================================
SELECT slice_start, device_id, _row_count FROM iot_sensor_vector ORDER BY slice_start;

-- ====================================================================
-- 6. 清理
-- ====================================================================
DROP TABLE iot_sensor_vector;
DROP TABLE iot_sensors;
```

---

*文档结束。安装步骤见 [UBUNTU_INSTALL_GUIDE.md](UBUNTU_INSTALL_GUIDE.md)；测试结果见 [timeseries_vector_TEST_REPORT.md](timeseries_vector_TEST_REPORT.md)；技术设计见 [timeseries_vector_design.md](timeseries_vector_design.md)。*
