\echo Use "CREATE EXTENSION jolix_predict" to load this file. \quit

CREATE TABLE jolix_llm_config (
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

REVOKE ALL ON jolix_llm_config FROM PUBLIC;
GRANT SELECT, INSERT, UPDATE, DELETE ON jolix_llm_config TO CURRENT_USER;

CREATE TABLE jolix_llm_history (
    id serial PRIMARY KEY,
    table_name text NOT NULL,
    role text NOT NULL CHECK (role IN ('user', 'assistant')),
    content text NOT NULL,
    created_at timestamp with time zone DEFAULT now()
);

CREATE INDEX idx_llm_history_table_time ON jolix_llm_history(table_name, created_at DESC);

REVOKE ALL ON jolix_llm_history FROM PUBLIC;
GRANT SELECT, INSERT, UPDATE, DELETE ON jolix_llm_history TO CURRENT_USER;

CREATE FUNCTION set_llm_config(
    p_api_url text,
    p_api_key text DEFAULT '',
    p_model_name text DEFAULT 'gpt-3.5-turbo',
    p_temperature float8 DEFAULT 0.7,
    p_max_tokens integer DEFAULT 1024,
    p_system_prompt text DEFAULT '',
    p_prompt_template text DEFAULT '',
    p_history_count integer DEFAULT 0,
    p_rag_table regclass DEFAULT NULL,
    p_rag_similarity float8 DEFAULT 0.5,
    p_rag_topn integer DEFAULT 5
) RETURNS void
LANGUAGE plpgsql
AS $$
BEGIN
    INSERT INTO jolix_llm_config (scope, relid, api_url, api_key, model_name, temperature, max_tokens, system_prompt, prompt_template, history_count, rag_table, rag_similarity, rag_topn)
    VALUES ('system', NULL, p_api_url, p_api_key, p_model_name, p_temperature, p_max_tokens, p_system_prompt, p_prompt_template, p_history_count, p_rag_table, p_rag_similarity, p_rag_topn)
    ON CONFLICT (scope, relid) WHERE scope = 'system'
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

COMMENT ON FUNCTION set_llm_config(text, text, text, float8, integer, text, text, integer, regclass, float8, integer) IS
'Set system-level LLM configuration. This applies to all tables unless overridden. p_prompt_template is used as default user_input when llm_infer is called with NULL or empty user_input. p_rag_table specifies the table with EMBEDDING column for RAG retrieval.';

CREATE FUNCTION set_llm_config(
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
    INSERT INTO jolix_llm_config (scope, relid, api_url, api_key, model_name, temperature, max_tokens, system_prompt, prompt_template, history_count, rag_table, rag_similarity, rag_topn)
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

COMMENT ON FUNCTION set_llm_config(regclass, text, text, text, float8, integer, text, text, integer, regclass, float8, integer) IS
'Set table-level LLM configuration. This overrides system-level config for a specific table. p_prompt_template uses {{column_name}} syntax to reference row columns. p_rag_table specifies the table with EMBEDDING column for RAG retrieval.';

CREATE FUNCTION get_llm_config(
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
    FROM jolix_llm_config l
    WHERE l.scope = 'system'
    LIMIT 1;

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

COMMENT ON FUNCTION get_llm_config(OUT text, OUT text, OUT text, OUT float8, OUT integer, OUT text, OUT text, OUT integer, OUT oid, OUT float8, OUT integer) IS
'Get system-level LLM configuration including RAG settings and prompt_template.';

CREATE FUNCTION get_llm_config(
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
    FROM jolix_llm_config l
    WHERE l.scope = 'table' AND l.relid = p_table_name
    LIMIT 1;

    IF NOT FOUND THEN
        SELECT l.api_url, l.api_key, l.model_name, l.temperature, l.max_tokens, l.system_prompt, NULL::text, l.history_count, l.rag_table, l.rag_similarity, l.rag_topn
        INTO rec
        FROM jolix_llm_config l
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

COMMENT ON FUNCTION get_llm_config(regclass, OUT text, OUT text, OUT text, OUT float8, OUT integer, OUT text, OUT text, OUT integer, OUT oid, OUT float8, OUT integer) IS
'Get LLM configuration for a specific table including RAG settings. Falls back to system-level config if no table-level config exists.';

CREATE FUNCTION llm_infer(
    system_prompt text,
    user_input text
) RETURNS text
AS 'jolix_predict', 'llm_infer'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION llm_infer(text, text) IS
'Call LLM API with system prompt and user input. If system_prompt is NULL or empty, uses the configured system_prompt from set_llm_config(). If user_input is NULL or empty, uses the configured prompt_template from set_llm_config(). Uses GUC parameters (jolix_predict.*) for API configuration.';

CREATE FUNCTION llm_infer(
    system_prompt text,
    user_input text,
    history_count integer
) RETURNS text
AS 'jolix_predict', 'llm_infer'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION llm_infer(text, text, integer) IS
'Call LLM API with system prompt, user input, and history count. If system_prompt is NULL or empty, uses the configured system_prompt from set_llm_config(). If user_input is NULL or empty, uses the configured prompt_template from set_llm_config(). History is read from jolix_llm_history table using the default table name.';

CREATE FUNCTION llm_infer(
    system_prompt text,
    user_input text,
    history_count integer,
    table_name text
) RETURNS text
AS 'jolix_predict', 'llm_infer_with_history'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION llm_infer(text, text, integer, text) IS
'Call LLM API with system prompt, user input, history count, and table name. If system_prompt is NULL or empty, uses the configured system_prompt from set_llm_config(). If user_input is NULL or empty, uses the configured prompt_template from set_llm_config(). Reads the last N Q&A pairs from jolix_llm_history where table_name matches, and includes them as conversation history. Automatically records the new Q&A pair to history after inference. Content is truncated to 4096 characters for safety.';

CREATE FUNCTION llm_rag_infer(
    system_prompt text,
    user_input text,
    history_count integer DEFAULT 0
) RETURNS text
AS 'jolix_predict', 'llm_rag_infer'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION llm_rag_infer(text, text, integer) IS
'RAG-enhanced LLM inference function. If system_prompt is NULL or empty, uses the configured system_prompt from set_llm_config(). If user_input is NULL or empty, uses the configured prompt_template from set_llm_config(). Automatically uses the current table (from jolix_predict.current_table) as the RAG table. The current table must have an EMBEDDING column. Performs vector similarity search, retrieves relevant context, and combines it with the system prompt and user input before calling the LLM. RAG parameters (rag_similarity, rag_topn) are read from jolix_llm_config via set_llm_config().';

CREATE FUNCTION record_predict_history(
    p_table_name text,
    p_role text,
    p_content text
) RETURNS void
AS 'jolix_predict', 'record_predict_history'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION record_predict_history(text, text, text) IS
'Record a Q&A history entry to jolix_llm_history table. Parameters: p_table_name - table name to associate the history with; p_role - must be ''user'' or ''assistant''; p_content - the question or answer text. Used by llm_infer with history and llm_predict_ext to automatically record conversation history.';

CREATE FUNCTION clear_predict_history(
    p_table_name text DEFAULT NULL
) RETURNS integer
AS 'jolix_predict', 'clear_predict_history'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION clear_predict_history(text) IS
'Clear Q&A history from jolix_llm_history table. If p_table_name is provided, only clears history for that table. If NULL, clears all history. Returns the number of rows deleted.';

CREATE FUNCTION cleanup_predict_history()
RETURNS integer
AS 'jolix_predict', 'cleanup_predict_history'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION cleanup_predict_history() IS
'Manually clean up expired history records from jolix_llm_history table based on jolix_predict.history_retention_days GUC parameter. Returns the number of rows deleted. Set history_retention_days=0 to keep history forever (default is 7 days).';

CREATE FUNCTION limix_infer() RETURNS text
AS 'jolix_predict', 'limix_infer'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION limix_infer() IS
'Limix local inference function (zero-parameter, no external LLM required): uses vector similarity search (k-NN) to find similar historical rows in the same table, then infers the prediction based on their PREDICT column values. All configuration is through table WITH parameters: limix_model (default: limix-2m), limix_task (default: classification), limix_topn (default: 5). Task types: classification (weighted majority vote), regression (weighted average), anomaly (distance threshold), extraction (nearest neighbor copy).';
