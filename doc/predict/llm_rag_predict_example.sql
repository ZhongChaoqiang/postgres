-- ============================================================
-- llm_rag_predict 完整使用示例
-- 场景：工厂生产数据智能问答系统
-- 参考 D:\1.txt 中的 RAG 消息流程：
--   用户提问 → 向量检索相似示例/表结构 → LLM 生成 SQL/回答
-- ============================================================

-- 步骤1: 清理旧数据（如果存在）
DROP TABLE IF EXISTS production_qa CASCADE;
DROP TABLE IF EXISTS rag_examples CASCADE;

-- ============================================================
-- 步骤2: 创建 RAG 知识库表（带 EMBEDDING 列）
-- 重要：vector_len 必须与 embedding 函数的输出维度一致
--   sentence_transformers 默认输出 384 维
-- ============================================================
CREATE TABLE rag_examples (
    id SERIAL PRIMARY KEY,
    question TEXT,                          -- 用户问题
    answer TEXT,                            -- 对应的 SQL 或回答
    tables TEXT,                            -- 涉及的表
    content TEXT EMBEDDING                  -- EMBEDDING 列，用于向量检索
) WITH (
    embedding_function = 'sentence_transformers_embedding',
    vector_len = 384                        -- 必须与 embedding 函数维度一致
);

-- 插入示例数据（content 列会自动生成向量）
INSERT INTO rag_examples (question, answer, tables, content) VALUES
('本周的生产情况',
 'SELECT wd.record_date AS 统计日期, wd.produce_quantity AS 产出数量, wd.unqualified_quantity AS 不良数量, wd.plan_quantity AS 计划数量, wd.qualified_rate AS 合格率 FROM dfs_metrics_work_order_daily wd WHERE WEEK(wd.record_date) = WEEK(CURDATE()) AND YEAR(wd.record_date) = YEAR(CURDATE()) HAVING 产出数>0;',
 'dfs_metrics_work_order_daily',
 '本周的生产情况'),

('本周生产完成情况',
 'SELECT DATE(wd.record_date) AS 统计日期, SUM(wd.produce_quantity) AS 产出数量, SUM(wd.unqualified_quantity) AS 不良数量 FROM dfs_metrics_work_order_daily wd WHERE WEEK(wd.record_date) = WEEK(CURDATE()) GROUP BY DATE(wd.record_date);',
 'dfs_metrics_work_order_daily',
 '本周生产完成情况'),

('上周三的生产情况是怎样的',
 'SELECT wd.work_order_number AS 工单号, wd.produce_quantity AS 产出数量, wd.unqualified_quantity AS 不良数量 FROM dfs_metrics_work_order_daily wd WHERE DATE(wd.record_date) = DATE_SUB(CURDATE(), INTERVAL WEEKDAY(CURDATE()) + 3 DAY);',
 'dfs_metrics_work_order_daily',
 '上周三的生产情况是怎样的'),

('昨天生产情况',
 'SELECT wd.record_date AS 统计日期, wd.produce_quantity AS 产出数量, wd.unqualified_quantity AS 不良数量 FROM dfs_metrics_work_order_daily wd WHERE DATE(wd.record_date) = DATE_SUB(CURDATE(), INTERVAL 1 DAY);',
 'dfs_metrics_work_order_daily',
 '昨天生产情况'),

('上周每天的生产情况分布',
 'SELECT DATE(wd.record_date) AS 统计日期, SUM(wd.produce_quantity) AS 产出数量, SUM(wd.unqualified_quantity) AS 不良数量 FROM dfs_metrics_work_order_daily wd WHERE YEARWEEK(wd.record_date) = YEARWEEK(CURDATE() - INTERVAL 1 WEEK) GROUP BY DATE(wd.record_date) ORDER BY 统计日期;',
 'dfs_metrics_work_order_daily',
 '上周每天的生产情况分布');

-- 验证数据插入成功
SELECT id, question, tables FROM rag_examples;

-- ============================================================
-- 步骤3: 创建使用 RAG 的 PREDICT 表
-- llm_rag_predict 作为 predict_function
-- ============================================================
CREATE TABLE production_qa (
    id SERIAL PRIMARY KEY,
    question TEXT,                          -- 用户问题
    sql_result TEXT PREDICT                 -- LLM 生成的 SQL/回答
) WITH (
    predict_timing = immediate,
    predict_function = 'llm_rag_predict'
);

-- ============================================================
-- 步骤4: 配置表级 LLM 参数（含 RAG 配置）
-- ============================================================
SELECT set_predict_config(
    'production_qa'::regclass,              -- 目标表
    'http://localhost:8080/v1/chat/completions',  -- API 地址
    'your-api-key',                         -- API 密钥
    'deepseek-v3',                          -- 模型名称
    0.3,                                    -- 温度（低温度，更确定性）
    4096,                                   -- 最大 token
    '你是一个数据分析SQL生成助手。根据用户问题和检索到的示例，生成对应的MySQL查询SQL。只输出SQL，不要包含任何解释。',  -- 系统提示词
    '{{question}}',                         -- prompt_template
    2,                                      -- 历史对话条数
    'rag_examples'::regclass,               -- RAG 检索表
    0.5,                                    -- RAG 相似度阈值（余弦距离）
    3                                       -- RAG Top-N（检索最相似的3条示例）
);

-- ============================================================
-- 步骤5: 查看配置
-- ============================================================
SELECT * FROM get_predict_config('production_qa'::regclass);

-- ============================================================
-- 步骤6: 使用 llm_rag_predict（通过 INSERT 自动触发）
-- ============================================================
-- 当执行 INSERT 时，predict_trigger 自动调用 llm_rag_predict：
--   1. 从 pg_predict_config 读取配置
--   2. 在 rag_examples 表上执行向量相似度搜索
--      SELECT * FROM rag_examples ORDER BY content <=> '上周生产情况' LIMIT 3
--   3. 查询重写器自动将文本距离转为向量距离
--   4. 格式化检索结果为上下文
--   5. 将上下文 + 用户问题发送给 LLM
--   6. LLM 返回的文本自动写入 sql_result 列
--   7. predict_trigger 自动将文本转为目标列类型

-- INSERT INTO production_qa (question) VALUES ('上周生产情况');
-- sql_result 会自动填充为 LLM 生成的 SQL

-- ============================================================
-- 步骤7: 直接调用 llm_rag_infer（不依赖 PREDICT 列）
-- ============================================================
-- 也可以直接调用 llm_rag_infer，手动指定所有参数：
-- SELECT llm_rag_infer(
--     '你是一个数据分析SQL生成助手。根据用户问题和检索到的示例，生成对应的MySQL查询SQL。只输出SQL，不要包含任何解释。',
--     0,
--     '上周生产情况',
--     'rag_examples'::regclass,
--     0.5,
--     3
-- );

-- ============================================================
-- 完整的消息流程（参考 D:\1.txt）
-- ============================================================
--
-- 1. 用户输入: "上周生产情况"
--
-- 2. RAG 检索（向量相似度搜索）:
--    SELECT * FROM rag_examples ORDER BY content <=> '上周生产情况' LIMIT 3
--    → 检索到最相似的3条示例
--
-- 3. 格式化上下文:
--    "Retrieved context:
--     --- Result 1 ---
--     question: 本周的生产情况
--     answer: SELECT wd.record_date AS 统计日期, ...
--     tables: dfs_metrics_work_order_daily
--     --- Result 2 ---
--     question: 本周生产完成情况
--     answer: SELECT DATE(wd.record_date) AS 统计日期, ...
--     tables: dfs_metrics_work_order_daily
--     --- Result 3 ---
--     question: 上周三的生产情况是怎样的
--     answer: SELECT wd.work_order_number AS 工单号, ...
--     tables: dfs_metrics_work_order_daily"
--
-- 4. 组合用户输入:
--    "{上下文}\n\n上周生产情况"
--
-- 5. 发送给 LLM:
--    messages = [
--      {system: "你是一个数据分析SQL生成助手..."},
--      {user: "{上下文}\n\n上周生产情况"}
--    ]
--
-- 6. LLM 返回:
--    "SELECT DATE(wd.record_date) AS 统计日期,
--            SUM(wd.produce_quantity) AS 产出数量,
--            SUM(wd.unqualified_quantity) AS 不良数量
--     FROM dfs_metrics_work_order_daily wd
--     WHERE YEARWEEK(wd.record_date) = YEARWEEK(CURDATE() - INTERVAL 1 WEEK)
--     GROUP BY DATE(wd.record_date)
--     ORDER BY 统计日期;"
--
-- 7. predict_trigger 自动将文本写入 sql_result 列
