-- 创建 pgvector 扩展
CREATE EXTENSION IF NOT EXISTS vector;

-- 简单的文本嵌入函数示例
CREATE OR REPLACE FUNCTION my_embedding(row_data record)
RETURNS vector
AS $$
DECLARE
    text_content text;
    result vector;
BEGIN
    -- 从行数据中提取文本内容
    text_content := row_data.content;
    
    -- 返回示例向量（实际应用中应调用真实嵌入模型）
    result := '[0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0]'::vector;
    
    RETURN result;
END;
$$ LANGUAGE plpgsql IMMUTABLE;

-- 创建表
CREATE TABLE documents (
    id int PRIMARY KEY,
    content text EMBEDDING
) WITH (
    vector_len = 10,
    embedding_function = 'my_embedding'
);

-- 插入数据
INSERT INTO documents (id, content) VALUES (1, 'Hello World');

-- 查询结果
SELECT id, content, content_embedding FROM documents;
