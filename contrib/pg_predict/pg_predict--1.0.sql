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
    p_history_count integer DEFAULT 0
) RETURNS void
LANGUAGE plpgsql
AS $$
BEGIN
    INSERT INTO pg_predict_config (scope, relid, api_url, api_key, model_name, temperature, max_tokens, system_prompt, history_count)
    VALUES ('system', NULL, p_api_url, p_api_key, p_model_name, p_temperature, p_max_tokens, p_system_prompt, p_history_count)
    ON CONFLICT (scope, relid) WHERE scope = 'system'
    DO UPDATE SET
        api_url = EXCLUDED.api_url,
        api_key = EXCLUDED.api_key,
        model_name = EXCLUDED.model_name,
        temperature = EXCLUDED.temperature,
        max_tokens = EXCLUDED.max_tokens,
        system_prompt = EXCLUDED.system_prompt,
        history_count = EXCLUDED.history_count,
        updated_at = now();
END;
$$;

COMMENT ON FUNCTION set_predict_config(text, text, text, float8, integer, text, integer) IS
'Set system-level LLM configuration. This applies to all tables unless overridden.';

CREATE FUNCTION set_predict_config(
    p_table_name regclass,
    p_api_url text,
    p_api_key text DEFAULT '',
    p_model_name text DEFAULT 'gpt-3.5-turbo',
    p_temperature float8 DEFAULT 0.7,
    p_max_tokens integer DEFAULT 1024,
    p_system_prompt text DEFAULT '',
    p_prompt_template text DEFAULT NULL,
    p_history_count integer DEFAULT 0
) RETURNS void
LANGUAGE plpgsql
AS $$
BEGIN
    INSERT INTO pg_predict_config (scope, relid, api_url, api_key, model_name, temperature, max_tokens, system_prompt, prompt_template, history_count)
    VALUES ('table', p_table_name, p_api_url, p_api_key, p_model_name, p_temperature, p_max_tokens, p_system_prompt, p_prompt_template, p_history_count)
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
        updated_at = now();
END;
$$;

COMMENT ON FUNCTION set_predict_config(regclass, text, text, text, float8, integer, text, text, integer) IS
'Set table-level LLM configuration. This overrides system-level config for a specific table. p_prompt_template uses {{column_name}} syntax to reference row columns (e.g., ''Classify: {{content}}''). If not set, the entire row is formatted as key-value pairs.';

CREATE FUNCTION get_predict_config(
    OUT api_url text,
    OUT api_key text,
    OUT model_name text,
    OUT temperature float8,
    OUT max_tokens integer,
    OUT system_prompt text,
    OUT history_count integer
) RETURNS record
LANGUAGE plpgsql STABLE
AS $$
DECLARE
    rec record;
BEGIN
    SELECT l.api_url, l.api_key, l.model_name, l.temperature, l.max_tokens, l.system_prompt, l.history_count
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
END;
$$;

COMMENT ON FUNCTION get_predict_config(OUT text, OUT text, OUT text, OUT float8, OUT integer, OUT text, OUT integer) IS
'Get system-level LLM configuration.';

CREATE FUNCTION get_predict_config(
    p_table_name regclass,
    OUT api_url text,
    OUT api_key text,
    OUT model_name text,
    OUT temperature float8,
    OUT max_tokens integer,
    OUT system_prompt text,
    OUT prompt_template text,
    OUT history_count integer
) RETURNS record
LANGUAGE plpgsql STABLE
AS $$
DECLARE
    rec record;
BEGIN
    SELECT l.api_url, l.api_key, l.model_name, l.temperature, l.max_tokens, l.system_prompt, l.prompt_template, l.history_count
    INTO rec
    FROM pg_predict_config l
    WHERE l.scope = 'table' AND l.relid = p_table_name
    LIMIT 1;

    IF NOT FOUND THEN
        SELECT l.api_url, l.api_key, l.model_name, l.temperature, l.max_tokens, l.system_prompt, NULL::text, l.history_count
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
END;
$$;

COMMENT ON FUNCTION get_predict_config(regclass, OUT text, OUT text, OUT text, OUT float8, OUT integer, OUT text, OUT text, OUT integer) IS
'Get LLM configuration for a specific table. Falls back to system-level config if no table-level config exists.';

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

CREATE FUNCTION llm_predict(
    input_row record
) RETURNS text
AS 'pg_predict', 'llm_predict'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION llm_predict(record) IS
'Default LLM predict function for PREDICT columns. Sends the entire row data to the LLM. Reads configuration from pg_predict_config table. Supports prompt_template with {{column_name}} syntax. If no template is set, the entire row is formatted as key-value pairs. Supports system prompt and history conversations.';
