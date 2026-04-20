-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_embedding_st" to load this file. \quit

-- Sentence Transformers embedding function
-- Returns vector type directly (requires pgvector extension)
-- Default model: sentence-transformers/all-MiniLM-L6-v2
CREATE FUNCTION sentence_transformers_embedding(input_text text)
RETURNS vector
AS 'pg_embedding_st', 'sentence_transformers_embedding'
LANGUAGE C IMMUTABLE;

COMMENT ON FUNCTION sentence_transformers_embedding(text) IS
'Generate embedding vector using sentence-transformers (default model: all-MiniLM-L6-v2). Returns vector type directly. Can be used as embedding_function in CREATE TABLE.';

-- Sentence Transformers embedding function returning text
-- Use this if you need text format output
CREATE FUNCTION sentence_transformers_embedding_text(input_text text)
RETURNS text
AS 'pg_embedding_st', 'sentence_transformers_embedding_text'
LANGUAGE C IMMUTABLE;

COMMENT ON FUNCTION sentence_transformers_embedding_text(text) IS
'Generate embedding vector using sentence-transformers, returns text format [0.1, 0.2, ...].';

-- Sentence Transformers embedding function with custom model
-- Model name can be any HuggingFace model, e.g., 'BAAI/bge-large-en-v1.5'
CREATE FUNCTION sentence_transformers_embedding(input_text text, model_name text)
RETURNS vector
AS 'pg_embedding_st', 'sentence_transformers_embedding_with_model'
LANGUAGE C IMMUTABLE;

COMMENT ON FUNCTION sentence_transformers_embedding(text, text) IS
'Generate embedding vector using sentence-transformers with specified model. Model name can be any HuggingFace model. Returns vector type directly.';

-- List available models
CREATE FUNCTION st_embedding_list_models()
RETURNS SETOF text
AS 'pg_embedding_st', 'st_embedding_list_models'
LANGUAGE C;

COMMENT ON FUNCTION st_embedding_list_models() IS
'List available embedding models in the model directory.';
