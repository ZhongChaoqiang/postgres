-- ================================================
-- 演示测试脚本：同一表中使用 PREDICT + EMBEDDING + RAG 推理
-- 表: enriched_reviews
-- 功能: 商品评论情感分析(PREDICT) + 评论向量化(EMBEDDING) + RAG智能问答
-- ================================================

-- ============================================
-- 1. 环境准备
-- ============================================

SELECT '========================================' AS info;
SELECT '1. 环境准备：安装扩展 + 配置LLM' AS info;
SELECT '========================================' AS info;

-- 扩展在数据库初始化时已自动安装，无需手动 CREATE EXTENSION

-- 配置 LLM API（系统级）
SELECT set_llm_config(
    p_api_url := 'https://ark.cn-beijing.volces.com/api/v3/chat/completions',
    p_api_key := 'acc96ba1-d743-45d7-9b0e-a415bd96a046',
    p_model_name := 'ep-20251128103853-pp9jw',
    p_temperature := 0.7,
    p_max_tokens := 1024,
    p_system_prompt := 'You are a helpful assistant.',
    p_rag_similarity := 2.0,
    p_rag_topn := 3
);


-- ============================================
-- 2. 创建演示表（PREDICT + EMBEDDING + RAG 三合一）
-- ============================================

SELECT '========================================' AS info;
SELECT '2. 创建 enriched_reviews 表' AS info;
SELECT '========================================' AS info;

DROP TABLE IF EXISTS enriched_reviews CASCADE;
DROP SCHEMA IF EXISTS files CASCADE;
CREATE SCHEMA IF NOT EXISTS files;

CREATE TABLE enriched_reviews (
    id serial PRIMARY KEY,
    product_name text,
    review_text text,

    -- PREDICT: LLM情感分析
    sentiment text PREDICT AS (llm_infer(
        'Classify the sentiment of this product review. Reply with exactly one word: positive, negative, or neutral. No other text.',
        'Product: ' || product_name || '. Review: ' || review_text
    )) STORED,

    -- EMBEDDING: 评论向量化（用于语义搜索和RAG检索）
    review_emb text EMBEDDING AS (st_embedding(review_text)) STORED,

    -- PREDICT + RAG: 基于相似评论上下文的智能问答
    rag_answer text PREDICT AS (llm_rag_infer(
        'Based on the similar reviews provided, answer the question concisely in one or two sentences. If the reviews do not contain relevant information, say so.',
        review_text
    )) STORED
) WITH (
    predict_timing = immediate,
    vector_len = 384
);


-- ============================================
-- 3. 插入知识数据（评论数据，自动生成embedding）
-- ============================================

SELECT '========================================' AS info;
SELECT '3. 插入评论数据（自动生成embedding向量）' AS info;
SELECT '========================================' AS info;

INSERT INTO enriched_reviews (product_name, review_text) VALUES
    ('iPhone 15 Pro', 'The camera quality is outstanding and the battery lasts all day. Best phone I have ever owned!'),
    ('iPhone 15 Pro', 'The price is too high for what you get. The USB-C port is a welcome change though.'),
    ('MacBook Air M3', 'Incredibly fast and lightweight. Perfect for developers. The battery life is amazing.'),
    ('MacBook Air M3', 'The screen could be brighter. Otherwise a solid laptop for everyday use.'),
    ('AirPods Pro 2', 'Noise cancellation is top-notch. Sound quality improved significantly from the previous version.'),
    ('AirPods Pro 2', 'They keep falling out of my ears. The fit is terrible for me.'),
    ('iPad Pro M4', 'The M4 chip makes this incredibly powerful. Great for creative professionals.'),
    ('iPad Pro M4', 'Too expensive for what most people need. The accessories are also overpriced.');

-- 查看情感分析结果和embedding向量
SELECT id, product_name, left(review_text, 40) AS review_preview,
       sentiment,
       left(review_emb_embedding::text, 50) AS emb_preview,
	   rag_answer
FROM enriched_reviews
ORDER BY id;

-- ============================================
-- 4. 语义搜索（EMBEDDING 向量距离查询）
-- 注意：EMBEDDING 列可直接使用 <=> 操作符进行语义搜索，
--       查询重写器会自动将其转换为向量距离计算。
-- ============================================

-- 4.1 Top-K 语义搜索：查找与"价格贵"最相关的3条评论
SELECT id, product_name, left(review_text, 50) AS review_preview,
       round((review_emb <=> 'expensive price')::numeric, 4) AS cosine_distance
FROM enriched_reviews
ORDER BY review_emb <=> 'expensive price'
LIMIT 3;

-- 4.2 带阈值的语义过滤：只返回高度相关的结果（距离 < 0.6）
SELECT id, product_name, left(review_text, 50) AS review_preview,
       round((review_emb <=> 'expensive price')::numeric, 4) AS cosine_distance
FROM enriched_reviews
WHERE review_emb <=> 'expensive price' < 0.6
ORDER BY review_emb <=> 'expensive price';

-- 4.3 语义搜索对比：同一产品不同语义的查询
-- 查找与"电池续航"最相关的评论
SELECT id, product_name, left(review_text, 50) AS review_preview,
       round((review_emb <=> 'battery life')::numeric, 4) AS cosine_distance
FROM enriched_reviews
ORDER BY review_emb <=> 'battery life'
LIMIT 3;

-- 查找与"音质降噪"最相关的评论
SELECT id, product_name, left(review_text, 50) AS review_preview,
       round((review_emb <=> 'sound quality noise cancellation')::numeric, 4) AS cosine_distance
FROM enriched_reviews
ORDER BY review_emb <=> 'sound quality noise cancellation'
LIMIT 3;

-- ============================================
-- 5. RAG 推理（基于相似评论上下文的问答）
-- ============================================

SELECT '========================================' AS info;
SELECT '5. RAG推理' AS info;
SELECT '========================================' AS info;

INSERT INTO enriched_reviews (product_name, review_text) VALUES
    ('iPhone 15 Pro', 'How is the battery life of iPhone 15 Pro?'),
    ('MacBook Air M3', 'Is MacBook Air M3 good for developers?'),
    ('AirPods Pro 2', 'What do users think about the noise cancellation?');

SELECT id, product_name, review_text, rag_answer
FROM enriched_reviews
WHERE rag_answer IS NOT NULL
ORDER BY id;

-- ============================================
-- 6. PREDICT 列直接推理（情感分析）验证
-- ============================================

SELECT '========================================' AS info;
SELECT '6. 验证PREDICT情感分析结果' AS info;
SELECT '========================================' AS info;

-- 统计各情感类别数量
SELECT sentiment, count(*) AS review_count
FROM enriched_reviews
WHERE sentiment IS NOT NULL
GROUP BY sentiment
ORDER BY review_count DESC;

-- 按产品查看情感分布
SELECT product_name,
       count(*) AS total_reviews,
       count(*) FILTER (WHERE sentiment = 'positive') AS positive,
       count(*) FILTER (WHERE sentiment = 'negative') AS negative,
       count(*) FILTER (WHERE sentiment = 'neutral') AS neutral
FROM enriched_reviews
WHERE sentiment IS NOT NULL
GROUP BY product_name
ORDER BY product_name;

-- ============================================
-- 7. 联合查询（PREDICT + EMBEDDING）
-- ============================================

SELECT '========================================' AS info;
SELECT '7. 联合查询：语义搜索 + 情感过滤' AS info;
SELECT '========================================' AS info;

-- 查找与"性能好"相关的正面评论
SELECT id, product_name, left(review_text, 50) AS review_preview,
       sentiment,
       review_emb <=> 'great performance and speed' AS relevance
FROM enriched_reviews
WHERE sentiment = 'positive'
ORDER BY review_emb <=> 'great performance and speed'
LIMIT 5;

-- ============================================
-- 7.5 deferred 模式 SELECT INFER 按需推理演示
-- ============================================

SELECT '========================================' AS info;
SELECT '7.5 deferred模式：INSERT时不推理，SELECT INFER时按需推理' AS info;
SELECT '========================================' AS info;

-- 创建 deferred 模式的表（predict_timing = deferred）
DROP TABLE IF EXISTS deferred_reviews CASCADE;
CREATE TABLE deferred_reviews (
    id serial PRIMARY KEY,
    product_name text,
    review_text text,

    -- PREDICT: LLM情感分析（deferred模式，INSERT时不推理）
    sentiment text PREDICT AS (llm_infer(
        'Classify the sentiment of this product review. Reply with exactly one word: positive, negative, or neutral. No other text.',
        'Product: ' || product_name || '. Review: ' || review_text
    )) STORED
) WITH (predict_timing = deferred);

-- 插入数据（sentiment列为NULL，不触发推理）
INSERT INTO deferred_reviews (product_name, review_text) VALUES
    ('iPhone 15 Pro', 'The camera quality is outstanding and the battery lasts all day.'),
    ('MacBook Air M3', 'The price is too high for what you get.'),
    ('AirPods Pro 2', 'Noise cancellation is top-notch.');

-- 默认 SELECT：跳过推理，sentiment 应为 NULL
SELECT id, product_name, left(review_text, 40) AS review_preview, sentiment
FROM deferred_reviews
ORDER BY id;

-- SELECT INFER：触发按需推理，sentiment 应有推理结果
SELECT INFER id, product_name, left(review_text, 40) AS review_preview, sentiment
FROM deferred_reviews
ORDER BY id;

-- 再次默认 SELECT：验证推理结果已持久化
SELECT id, product_name, sentiment FROM deferred_reviews ORDER BY id;

-- 查看未推理的行数（默认跳过推理，不触发新推理）
-- 此时所有行已推理，应为 0
SELECT count(*) AS pending_count FROM deferred_reviews WHERE sentiment IS NULL;

-- 清理
DROP TABLE IF EXISTS deferred_reviews CASCADE;

