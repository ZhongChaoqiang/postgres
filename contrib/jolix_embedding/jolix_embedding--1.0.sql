-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION jolix_embedding" to load this file. \quit

-- Sentence Transformers embedding function
-- Returns vector type directly (requires pgvector extension)
-- Default model: sentence-transformers/all-MiniLM-L6-v2
CREATE FUNCTION sentence_transformers_embedding(input_text text)
RETURNS vector
AS 'jolix_embedding', 'sentence_transformers_embedding'
LANGUAGE C IMMUTABLE;

COMMENT ON FUNCTION sentence_transformers_embedding(text) IS
'Generate embedding vector using sentence-transformers (default model: all-MiniLM-L6-v2). Returns vector type directly. Can be used as embedding_function in CREATE TABLE.';

-- Short alias for convenience
CREATE FUNCTION st_embedding(input_text text)
RETURNS vector
AS 'jolix_embedding', 'sentence_transformers_embedding'
LANGUAGE C IMMUTABLE;

COMMENT ON FUNCTION st_embedding(text) IS
'Short alias for sentence_transformers_embedding. Generate embedding vector using default model (all-MiniLM-L6-v2).';

-- Sentence Transformers embedding function with custom model
-- Model name can be any HuggingFace model, e.g., 'BAAI/bge-large-en-v1.5'
CREATE FUNCTION sentence_transformers_embedding(input_text text, model_name text)
RETURNS vector
AS 'jolix_embedding', 'sentence_transformers_embedding_with_model'
LANGUAGE C IMMUTABLE;

COMMENT ON FUNCTION sentence_transformers_embedding(text, text) IS
'Generate embedding vector using sentence-transformers with specified model. Model name can be any HuggingFace model. Returns vector type directly.';

-- Short alias for convenience
CREATE FUNCTION st_embedding(input_text text, model_name text)
RETURNS vector
AS 'jolix_embedding', 'sentence_transformers_embedding_with_model'
LANGUAGE C IMMUTABLE;

COMMENT ON FUNCTION st_embedding(text, text) IS
'Short alias for sentence_transformers_embedding. Generate embedding vector using specified model.';

-- Placeholder distance functions for text <-> text operator syntax
-- These allow the parser to accept queries like: ORDER BY content <=> 'search text'
-- The query rewriter will replace them with vector distance operators at execution time
CREATE FUNCTION st_text_cosine_distance(left text, right text)
RETURNS float8
AS 'jolix_embedding', 'st_text_placeholder_distance'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION st_text_l2_distance(left text, right text)
RETURNS float8
AS 'jolix_embedding', 'st_text_placeholder_distance'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION st_text_inner_product_distance(left text, right text)
RETURNS float8
AS 'jolix_embedding', 'st_text_placeholder_distance'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION st_text_l1_distance(left text, right text)
RETURNS float8
AS 'jolix_embedding', 'st_text_placeholder_distance'
LANGUAGE C IMMUTABLE;

-- Placeholder distance operators for EMBEDDING columns
-- These operators enable the syntax: EMBEDDING_column <=> 'search text'
-- The query rewriter (rewrite_embedding) will automatically:
--   1. Replace the EMBEDDING column Var with the _embedding column Var
--   2. Convert the text constant to a vector using the embedding_function
--   3. Replace the text operator with the corresponding vector operator
-- If the rewriter does not activate (e.g., non-EMBEDDING column), these operators
-- will raise an error at execution time.
CREATE OPERATOR <=> (
    LEFTARG = text, RIGHTARG = text, PROCEDURE = st_text_cosine_distance,
    COMMUTATOR = '<=>'
);

CREATE OPERATOR <-> (
    LEFTARG = text, RIGHTARG = text, PROCEDURE = st_text_l2_distance,
    COMMUTATOR = '<->'
);

CREATE OPERATOR <#> (
    LEFTARG = text, RIGHTARG = text, PROCEDURE = st_text_inner_product_distance,
    COMMUTATOR = '<#>'
);

CREATE OPERATOR <+> (
    LEFTARG = text, RIGHTARG = text, PROCEDURE = st_text_l1_distance,
    COMMUTATOR = '<+>'
);

-- FT-Transformer embedding function for VECTORIZE columns
-- Accepts variable number of arguments (multiple columns) and returns a vector
-- Used with CREATE VECTORIZE ... USING ft_transformer_embedding
CREATE FUNCTION ft_transformer_embedding(VARIADIC "any")
RETURNS vector
AS 'jolix_embedding', 'ft_transformer_embedding'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION ft_transformer_embedding(VARIADIC "any") IS
'Generate vector embedding using FT-Transformer model. Accepts multiple column values as input. Used with CREATE VECTORIZE for multi-column vectorization.';
