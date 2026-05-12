# PREDICT列和PREDICT函数详细设计文档

## 1. 概述

### 1.1 功能简介
PREDICT列和PREDICT函数是PostgreSQL的一个扩展功能，用于在插入数据时自动计算预测值。该功能通过以下组件实现：

- **PREDICT列属性**：标记列为预测列，支持任意基础数据类型
- **PREDICT AS 语法**：使用 `PREDICT AS (expr) STORED` 内联指定预测表达式
- **预测触发器**：自动处理预测逻辑的触发器函数
- **关系选项**：控制预测行为的表级选项
- **EMBEDDING列属性**：标记列为嵌入向量列，自动创建向量存储列

### 1.3 当前实现状态
PREDICT功能的主要实现包括：
- PREDICT列属性标记（attpredict字段）
- 自动创建预测结果列（`_predict`后缀列）
- 自动创建实际值列（`_actual`后缀列）
- 隐藏列机制（atthidden字段，SELECT *时不显示）
- 预测函数查找机制（从pg_attrdef获取内联表达式）
- 预测触发器函数（predict_trigger）
- PREDICT列验证函数（is_predict_column）
- 预测时机控制选项（predict_timing表选项）
- EMBEDDING列属性标记（attembedding字段）
- 自动创建嵌入向量列（`_embedding`后缀列）
- 嵌入向量长度控制选项（embedding_vector_len表选项）

### 1.2 设计目标
- 提供灵活的数据预测机制
- 支持用户自定义预测算法
- 保持与现有PostgreSQL架构的兼容性
- 提供可配置的预测时机控制
- 支持嵌入向量自动存储和管理

## 2. 架构设计

### 2.1 系统架构图

```mermaid
graph TB
    A[SQL语句] --> B[语法解析器]
    
    subgraph "DDL流程"
        B --> C[CREATE TABLE语句]
        C --> D[列定义处理]
        D --> E{是否PREDICT列?}
        E -->|是| E1[设置attpredict标志]
        E1 --> E2[创建_predict隐藏列]
        E2 --> E3[创建_actual隐藏列]
        E3 --> F[创建预测触发器]
        E -->|否| F
    end
    
    subgraph "INSERT/UPDATE流程"
        G[INSERT/UPDATE语句] --> H[预测触发器]
        H --> I[检查PREDICT列]
        I --> I1[同步值到_actual列]
        I1 --> I2{PREDICT列为NULL且timing=immediate?}
        I2 -->|是| J[获取预测函数]
        J --> K[调用预测函数]
        K --> L[保存结果到_predict列]
        L --> M[返回修改后的元组]
        I2 -->|否| M
    end
    
    subgraph "SELECT流程"
        Q[SELECT语句] --> R[查询解析器]
        R --> S[查询优化器]
        S --> T[执行计划生成]
        T --> U[数据读取]
        U --> V{SELECT * ?}
        V -->|是| V1[过滤隐藏列]
        V -->|否| V2[包含所有指定列]
        V1 --> W[结果格式化]
        V2 --> W
        W --> X[返回查询结果]
    end
    
    subgraph "函数管理"
        N[预测函数注册] --> O[函数查找机制]
        O --> P[函数调用]
    end
    
    %% 跨流程连接
    E1 -.-> V
    P -.-> K
    F -.-> H
```

### 2.2 核心组件

#### 2.2.1 PREDICT列属性
- 在`pg_attribute`系统表中添加`attpredict`布尔字段
- 标记该列需要预测处理
- **定义和存储时**：保持原始基础类型（如integer、float等），无需转换为数组
- **写入时**：根据`predict_timing`选项决定是否调用预测函数
- **读取时**：直接返回原始数据，无需特殊处理

#### 2.2.2 自动创建预测结果列
当定义一个PREDICT列时，系统会自动创建两个同类型的隐藏列和一个索引：
- **预测结果列**：原列名 + `_predict` 后缀（如 `value` → `value_predict`）
  - 数据类型：与原PREDICT列相同
  - 隐藏属性：自动设置为隐藏列
  - 用途：存储预测函数的计算结果

- **实际值列**：原列名 + `_actual` 后缀（如 `value` → `value_actual`）
  - 数据类型：与原PREDICT列相同
  - 隐藏属性：自动设置为隐藏列
  - 用途：存储实际值，用于与预测值进行对比分析

- **自动索引**：`{表名}_{列名}_predict_idx`（如 `predictions_value_predict_idx`）
  - 索引类型：B-tree复合索引
  - 索引列：PREDICT列和`_predict`后缀列
  - 用途：加速对PREDICT列和预测结果的联合查询

#### 2.2.3 隐藏列机制
- 在`pg_attribute`系统表中添加`atthidden`布尔字段
- 隐藏列在`SELECT *`展开时被跳过
- 显式指定列名时仍可正常查询隐藏列
- 利用现有的`p_dontexpand`机制实现（与CTE的SEARCH/CYCLE列相同）

#### 2.2.4 预测触发器
- 自动创建的BEFORE INSERT OR UPDATE触发器
- 处理所有PREDICT列的预测逻辑
- 将用户输入值同步到`_actual`列
- 当PREDICT列值为NULL且`predict_timing`为`immediate`时，调用预测函数并将结果存储到`_predict`列

#### 2.2.5 预测表达式机制
- 通过 `PREDICT AS (expr) STORED` 语法在列定义中内联指定
- 表达式存储在 `pg_attrdef` 系统目录中
- 支持自定义预测算法
- 表达式可以引用同表其他列
- 支持 VOLATILE 函数（如 LLM 推理）

**预测表达式样例代码：**

```sql
-- 示例1：简单的预测表达式，基于其他列计算预测值
CREATE TABLE predictions (
    id SERIAL PRIMARY KEY,
    value integer,
    predicted integer PREDICT AS (value * 10 + 5) STORED
) WITH (predict_timing = immediate);

-- 示例2：使用自定义函数的预测表达式
CREATE OR REPLACE FUNCTION ml_predict(feature float)
RETURNS float AS $$
BEGIN
    RETURN 0.5 * feature + 1.0;
END;
$$ LANGUAGE plpgsql IMMUTABLE;

CREATE TABLE ml_predictions (
    id SERIAL PRIMARY KEY,
    feature float,
    result float PREDICT AS (ml_predict(feature)) STORED
) WITH (predict_timing = immediate);

-- 示例3：使用 Python 通过 PL/Python 调用机器学习模型
CREATE OR REPLACE FUNCTION python_predict(feature float)
RETURNS float AS $$
    import pickle
    import numpy as np
    
    model = pickle.loads(open('/tmp/model.pkl', 'rb').read())
    result = model.predict(np.array([[feature]]))[0]
    return float(result)
$$ LANGUAGE plpython3u IMMUTABLE;

CREATE TABLE py_predictions (
    id SERIAL PRIMARY KEY,
    feature1 float,
    result float PREDICT AS (python_predict(feature1)) STORED
) WITH (predict_timing = immediate);

-- 示例4：多个 predict 列使用不同表达式
CREATE TABLE multi_predict (
    id SERIAL PRIMARY KEY,
    price numeric,
    price_with_tax numeric PREDICT AS (price * 1.1) STORED,
    price_doubled numeric PREDICT AS (price * 2) STORED
) WITH (predict_timing = immediate);
```

#### 2.2.6 查询处理器
- 解析SELECT语句中的PREDICT列引用
- 验证PREDICT列在查询上下文中的合法性
- 优化包含PREDICT列的查询执行计划
- 格式化查询结果中的PREDICT列数据

#### 2.2.7 查询优化器
- 针对PREDICT列的特殊优化策略
- 查询计划缓存和重用机制
- PREDICT列索引优化考虑

### 2.3 代码实现文件

PREDICT功能涉及以下核心代码文件：

| 文件路径 | 功能说明 |
|---------|----------|
| `src/backend/parser/gram.y` | PREDICT关键字语法解析，设置`is_predict`和`is_hidden`标志 |
| `src/backend/parser/parse_utilcmd.c` | 建表时列定义处理，自动创建`_predict`后缀列 |
| `src/backend/parser/parse_target.c` | INSERT时目标列处理，直接保存原始数据 |
| `src/backend/parser/parse_relation.c` | SELECT时列引用处理，隐藏列的`p_dontexpand`设置 |
| `src/backend/commands/tablecmds.c` | 建表逻辑，创建预测触发器，传递`is_hidden`到`atthidden` |
| `src/backend/catalog/heap.c` | 系统表插入，处理`atthidden`字段 |
| `src/backend/access/common/tupdesc.c` | 元组描述符初始化，初始化`atthidden`字段 |
| `src/backend/nodes/makefuncs.c` | `makeColumnDef()`函数，初始化`is_hidden`字段 |
| `src/backend/nodes/copyfuncs.funcs.c` | 节点复制，处理`is_hidden`字段 |
| `src/backend/nodes/equalfuncs.funcs.c` | 节点比较，处理`is_hidden`字段 |
| `src/backend/nodes/outfuncs.funcs.c` | 节点输出，处理`is_hidden`字段 |
| `src/backend/nodes/readfuncs.funcs.c` | 节点读取，处理`is_hidden`字段 |
| `src/backend/utils/adt/predict.c` | 预测触发器函数实现 |
| `src/include/catalog/pg_attribute.h` | `attpredict`和`atthidden`字段定义 |
| `src/include/nodes/parsenodes.h` | `ColumnDef.is_predict`和`ColumnDef.is_hidden`字段定义 |
| `src/include/parser/parse_node.h` | `ParseNamespaceColumn.p_dontexpand`字段定义 |

## 3. 详细设计

### 3.1 数据模型设计

#### 3.1.1 系统表扩展
```sql
-- pg_attribute表扩展
ALTER TABLE pg_attribute ADD COLUMN attpredict boolean NOT NULL DEFAULT false;
ALTER TABLE pg_attribute ADD COLUMN atthidden boolean NOT NULL DEFAULT false;
```

#### 3.1.2 PREDICT列数据结构
```c
// 在FormData_pg_attribute中定义
typedef struct FormData_pg_attribute
{
    // ... 现有字段
    bool        attpredict;     // PREDICT列标记
    bool        atthidden;      // 隐藏列标记（SELECT *时不显示）
} FormData_pg_attribute;

// 在ColumnDef中定义
typedef struct ColumnDef
{
    // ... 现有字段
    bool        is_predict;     // PREDICT选项指定
    bool        is_hidden;      // 隐藏列标记
} ColumnDef;

// PREDICT列的类型处理
// 定义类型：基础类型（如integer、float等）
// 存储类型：保持原始基础类型，无需转换为数组
// 写入时：
//   - deferred模式：直接保存用户输入的数据
//   - immediate模式：调用预测函数后保存结果
// 查询时：直接返回原始数据，无需特殊处理
```

#### 3.1.3 隐藏列机制数据结构
```c
// 在ParseNamespaceColumn中定义
struct ParseNamespaceColumn
{
    // ... 现有字段
    bool        p_dontexpand;   // 不包含在星号展开中
};

// 隐藏列的处理流程：
// 1. pg_attribute.atthidden 存储列的隐藏属性
// 2. buildNSItemFromTupleDesc() 读取 atthidden 并设置 p_dontexpand
// 3. expandNSItemVars() 检查 p_dontexpand，跳过隐藏列
```

### 3.2 语法设计

#### 3.2.1 CREATE TABLE语法扩展
```sql
-- 创建带PREDICT列的表，-- 系统自动创建两个隐藏列：_predict 和 _actual
CREATE TABLE example_table (
    id SERIAL PRIMARY KEY,
    data_value integer PREDICT,  -- PREDICT列
    -- 系统自动创建：
    -- data_value_predict (hidden) - 存储预测结果
    -- data_value_actual (hidden) - 存储实际值
);

-- 查询时，SELECT * 不会返回隐藏列
SELECT * FROM example_table;
-- 结果：id, data_value

-- 显式指定列名时，仍然可以查询隐藏列
SELECT id, data_value, data_value_predict, data_value_actual FROM example_table;
-- 结果：id, data_value, data_value_predict, data_value_actual
```

#### 3.2.2 语法解析规则
在`gram.y`中添加PREDICT关键字支持：
```bison
column_def:
    colid typename col_qual_list
    {
        // ... 现有处理
        if ($3 != NIL && contains_predict_keyword($3))
        {
            // 设置attpredict标志
        }
    }
;

col_qual_list:
    col_qual_list col_qualifier
    | col_qualifier
;

col_qualifier:
    PREDICT { $$ = makePredictQualifier(); }
    | /* 其他限定符 */
;
```

#### 3.2.3 predict_timing表选项语法
在`reloptions.c`中定义`predict_timing`表选项：

```c
/* predict_timing选项的枚举值定义 */
typedef enum StdRdOptPredictTiming
{
    STDRD_OPTION_PREDICT_TIMING_DEFERRED = 0,
    STDRD_OPTION_PREDICT_TIMING_IMMEDIATE,
} StdRdOptPredictTiming;

/* 在StdRdOptions结构体中添加字段 */
typedef struct StdRdOptions
{
    // ... 现有字段
    StdRdOptPredictTiming predict_timing;    /* 控制预测时机 */
} StdRdOptions;

/* 在enumRelOpts数组中添加选项定义 */
static relopt_enum_elt_def StdRdOptPredictTimingValues[] =
{
    {"deferred", STDRD_OPTION_PREDICT_TIMING_DEFERRED},
    {"immediate", STDRD_OPTION_PREDICT_TIMING_IMMEDIATE},
    {(const char *) NULL} /* 列表终止符 */
};

static relopt_enum enumRelOpts[] =
{
    {
        {
            "predict_timing",
            "Controls when prediction operations are performed",
            RELOPT_KIND_HEAP,
            AccessExclusiveLock
        },
        StdRdOptPredictTimingValues,
        STDRD_OPTION_PREDICT_TIMING_DEFERRED,
        gettext_noop("Valid values are \"deferred\" and \"immediate\".")
    },
    // ... 其他选项
};
```

### 3.3 预测触发器设计

#### 3.3.1 触发器创建逻辑
```c
// 在表创建时自动创建预测触发器
void CreatePredictTrigger(Relation rel)
{
    CreateTrigStmt *trigstmt = makeNode(CreateTrigStmt);
    trigstmt->trigname = "predict_trigger";
    trigstmt->relation = rel;
    trigstmt->funcname = list_make1(makeString("predict_trigger"));
    trigstmt->args = NIL;
    trigstmt->row = true;
    trigstmt->timing = TRIGGER_TYPE_BEFORE;
    trigstmt->events = TRIGGER_TYPE_INSERT;
    
    CreateTrigger(trigstmt, NULL, rel->rd_id, InvalidOid, InvalidOid, false);
}
```

#### 3.3.2 增强的触发器执行流程

```mermaid
sequenceDiagram
    participant Client
    participant Parser
    participant Executor
    participant PredictTrigger
    participant PredictFunction
    
    Client->>Parser: INSERT INTO table VALUES (用户数据)
    Parser->>Executor: 解析执行计划
    Executor->>PredictTrigger: 调用BEFORE INSERT触发器
    PredictTrigger->>PredictTrigger: 查找PREDICT列
    PredictTrigger->>PredictTrigger: 获取用户插入的数据
    PredictTrigger->>PredictTrigger: 检查predict_timing选项
    
    alt immediate模式
        PredictTrigger->>PredictFunction: 立即调用预测函数(用户数据)
        PredictFunction->>PredictTrigger: 返回预测结果
        PredictTrigger->>PredictTrigger: 保存预测结果到列
    else deferred模式
        PredictTrigger->>PredictTrigger: 直接保存用户数据
    end
    
    PredictTrigger->>Executor: 返回修改后的元组
    Executor->>Client: 执行完成
```

### 3.3.3 预测函数调用控制

#### 3.3.3.1 实现细节

根据`predict_timing`选项的值，预测触发器会决定是否在INSERT时调用预测函数：

- **immediate模式**：在BEFORE INSERT触发器中立即调用预测函数，将预测结果保存到列中
- **deferred模式**：在BEFORE INSERT触发器中跳过预测函数调用，直接保存用户输入的数据

#### 3.3.3.2 代码实现

```c
Datum predict_trigger(PG_FUNCTION_ARGS)
{
    TriggerData *trigdata = (TriggerData *) fcinfo->context;
    HeapTuple newtuple = trigdata->tg_trigtuple;
    TupleDesc tupdesc = RelationGetDescr(trigdata->tg_relation);
    Relation rel = trigdata->tg_relation;
    
    // 遍历所有列查找PREDICT列
    for (int attnum = 1; attnum <= tupdesc->natts; attnum++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);
        
        if (attr->attpredict && !attr->attisdropped)
        {
            // 获取用户插入的数据
            Datum coldatum = heap_getattr(newtuple, attnum, tupdesc, &isnull);
            if (isnull) continue;
            
            Datum resultdatum;
            
            // 检查predict_timing选项
            StdRdOptions *relopts = (StdRdOptions *) rel->rd_options;
            StdRdOptPredictTiming predict_timing = STDRD_OPTION_PREDICT_TIMING_DEFERRED;
            
            if (relopts != NULL)
                predict_timing = relopts->predict_timing;
            
            if (predict_timing == STDRD_OPTION_PREDICT_TIMING_IMMEDIATE)
            {
                // 立即预测模式：计算预测表达式
                if (attr->attgenerated == ATTRIBUTE_GENERATED_PREDICT)
                {
                    Expr *expr = build_column_default(rel, attnum);
                    if (expr != NULL)
                    {
                        resultdatum = ExecEvalExpr(expr, econtext, &isnull);
                    }
                }
            }
            else
            {
                // 延迟预测模式：直接保存用户数据
                resultdatum = coldatum;
            }
            
            // 更新元组
            newtuple = heap_modify_tuple_by_cols(newtuple, tupdesc, 1, &attnum, &resultdatum, &false);
        }
    }
    
    PG_RETURN_POINTER(newtuple);
}
```

#### 3.3.3.3 行为变化

| 预测时机模式 | 预测函数调用时机 | 数据保存方式 | 性能影响 |
|-------------|-----------------|-------------|----------|
| immediate   | INSERT时立即调用 | 预测函数结果 | 插入时性能开销较大，但实时获得预测结果 |
| deferred    | 不调用           | 用户原始数据 | 插入时性能开销最小 |

### 3.4 预测表达式计算机制

#### 3.4.1 表达式获取与计算逻辑
```c
// PREDICT AS (expr) STORED 语法的表达式存储在 pg_attrdef 中
// 通过 build_column_default 获取表达式并计算

Expr* get_predict_expression(Relation rel, AttrNumber attnum)
{
    // 从 pg_attrdef 获取列的默认表达式
    // 对于 PREDICT AS 列，表达式即为预测表达式
    Expr *expr = build_column_default(rel, attnum);
    return expr;
}

Datum evaluate_predict_expression(Expr *expr, EState *estate, ExprContext *econtext)
{
    // 使用执行器计算表达式
    ExprState *exprstate = ExecPrepareExpr(expr, estate);
    bool isnull;
    Datum result = ExecEvalExpr(exprstate, econtext, &isnull);
    return result;
}
```

### 3.5 查询处理流程

PREDICT列在查询时直接返回原始数据，无需特殊处理：
- PREDICT列保持原始基础类型
- 查询时直接返回列值，与普通列行为一致
- 无需数组下标访问或类型转换

#### 3.4.2 表达式计算流程
```c
Datum evaluate_predict_expression(Expr *expr, EState *estate, ExprContext *econtext)
{
    ExprState *exprstate;
    bool isnull;
    
    exprstate = ExecPrepareExpr(expr, estate);
    return ExecEvalExpr(exprstate, econtext, &isnull);
}
```

## 4. 核心算法实现

PREDICT功能的核心算法包括：

### 4.1 预测触发器算法

#### 4.1.1 预测触发器执行流程
```mermaid
sequenceDiagram
    participant Client
    participant Parser
    participant Executor
    participant PredictTrigger
    participant PredictFunc
    
    Client->>Parser: INSERT INTO table (data_value)
    Parser->>Executor: 解析执行计划
    Executor->>PredictTrigger: 调用BEFORE INSERT触发器
    PredictTrigger->>PredictTrigger: 查找PREDICT列
    PredictTrigger->>PredictTrigger: 获取用户插入的数据
    PredictTrigger->>PredictTrigger: 检查predict_timing选项
    
    alt immediate模式
        PredictTrigger->>PredictFunc: 调用预测函数(用户数据)
        PredictFunc->>PredictTrigger: 返回预测结果
        PredictTrigger->>PredictTrigger: 保存预测结果
    else deferred模式
        PredictTrigger->>PredictTrigger: 直接保存用户数据
    end
    
    PredictTrigger->>Executor: 返回修改后的元组
    Executor->>Client: 执行完成
```

#### 4.1.2 预测触发器算法说明
- 在BEFORE INSERT触发器中处理PREDICT列
- 根据`predict_timing`选项决定预测时机
- **immediate模式**：立即执行预测计算，保存预测结果
- **deferred模式**：直接保存用户输入的数据
- 从表选项中获取预测函数名称
- 调用预测函数处理数据并保存结果

```c
Datum predict_trigger(PG_FUNCTION_ARGS)
{
    TriggerData *trigdata = (TriggerData *) fcinfo->context;
    HeapTuple newtuple = trigdata->tg_trigtuple;
    TupleDesc tupdesc = RelationGetDescr(trigdata->tg_relation);
    Relation rel = trigdata->tg_relation;
    
    // 获取预测时机设置
    StdRdOptions *relopts = (StdRdOptions *) rel->rd_options;
    StdRdOptPredictTiming predict_timing = STDRD_OPTION_PREDICT_TIMING_DEFERRED;
    
    if (relopts != NULL)
        predict_timing = relopts->predict_timing;
    
    // 遍历所有列查找PREDICT列
    for (int attnum = 1; attnum <= tupdesc->natts; attnum++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);
        
        if (attr->attpredict && !attr->attisdropped)
        {
            // 获取用户插入的数据
            Datum coldatum = heap_getattr(newtuple, attnum, tupdesc, &isnull);
            if (isnull) continue;
            
            Datum resultdatum = coldatum;
            
            if (predict_timing == STDRD_OPTION_PREDICT_TIMING_IMMEDIATE)
            {
                // 立即预测模式：计算预测表达式
                if (attr->attgenerated == ATTRIBUTE_GENERATED_PREDICT)
                {
                    Expr *expr = build_column_default(rel, attnum);
                    if (expr != NULL)
                    {
                        resultdatum = ExecEvalExpr(expr, econtext, &isnull);
                    }
                }
            }
            
            // 更新元组
            bool isnull = false;
            newtuple = heap_modify_tuple_by_cols(newtuple, tupdesc, 1, &attnum, &resultdatum, &isnull);
        }
    }
    
    PG_RETURN_POINTER(newtuple);
}
```

### 4.3 预测表达式计算算法

#### 4.3.1 预测表达式计算流程
```mermaid
sequenceDiagram
    participant Trigger
    participant RelOptions
    participant PgAttrdef
    participant Executor
    
    Trigger->>RelOptions: 获取predict_timing
    RelOptions->>Trigger: 返回timing值
    Trigger->>PgAttrdef: 获取预测表达式
    PgAttrdef->>Trigger: 返回表达式
    Trigger->>Executor: 计算表达式(行数据)
    Executor->>Trigger: 返回计算结果
    Trigger->>Trigger: 保存结果到列
    Trigger->>Executor: 返回修改后的元组
```

#### 4.3.2 函数调用流程说明
根据当前代码，预测函数调用流程包括：
- 从表reloptions中提取预测函数名称
- 查找匹配的预测函数OID
- 调用预测函数处理数据
- 将结果直接保存到列中

**函数调用模式**：
- `immediate`模式：立即调用预测函数，保存预测结果
- `deferred`模式：不调用预测函数，保存用户原始数据

## 5. 接口设计

### 5.1 用户接口

#### 5.1.1 SQL接口
```sql
-- 创建带PREDICT列的表
CREATE TABLE sensor_data (
    id SERIAL PRIMARY KEY,
    timestamp TIMESTAMP,
    temperature FLOAT PREDICT AS (temperature * 1.05) STORED
) WITH (predict_timing = immediate);

-- 创建带多个PREDICT列的表（每个列使用不同的表达式）
CREATE TABLE multi_predict_table (
    id SERIAL PRIMARY KEY,
    timestamp TIMESTAMP,
    temperature FLOAT,
    humidity FLOAT,
    pressure FLOAT,
    temp_predicted FLOAT PREDICT AS (temperature * 1.05) STORED,
    humidity_predicted FLOAT PREDICT AS (humidity * 0.95) STORED,
    pressure_predicted FLOAT PREDICT AS (pressure * 1.02) STORED
) WITH (predict_timing = immediate);

-- 插入数据，系统会自动计算预测表达式
INSERT INTO multi_predict_table (timestamp, temperature, humidity, pressure) 
VALUES ('2024-01-01 10:00:00', 25.5, 60.0, 1013.25);

-- 查询预测结果
SELECT id, timestamp, temperature, temp_predicted, temp_predicted_predict,
       humidity, humidity_predicted, humidity_predicted_predict,
       pressure, pressure_predicted, pressure_predicted_predict
FROM multi_predict_table;

-- 使用predict_timing选项控制预测时机
CREATE TABLE sensor_data_deferred (
    id SERIAL PRIMARY KEY,
    timestamp TIMESTAMP,
    temperature FLOAT PREDICT AS (temperature * 1.05) STORED
) WITH (predict_timing = deferred);  -- 延迟执行预测（默认值）

-- 使用ALTER TABLE修改预测时机
ALTER TABLE sensor_data SET (predict_timing = immediate);
ALTER TABLE sensor_data SET (predict_timing = deferred);

-- 重置为默认值
ALTER TABLE sensor_data RESET (predict_timing);

-- 同时设置多个选项
ALTER TABLE sensor_data SET (predict_timing = immediate, fillfactor = 80);

-- 查询PREDICT列状态
SELECT is_predict_column('sensor_data'::regclass, 'temperature');
```

#### 5.1.2 系统函数接口
```c
// 检查列是否为PREDICT列
Datum is_predict_column(PG_FUNCTION_ARGS)
{
    Oid relid = PG_GETARG_OID(0);
    text *colname = PG_GETARG_TEXT_P(1);
    // 实现逻辑...
    PG_RETURN_BOOL(is_predict);
}
```

### 5.2 内部接口

PREDICT功能的内部接口主要包括：
- `is_predict_column()` - 检查列是否为PREDICT列
- `predict_trigger()` - 预测触发器函数

这些函数在predict.c文件中实现，用于支持PREDICT功能的核心逻辑。

## 6. 配置选项

### 6.1 表级选项

PREDICT功能当前支持的表级选项只有 `predict_timing`：

```sql
CREATE TABLE example (
    data INTEGER PREDICT AS (data * 2) STORED
) WITH (predict_timing = immediate);
```

> **注意**：原有的 `predict_function` 表级选项已被移除。所有 predict 列必须使用 `PREDICT AS (expr) STORED` 语法指定推理表达式。

### 6.2 当前配置选项

根据当前代码，PREDICT功能支持的表级选项：

```sql
-- 预测时机控制
CREATE TABLE example (
    data INTEGER PREDICT AS (data * 2) STORED
) WITH (
    predict_timing = deferred      -- 延迟预测（默认）
);
```

### 6.3 predict_timing表选项详细设计

#### 6.3.1 功能概述
`predict_timing`表选项用于控制PREDICT列的预测执行时机，提供两种模式：
- **deferred**（默认）：延迟预测模式，直接保存用户输入的数据
- **immediate**：立即预测模式，在INSERT语句执行时立即执行预测并保存结果

#### 6.3.2 设计原理
```mermaid
graph TB
    A[INSERT语句] --> B{预测时机模式}
    B -->|immediate| C[立即执行预测]
    B -->|deferred| D[直接保存用户数据]
    C --> E[保存预测结果]
    D --> F[保存原始数据]
```

#### 6.3.3 实现机制

**核心数据结构**：
```c
/* 在src/include/utils/rel.h中定义 */
typedef enum StdRdOptPredictTiming
{
    STDRD_OPTION_PREDICT_TIMING_DEFERRED = 0,    /* 延迟预测 */
    STDRD_OPTION_PREDICT_TIMING_IMMEDIATE,       /* 立即预测 */
} StdRdOptPredictTiming;

/* 在StdRdOptions结构体中添加字段 */
typedef struct StdRdOptions
{
    // ... 现有字段
    StdRdOptPredictTiming predict_timing;        /* 预测时机控制 */
} StdRdOptions;
```

**选项注册**：
```c
/* 在src/backend/access/common/reloptions.c中注册 */
static relopt_enum_elt_def StdRdOptPredictTimingValues[] =
{
    {"deferred", STDRD_OPTION_PREDICT_TIMING_DEFERRED},
    {"immediate", STDRD_OPTION_PREDICT_TIMING_IMMEDIATE},
    {(const char *) NULL} /* 列表终止符 */
};

static relopt_enum enumRelOpts[] =
{
    {
        {
            "predict_timing",
            "Controls when prediction operations are performed",
            RELOPT_KIND_HEAP,
            AccessExclusiveLock
        },
        StdRdOptPredictTimingValues,
        STDRD_OPTION_PREDICT_TIMING_DEFERRED,
        gettext_noop("Valid values are \"deferred\" and \"immediate\".")
    },
    // ... 其他选项
};
```

#### 6.3.4 预测时机控制实现机制

**立即预测模式（immediate）实现**：
- 在INSERT语句执行时立即调用预测函数
- 将预测结果直接保存到列中
- 保持原始数据类型不变

**延迟预测模式（deferred）实现**：
- 在INSERT时直接保存用户输入的数据
- 不调用预测函数
- 保持原始数据类型不变

#### 6.3.5 预测触发器实现

**预测触发器算法**：
```c
Datum predict_trigger(PG_FUNCTION_ARGS)
{
    TriggerData *trigdata = (TriggerData *) fcinfo->context;
    HeapTuple newtuple = trigdata->tg_trigtuple;
    TupleDesc tupdesc = RelationGetDescr(trigdata->tg_relation);
    StdRdOptions *relopts = (StdRdOptions *) trigdata->tg_relation->rd_options;
    
    // 获取预测时机设置
    StdRdOptPredictTiming predict_timing = STDRD_OPTION_PREDICT_TIMING_DEFERRED;
    if (relopts && (relopts->predict_timing == STDRD_OPTION_PREDICT_TIMING_IMMEDIATE))
    {
        predict_timing = STDRD_OPTION_PREDICT_TIMING_IMMEDIATE;
    }
    
    // 遍历所有列查找PREDICT列
    for (int attnum = 1; attnum <= tupdesc->natts; attnum++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);
        
        if (attr->attpredict && !attr->attisdropped)
        {
            // 获取用户插入的数据
            Datum coldatum = heap_getattr(newtuple, attnum, tupdesc, &isnull);
            if (isnull) continue;
            
            Datum resultdatum = coldatum;
            
            if (predict_timing == STDRD_OPTION_PREDICT_TIMING_IMMEDIATE)
            {
                // 立即预测模式：计算预测表达式
                if (attr->attgenerated == ATTRIBUTE_GENERATED_PREDICT)
                {
                    Expr *expr = build_column_default(rel, attnum);
                    if (expr != NULL)
                    {
                        resultdatum = ExecEvalExpr(expr, econtext, &isnull);
                    }
                }
            }
            
            // 更新元组
            bool isnull = false;
            newtuple = heap_modify_tuple_by_cols(newtuple, tupdesc, 1, &attnum, &resultdatum, &isnull);
        }
    }
    
    PG_RETURN_POINTER(newtuple);
}
```

#### 6.3.6 使用场景

**立即预测模式（immediate）适用场景**：
- 需要立即获取预测结果的实时应用
- 预测计算开销较小的场景

**延迟预测模式（deferred）适用场景**：
- 批量插入数据的场景
- 对事务性能要求较高的应用

#### 6.3.7 兼容性考虑
- `predict_timing`选项与`PREDICT AS (expr) STORED`语法完全兼容
- 支持CREATE TABLE和ALTER TABLE语法
- 支持与PostgreSQL其他表选项同时使用
- 默认值为`deferred`，确保向后兼容性

### 6.4 关键代码修改点

PREDICT列保持原始数据类型的设计涉及以下关键代码修改：

#### 6.4.1 建表时类型处理（parse_utilcmd.c）

**移除的代码**：不再自动将基础类型转换为数组类型

```c
/* 已移除：不再自动添加数组边界
if (column->is_predict)
{
    if (column->typeName->arrayBounds == NIL)
    {
        column->typeName->arrayBounds = list_make2(makeInteger(2), makeInteger(-1));
    }
}
*/
```

#### 6.4.2 INSERT时数据处理（parse_target.c）

**移除的代码**：不再自动将标量值包装为数组

```c
/* 已移除：不再自动包装为数组 {expr, 0}
if (attr->attpredict)
{
    ArrayExpr *array_expr;
    // ... 创建两元素数组的逻辑
    array_expr->elements = list_make2(orig_expr, zero_elem);
}
*/
```

#### 6.4.3 SELECT时数据处理（parse_relation.c）

**移除的代码**：不再自动添加数组下标访问

```c
/* 已移除：不再自动添加[1]下标访问
if (attr->attpredict)
{
    A_Indirection *ind;
    A_Indices *indices;
    // ... 创建数组下标访问的逻辑
    ind->indirection = list_make1(indices);
    return transformExpr(pstate, (Node *) ind, ...);
}
*/
```

#### 6.4.4 预测触发器（predict.c）

**简化后的代码**：直接保存原始数据或预测结果

```c
/* 简化后的触发器逻辑 */
if (attr->attgenerated == ATTRIBUTE_GENERATED_PREDICT)
{
    Datum coldatum = heap_getattr(newtuple, attnum, tupdesc, &isnull);
    Datum resultdatum;
    
    if (predict_timing == STDRD_OPTION_PREDICT_TIMING_DEFERRED)
    {
        resultdatum = coldatum;  // 直接保存用户数据
    }
    else
    {
        // 计算预测表达式，保存预测结果
        Expr *expr = build_column_default(rel, attnum);
        if (expr != NULL)
        {
            resultdatum = ExecEvalExpr(expr, econtext, &isnull);
        }
    }
    
    // 直接更新元组，无需构造数组
    newtuple = heap_modify_tuple_by_cols(newtuple, tupdesc, 1, &attnum, &resultdatum, &isnull);
}
```

## 7. 错误处理

根据当前代码实现，PREDICT功能的错误处理主要依赖于PostgreSQL的标准错误处理机制：

- 使用标准的`ereport`函数报告错误
- 使用PostgreSQL预定义的错误码
- 在表达式计算过程中处理表达式不存在或计算失败的情况

## 8. 自动创建预测结果列和实际值列

### 8.1 功能概述

当用户定义一个PREDICT列时，系统会自动创建两个同类型的隐藏列：
- **预测结果列**：存储预测表达式的计算结果
- **实际值列**：存储实际值，用于与预测值进行对比分析

### 8.2 列命名规则

| 原PREDICT列名 | 预测结果列名 | 实际值列名 |
|--------------|-------------|-----------|
| `value` | `value_predict` | `value_actual` |
| `temperature` | `temperature_predict` | `temperature_actual` |
| `score` | `score_predict` | `score_actual` |

### 8.3 实现位置

**文件**: `src/backend/parser/parse_utilcmd.c`

**函数**: `transformColumnDefinition()`

```c
/*
 * If this is a PREDICT column, automatically add companion columns:
 * - "_predict" suffix column: stores the prediction result
 * - "_actual" suffix column: stores the actual value for comparison
 * Both columns are hidden from SELECT * expansion.
 */
if (column->is_predict)
{
    ColumnDef  *predict_col;
    ColumnDef  *actual_col;
    char       *predict_colname;
    char       *actual_colname;

    /* Create _predict column */
    predict_colname = psprintf("%s_predict", column->colname);
    predict_col = makeNode(ColumnDef);
    predict_col->colname = predict_colname;
    predict_col->typeName = copyObject(column->typeName);
    predict_col->is_hidden = true;
    // ... 其他字段初始化
    cxt->columns = lappend(cxt->columns, predict_col);

    /* Create _actual column */
    actual_colname = psprintf("%s_actual", column->colname);
    actual_col = makeNode(ColumnDef);
    actual_col->colname = actual_colname;
    actual_col->typeName = copyObject(column->typeName);
    actual_col->is_hidden = true;
    // ... 其他字段初始化
    cxt->columns = lappend(cxt->columns, actual_col);
}
```

### 8.4 列属性继承

预测结果列和实际值列从原PREDICT列继承以下属性：
- 数据类型（`typeName`）
- 排序规则（`collOid`）

两个自动创建的列的独特属性：
- `is_predict = false`：不是PREDICT列
- `is_hidden = true`：隐藏列，SELECT *时不显示

### 8.5 列的用途

| 列名 | 用途 |
|-----|------|
| `value` | 用户输入的原始数据（PREDICT列） |
| `value_predict` | 存储预测函数的计算结果 |
| `value_actual` | 存储实际值，用于与预测值进行对比分析 |

### 8.6 自动创建索引

当创建PREDICT列时，系统会自动在该列和`_predict`后缀列上创建一个复合B-tree索引，以加速查询性能。

**索引命名规则**：`{表名}_{列名}_predict_idx`（如 `predictions_value_predict_idx`）

**实现位置**：`src/backend/parser/parse_utilcmd.c`

```c
/* Create composite index on PREDICT column and _predict column */
index = makeNode(IndexStmt);
index->idxname = psprintf("%s_%s_predict_idx", 
                          cxt->relation->relname, column->colname);
index->relation = copyObject(cxt->relation);
index->accessMethod = pstrdup("btree");
index->unique = false;
index->if_not_exists = true;

/* Create index element for the PREDICT column */
iparam = makeNode(IndexElem);
iparam->name = pstrdup(column->colname);

/* Create index element for the _predict column */
iparam2 = makeNode(IndexElem);
iparam2->name = psprintf("%s_predict", column->colname);

index->indexParams = list_make2(iparam, iparam2);

cxt->alist = lappend(cxt->alist, index);
```

**索引特性**：
- 索引类型：B-tree复合索引
- 索引列：`{列名}`, `{列名}_predict`
- 非唯一索引
- 使用 `IF NOT EXISTS` 避免重复创建错误

**索引用途**：
- 加速对PREDICT列的查询
- 加速对PREDICT列和预测结果的联合查询
- 支持范围查询和排序操作

## 9. 隐藏列机制

### 9.1 功能概述

隐藏列机制允许某些列在`SELECT *`查询时自动被跳过，但仍可通过显式指定列名来查询。

### 9.2 设计原理

隐藏列利用PostgreSQL现有的`p_dontexpand`机制，该机制原本用于CTE的SEARCH和CYCLE子句添加的列。

```mermaid
graph TB
    A[SELECT * 查询] --> B[expandNSItemVars]
    B --> C{检查 p_dontexpand}
    C -->|true| D[跳过该列]
    C -->|false| E[包含该列]
    D --> F[返回结果]
    E --> F
```

### 9.3 数据流程

```
pg_attribute.atthidden
        ↓
buildNSItemFromTupleDesc()
        ↓
ParseNamespaceColumn.p_dontexpand
        ↓
expandNSItemVars() 检查并跳过
```

### 9.4 核心代码

#### 9.4.1 pg_attribute扩展

**文件**: `src/include/catalog/pg_attribute.h`

```c
/* Is hidden from SELECT * expansion or not */
bool        atthidden BKI_DEFAULT(f);
```

#### 9.4.2 解析器列定义扩展

**文件**: `src/include/nodes/parsenodes.h`

```c
typedef struct ColumnDef
{
    // ... 现有字段
    bool        is_hidden;      /* hidden from SELECT * expansion? */
} ColumnDef;
```

#### 9.4.3 命名空间列处理

**文件**: `src/backend/parser/parse_relation.c`

```c
static ParseNamespaceItem *
buildNSItemFromTupleDesc(RangeTblEntry *rte, Index rtindex,
                         RTEPermissionInfo *perminfo,
                         TupleDesc tupdesc)
{
    // ...
    for (varattno = 0; varattno < maxattrs; varattno++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, varattno);
        
        // ...
        
        /* Hidden columns are not included in SELECT * expansion */
        nscolumns[varattno].p_dontexpand = attr->atthidden;
    }
    // ...
}
```

#### 9.4.4 SELECT *展开处理

**文件**: `src/backend/parser/parse_relation.c`

```c
List *
expandNSItemVars(ParseState *pstate, ParseNamespaceItem *nsitem,
                 int sublevels_up, int location,
                 List **colnames)
{
    // ...
    foreach(lc, nsitem->p_names->colnames)
    {
        ParseNamespaceColumn *nscol = nsitem->p_nscolumns + colindex;

        if (nscol->p_dontexpand)
        {
            /* skip */  // 跳过隐藏列
        }
        else if (colname[0])
        {
            // 正常处理列
        }
        // ...
    }
    return result;
}
```

### 9.5 使用示例

```sql
-- 创建带PREDICT列的表
CREATE TABLE predictions (
    id SERIAL PRIMARY KEY,
    value INTEGER PREDICT
);

-- 实际表结构：
-- id (integer, NOT NULL)
-- value (integer)                       -- PREDICT列
-- value_predict (integer, HIDDEN)       -- 自动创建的隐藏列，存储预测结果
-- value_actual (integer, HIDDEN)        -- 自动创建的隐藏列，存储实际值
-- 自动创建的索引：
-- predictions_value_predict_idx (btree复合索引)  -- 自动创建在 (value, value_predict) 列上

-- 查看自动创建的索引
\d predictions
-- 或
SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'predictions';
-- 结果包含：predictions_value_predict_idx (btree复合索引 on value, value_predict)

-- INSERT 操作：用户输入值会自动同步到 _actual 列
INSERT INTO predictions (value) VALUES (100);
-- 触发器自动执行：
-- value = 100 (用户输入)
-- value_actual = 100 (自动同步)

-- INSERT 操作：当 PREDICT 列为 NULL 且 predict_timing = immediate 时
INSERT INTO predictions (id) VALUES (DEFAULT);
-- 或
INSERT INTO predictions (id, value) VALUES (DEFAULT, NULL);
-- 触发器自动执行：
-- value = NULL (用户输入)
-- value_actual = NULL (自动同步)
-- value_predict = 预测表达式计算值

-- UPDATE 操作：同样会同步到 _actual 列
UPDATE predictions SET value = 200 WHERE id = 1;
-- 触发器自动执行：
-- value = 200 (用户输入)
-- value_actual = 200 (自动同步)

-- SELECT * 不返回隐藏列
SELECT * FROM predictions;
-- 结果列：id, value

-- 显式指定可以查询隐藏列
SELECT id, value, value_predict, value_actual FROM predictions;
-- 结果列：id, value, value_predict, value_actual

-- 隐藏列仍然可以用于WHERE条件
SELECT * FROM predictions WHERE value_predict > 100;

-- 计算预测准确度
SELECT 
    id, 
    value,
    value_predict,
    value_actual,
    ABS(value_predict - value_actual) AS prediction_error
FROM predictions;
```

### 9.6 与系统列的对比

| 特性 | 隐藏列（atthidden） | 系统列（如ctid） |
|-----|-------------------|-----------------|
| 存储位置 | 正常用户列区域 | 系统列区域（负数attnum） |
| SELECT * | 不显示 | 不显示 |
| 显式查询 | 支持 | 支持 |
| attnum | 正数 | 负数 |
| 用途 | 存储内部数据 | 系统元数据 |

## 10. 重新构建和初始化

由于修改了`pg_attribute`系统表结构，需要重新构建项目并初始化数据库：

### 10.1 重新构建

```bash
cd /path/to/postgres/build
make clean
make -j4
make install
```

### 10.2 重新初始化数据库

```bash
# 停止数据库服务
pg_ctl stop -D /path/to/data

# 删除旧数据目录
rm -rf /path/to/data/*

# 重新初始化
initdb -D /path/to/data

# 启动数据库
pg_ctl start -D /path/to/data
```

---

**文档版本**: 1.6  
**最后更新**: 2026-04-09  
**作者**: PostgreSQL开发团队