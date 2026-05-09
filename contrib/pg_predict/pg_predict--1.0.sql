\echo Use "CREATE EXTENSION pg_predict" to load this file. \quit

CREATE TABLE pg_predict_config (
    id serial PRIMARY KEY,
    scope text NOT NULL DEFAULT 'system',
    relid oid DEFAULT NULL,
    api_url text,
    api_key text DEFAULT '',
    model_name text DEFAULT 'gpt-3.5-turbo',
    temperature float8 DEFAULT 0.7,
    max_tokens integer DEFAULT 1024,
    system_prompt text DEFAULT '',
    prompt_template text DEFAULT NULL,
    history_count integer DEFAULT 0,
    rag_table oid DEFAULT NULL,
    rag_similarity float8 DEFAULT 0.5,
    rag_topn integer DEFAULT 5,
    created_at timestamp with time zone DEFAULT now(),
    updated_at timestamp with time zone DEFAULT now(),
    UNIQUE(scope, relid)
);

REVOKE ALL ON pg_predict_config FROM PUBLIC;
GRANT SELECT, INSERT, UPDATE, DELETE ON pg_predict_config TO CURRENT_USER;

CREATE FUNCTION set_predict_config(
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
) RETURNS void
LANGUAGE plpgsql
AS $$
BEGIN
    INSERT INTO pg_predict_config (scope, relid, api_url, api_key, model_name, temperature, max_tokens, system_prompt, history_count, rag_table, rag_similarity, rag_topn)
    VALUES ('system', NULL, p_api_url, p_api_key, p_model_name, p_temperature, p_max_tokens, p_system_prompt, p_history_count, p_rag_table, p_rag_similarity, p_rag_topn)
    ON CONFLICT (scope, relid) WHERE scope = 'system'
    DO UPDATE SET
        api_url = EXCLUDED.api_url,
        api_key = EXCLUDED.api_key,
        model_name = EXCLUDED.model_name,
        temperature = EXCLUDED.temperature,
        max_tokens = EXCLUDED.max_tokens,
        system_prompt = EXCLUDED.system_prompt,
        history_count = EXCLUDED.history_count,
        rag_table = EXCLUDED.rag_table,
        rag_similarity = EXCLUDED.rag_similarity,
        rag_topn = EXCLUDED.rag_topn,
        updated_at = now();
END;
$$;

COMMENT ON FUNCTION set_predict_config(text, text, text, float8, integer, text, integer, regclass, float8, integer) IS
'Set system-level LLM configuration. This applies to all tables unless overridden. p_rag_table specifies the table with EMBEDDING column for RAG retrieval.';

CREATE FUNCTION set_predict_config(
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
) RETURNS void
LANGUAGE plpgsql
AS $$
BEGIN
    INSERT INTO pg_predict_config (scope, relid, api_url, api_key, model_name, temperature, max_tokens, system_prompt, prompt_template, history_count, rag_table, rag_similarity, rag_topn)
    VALUES ('table', p_table_name, p_api_url, p_api_key, p_model_name, p_temperature, p_max_tokens, p_system_prompt, p_prompt_template, p_history_count, p_rag_table, p_rag_similarity, p_rag_topn)
    ON CONFLICT (scope, relid) WHERE scope = 'table'
    DO UPDATE SET
        api_url = EXCLUDED.api_url,
        api_key = EXCLUDED.api_key,
        model_name = EXCLUDED.model_name,
        temperature = EXCLUDED.temperature,
        max_tokens = EXCLUDED.max_tokens,
        system_prompt = EXCLUDED.system_prompt,
        prompt_template = EXCLUDED.prompt_template,
        history_count = EXCLUDED.history_count,
        rag_table = EXCLUDED.rag_table,
        rag_similarity = EXCLUDED.rag_similarity,
        rag_topn = EXCLUDED.rag_topn,
        updated_at = now();
END;
$$;

COMMENT ON FUNCTION set_predict_config(regclass, text, text, text, float8, integer, text, text, integer, regclass, float8, integer) IS
'Set table-level LLM configuration. This overrides system-level config for a specific table. p_prompt_template uses {{column_name}} syntax to reference row columns. p_rag_table specifies the table with EMBEDDING column for RAG retrieval.';

CREATE FUNCTION get_predict_config(
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
) RETURNS record
LANGUAGE plpgsql STABLE
AS $$
DECLARE
    rec record;
BEGIN
    SELECT l.api_url, l.api_key, l.model_name, l.temperature, l.max_tokens, l.system_prompt, l.history_count, l.rag_table, l.rag_similarity, l.rag_topn
    INTO rec
    FROM pg_predict_config l
    WHERE l.scope = 'system'
    LIMIT 1;

    api_url := rec.api_url;
    api_key := rec.api_key;
    model_name := rec.model_name;
    temperature := rec.temperature;
    max_tokens := rec.max_tokens;
    system_prompt := rec.system_prompt;
    history_count := rec.history_count;
    rag_table := rec.rag_table;
    rag_similarity := rec.rag_similarity;
    rag_topn := rec.rag_topn;
END;
$$;

COMMENT ON FUNCTION get_predict_config(OUT text, OUT text, OUT text, OUT float8, OUT integer, OUT text, OUT integer, OUT oid, OUT float8, OUT integer) IS
'Get system-level LLM configuration including RAG settings.';

CREATE FUNCTION get_predict_config(
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
) RETURNS record
LANGUAGE plpgsql STABLE
AS $$
DECLARE
    rec record;
BEGIN
    SELECT l.api_url, l.api_key, l.model_name, l.temperature, l.max_tokens, l.system_prompt, l.prompt_template, l.history_count, l.rag_table, l.rag_similarity, l.rag_topn
    INTO rec
    FROM pg_predict_config l
    WHERE l.scope = 'table' AND l.relid = p_table_name
    LIMIT 1;

    IF NOT FOUND THEN
        SELECT l.api_url, l.api_key, l.model_name, l.temperature, l.max_tokens, l.system_prompt, NULL::text, l.history_count, l.rag_table, l.rag_similarity, l.rag_topn
        INTO rec
        FROM pg_predict_config l
        WHERE l.scope = 'system'
        LIMIT 1;
    END IF;

    api_url := rec.api_url;
    api_key := rec.api_key;
    model_name := rec.model_name;
    temperature := rec.temperature;
    max_tokens := rec.max_tokens;
    system_prompt := rec.system_prompt;
    prompt_template := rec.prompt_template;
    history_count := rec.history_count;
    rag_table := rec.rag_table;
    rag_similarity := rec.rag_similarity;
    rag_topn := rec.rag_topn;
END;
$$;

COMMENT ON FUNCTION get_predict_config(regclass, OUT text, OUT text, OUT text, OUT float8, OUT integer, OUT text, OUT text, OUT integer, OUT oid, OUT float8, OUT integer) IS
'Get LLM configuration for a specific table including RAG settings. Falls back to system-level config if no table-level config exists.';

CREATE FUNCTION llm_infer(
    system_prompt text,
    user_input text
) RETURNS text
AS 'pg_predict', 'llm_infer'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION llm_infer(text, text) IS
'Call LLM API with system prompt and user input. Uses GUC parameters (pg_predict.*) for API configuration.';

CREATE FUNCTION llm_infer(
    system_prompt text,
    history_count integer,
    user_input text
) RETURNS text
AS 'pg_predict', 'llm_infer'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION llm_infer(text, integer, text) IS
'Call LLM API with system prompt, history count, and user input. Uses GUC parameters (pg_predict.*) for API configuration.';

CREATE FUNCTION llm_predict_ext(
    input_row record
) RETURNS text
AS 'pg_predict', 'llm_predict_ext'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION llm_predict_ext(record) IS
'Default LLM predict function for PREDICT columns (extension implementation). Sends the entire row data to the LLM. Reads configuration from pg_predict_config table. Supports prompt_template with {{column_name}} syntax. If no template is set, the entire row is formatted as key-value pairs. Supports system prompt and history conversations.';

CREATE FUNCTION llm_rag_infer(
    system_prompt text,
    history_count integer,
    user_input text,
    rag_table regclass,
    rag_similarity float8,
    rag_topn integer
) RETURNS text
AS 'pg_predict', 'llm_rag_infer'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION llm_rag_infer(text, integer, text, regclass, float8, integer) IS
'RAG-enhanced LLM inference function. Performs vector similarity search on the specified RAG table (which must have an EMBEDDING column), retrieves relevant context, and combines it with the system prompt and user input before calling the LLM. Parameters: system_prompt - system instruction for the LLM; history_count - number of recent conversation turns to include; user_input - the latest user query; rag_table - table with EMBEDDING column for RAG retrieval; rag_similarity - minimum cosine similarity threshold (0.0-2.0, lower means more similar); rag_topn - maximum number of RAG results to retrieve.';

CREATE FUNCTION llm_rag_predict_ext(
    input_row record
) RETURNS text
AS 'pg_predict', 'llm_rag_predict_ext'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION llm_rag_predict_ext(record) IS
'RAG-enhanced default predict function for PREDICT columns (extension implementation). Reads configuration from pg_predict_config table including rag_table, rag_similarity, rag_topn. Performs vector similarity search on the RAG table to retrieve relevant context, then combines it with the row data and system prompt before calling the LLM. If rag_table is not configured, falls back to llm_predict behavior.';
