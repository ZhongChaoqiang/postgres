/*-------------------------------------------------------------------------
 *
 * jolix_predict.c
 *    LLM inference functions for PostgreSQL
 *
 * This module provides:
 * - llm_infer(): Standalone LLM inference function
 * - llm_predict(): Default predict function for PREDICT columns
 * - llm_rag_infer(): RAG-enhanced LLM inference function
 * - llm_rag_predict(): RAG-enhanced predict function for PREDICT columns
 * - Configuration via jolix_llm_config table and GUC parameters
 *
 * The llm_predict function sends the entire row data to the LLM,
 * inspired by MindsDB's approach. It supports:
 * - prompt_template with {{column_name}} syntax (like MindsDB)
 * - Automatic row-to-text formatting when no template is set
 * - History conversations from previous rows
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    contrib/jolix_predict/jolix_predict.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"
#include "utils/array.h"
#include "catalog/pg_class.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/predict.h"
#include "utils/timestamp.h"

#include <curl/curl.h>
#include <string.h>
#include <stdlib.h>
#include <regex.h>
#include <float.h>

PG_MODULE_MAGIC;

static char *jolix_predict_llm_api_url = NULL;
static char *jolix_predict_llm_api_key = NULL;
static char *jolix_predict_llm_model = NULL;
static double jolix_predict_temperature = 0.7;
static int jolix_predict_max_tokens = 1024;
static int jolix_predict_llm_timeout = 60;
static char *jolix_predict_current_table = NULL;
static char *jolix_predict_llm_history_table = NULL;
static int jolix_llm_history_retention_days = 7;
static char *jolix_predict_ldm_current_vector = NULL;

#define MAX_HISTORY_CONTENT_LEN 4096
#define HISTORY_CLEANUP_INTERVAL 3600

typedef struct LLMHttpResponse
{
	char	   *data;
	size_t		size;
} LLMHttpResponse;

typedef struct LLMConfig
{
	char	   *api_url;
	char	   *api_key;
	char	   *model;
	double		temperature;
	int			max_tokens;
	char	   *system_prompt;
	char	   *prompt_template;
	int			history_count;
	Oid			rag_table;
	double		rag_similarity;
	int			rag_topn;
} LLMConfig;

static size_t llm_http_callback(void *contents, size_t size, size_t nmemb, void *userp);
static char *llm_http_post(const char *url, const char *api_key, const char *json_body);
static void append_json_string(StringInfo buf, const char *str);
static char *build_chat_request(const char *model, const char *system_prompt,
								int history_count, char **history_roles, char **history_contents,
								const char *user_input,
								double temperature, int max_tokens);
static char *parse_chat_response(const char *json_response);
static bool read_llm_config(Oid relid, LLMConfig *config);
static void fill_config_defaults(LLMConfig *config);
static void record_predict_history_to_table(const char *table_name, const char *role, const char *content);
static int read_predict_history_from_table(const char *table_name, int count,
										   char ***out_roles, char ***out_contents);
static char *spi_pstrdup(const char *str);
static char *find_embedding_column_name(Oid relid);
static char *do_rag_retrieval(Oid rag_table, const char *user_input,
							  double rag_similarity, int rag_topn);

static char *
spi_pstrdup(const char *str)
{
	Size		len;
	char	   *result;

	if (str == NULL)
		return NULL;

	len = strlen(str) + 1;
	result = SPI_palloc(len);
	memcpy(result, str, len);
	return result;
}

PG_FUNCTION_INFO_V1(llm_infer);
PG_FUNCTION_INFO_V1(llm_infer_with_history);
PG_FUNCTION_INFO_V1(llm_rag_infer);
PG_FUNCTION_INFO_V1(record_predict_history);
PG_FUNCTION_INFO_V1(clear_predict_history);
PG_FUNCTION_INFO_V1(cleanup_predict_history);

static void maybe_cleanup_expired_history(void);

void		_PG_init(void);

void
_PG_init(void)
{
	DefineCustomStringVariable("jolix_predict.llm_api_url",
							   "Default LLM API URL",
							   NULL,
							   &jolix_predict_llm_api_url,
							   "",
							   PGC_USERSET,
							   0,
							   NULL, NULL, NULL);

	DefineCustomStringVariable("jolix_predict.llm_api_key",
							   "Default LLM API key",
							   NULL,
							   &jolix_predict_llm_api_key,
							   "",
							   PGC_USERSET,
							   0,
							   NULL, NULL, NULL);

	DefineCustomStringVariable("jolix_predict.llm_model",
							   "Default LLM model name",
							   NULL,
							   &jolix_predict_llm_model,
							   "",
							   PGC_USERSET,
							   0,
							   NULL, NULL, NULL);

	DefineCustomRealVariable("jolix_predict.temperature",
							 "Default LLM temperature",
							 NULL,
							 &jolix_predict_temperature,
							 0.7, 0.0, 2.0,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomIntVariable("jolix_predict.max_tokens",
							"Default LLM max tokens",
							NULL,
							&jolix_predict_max_tokens,
							1024, 1, 32768,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);

	DefineCustomIntVariable("jolix_predict.llm_timeout",
							"LLM API request timeout in seconds",
							NULL,
							&jolix_predict_llm_timeout,
							60, 1, 600,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);

	DefineCustomStringVariable("jolix_predict.current_table",
							   "Current table name for PREDICT column (set automatically by trigger)",
							   NULL,
							   &jolix_predict_current_table,
							   "",
							   PGC_USERSET,
							   0,
							   NULL, NULL, NULL);

	DefineCustomStringVariable("jolix_predict.llm_history_table",
							   "Default history table name for llm_infer auto-recording. Empty string disables auto-recording.",
							   NULL,
							   &jolix_predict_llm_history_table,
							   "default",
							   PGC_USERSET,
							   0,
							   NULL, NULL, NULL);

	DefineCustomIntVariable("jolix_predict.history_retention_days",
							"Number of days to retain predict history. 0 means keep forever.",
							NULL,
							&jolix_llm_history_retention_days,
							7, 0, 3650,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);

	DefineCustomStringVariable("jolix_predict.ldm_current_vector",
							   "Current EMBEDDINGS vector for ldm_infer (set automatically by trigger)",
							   NULL,
							   &jolix_predict_ldm_current_vector,
							   "",
							   PGC_USERSET,
							   0,
							   NULL, NULL, NULL);
}

static size_t
llm_http_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
	size_t		realsize = size * nmemb;
	LLMHttpResponse *resp = (LLMHttpResponse *) userp;
	char	   *ptr;

	ptr = realloc(resp->data, resp->size + realsize + 1);
	if (!ptr)
		return 0;

	resp->data = ptr;
	memcpy(&(resp->data[resp->size]), contents, realsize);
	resp->size += realsize;
	resp->data[resp->size] = 0;

	return realsize;
}

static char *
llm_http_post(const char *url, const char *api_key, const char *json_body)
{
	CURL	   *curl;
	CURLcode	res;
	LLMHttpResponse response;
	struct curl_slist *headers = NULL;
	char	   *result;
	long		http_code = 0;

	response.data = malloc(1);
	response.size = 0;
	response.data[0] = '\0';

	curl = curl_easy_init();
	if (!curl)
	{
		free(response.data);
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not initialize curl")));
	}

	headers = curl_slist_append(headers, "Content-Type: application/json");
	if (api_key && strlen(api_key) > 0)
	{
		char		auth_header[2048];

		snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);
		headers = curl_slist_append(headers, auth_header);
	}

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, llm_http_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long) jolix_predict_llm_timeout);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long) Min(jolix_predict_llm_timeout, 30));
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

	res = curl_easy_perform(curl);

	if (res != CURLE_OK)
	{
		long		connect_time = 0;
		long		total_time = 0;
		char	   *effective_url = NULL;

		curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &connect_time);
		curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total_time);
		curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);

		char	   *err_msg = psprintf("LLM API request failed: %s (url=%s, connect_time=%.2fs, total_time=%.2fs, timeout=%ds)",
									   curl_easy_strerror(res),
									   effective_url ? effective_url : url,
									   (double) connect_time,
									   (double) total_time,
									   jolix_predict_llm_timeout);

		curl_slist_free_all(headers);
		curl_easy_cleanup(curl);
		free(response.data);
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("%s", err_msg)));
	}

	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	if (http_code >= 400)
	{
		char	   *err_msg = psprintf("LLM API returned HTTP %ld: %s", http_code, response.data);

		curl_slist_free_all(headers);
		curl_easy_cleanup(curl);
		free(response.data);
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("%s", err_msg)));
	}

	result = pstrdup(response.data);

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	free(response.data);

	return result;
}

static void
append_json_string(StringInfo buf, const char *str)
{
	const char *p;

	appendStringInfoCharMacro(buf, '"');
	for (p = str; *p; p++)
	{
		switch (*p)
		{
			case '"':
				appendStringInfoString(buf, "\\\"");
				break;
			case '\\':
				appendStringInfoString(buf, "\\\\");
				break;
			case '\b':
				appendStringInfoString(buf, "\\b");
				break;
			case '\f':
				appendStringInfoString(buf, "\\f");
				break;
			case '\n':
				appendStringInfoString(buf, "\\n");
				break;
			case '\r':
				appendStringInfoString(buf, "\\r");
				break;
			case '\t':
				appendStringInfoString(buf, "\\t");
				break;
			default:
				if ((unsigned char) *p < 0x20)
					appendStringInfo(buf, "\\u%04x", (unsigned char) *p);
				else
					appendStringInfoCharMacro(buf, *p);
		}
	}
	appendStringInfoCharMacro(buf, '"');
}

static char *
build_chat_request(const char *model, const char *system_prompt,
				   int history_count, char **history_roles, char **history_contents,
				   const char *user_input,
				   double temperature, int max_tokens)
{
	StringInfo	buf = makeStringInfo();
	int			i;

	appendStringInfoString(buf, "{\"model\":");
	append_json_string(buf, model);
	appendStringInfoString(buf, ",\"messages\":[");

	if (system_prompt && strlen(system_prompt) > 0)
	{
		appendStringInfoString(buf, "{\"role\":\"system\",\"content\":");
		append_json_string(buf, system_prompt);
		appendStringInfoString(buf, "},");
	}

	for (i = 0; i < history_count; i++)
	{
		appendStringInfoString(buf, "{\"role\":");
		append_json_string(buf, history_roles[i]);
		appendStringInfoString(buf, ",\"content\":");
		append_json_string(buf, history_contents[i]);
		appendStringInfoString(buf, "},");
	}

	appendStringInfoString(buf, "{\"role\":\"user\",\"content\":");
	append_json_string(buf, user_input);
	appendStringInfoString(buf, "}");

	appendStringInfoString(buf, "],\"temperature\":");
	appendStringInfo(buf, "%.2f", temperature);
	appendStringInfoString(buf, ",\"max_tokens\":");
	appendStringInfo(buf, "%d", max_tokens);
	appendStringInfoString(buf, "}");

	return buf->data;
}

static char *
parse_chat_response(const char *json_response)
{
	int			ret;
	char	   *content = NULL;
	Oid			argtypes[1] = {TEXTOID};
	Datum		values[1];
	char		nulls[1] = {' '};

	values[0] = CStringGetTextDatum(json_response);

	ret = SPI_connect();
	if (ret != SPI_OK_CONNECT)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not connect to SPI for JSON parsing")));

	ret = SPI_execute_with_args(
								 "SELECT jsonb_extract_path_text($1::jsonb, 'choices', '0', 'message', 'content')",
								 1, argtypes, values, nulls, true, 1);

	if (ret == SPI_OK_SELECT && SPI_processed > 0)
	{
		bool		isnull;
		Datum		val;

		val = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
		if (!isnull)
			content = pstrdup(TextDatumGetCString(val));
	}

	SPI_finish();
	return content;
}

static void
fill_config_defaults(LLMConfig *config)
{
	if (jolix_predict_llm_api_url && strlen(jolix_predict_llm_api_url) > 0)
	{
		if (config->api_url)
			pfree(config->api_url);
		config->api_url = pstrdup(jolix_predict_llm_api_url);
	}
	else if (config->api_url == NULL)
		config->api_url = pstrdup("");

	if (jolix_predict_llm_api_key && strlen(jolix_predict_llm_api_key) > 0)
	{
		if (config->api_key)
			pfree(config->api_key);
		config->api_key = pstrdup(jolix_predict_llm_api_key);
	}
	else if (config->api_key == NULL)
		config->api_key = pstrdup("");

	if (jolix_predict_llm_model && strlen(jolix_predict_llm_model) > 0)
	{
		if (config->model)
			pfree(config->model);
		config->model = pstrdup(jolix_predict_llm_model);
	}
	else if (config->model == NULL)
		config->model = pstrdup("");

	config->temperature = jolix_predict_temperature;
	config->max_tokens = jolix_predict_max_tokens;

	if (config->system_prompt == NULL)
		config->system_prompt = pstrdup("");
	if (config->prompt_template == NULL)
		config->prompt_template = pstrdup("");
	if (!OidIsValid(config->rag_table))
		config->rag_table = InvalidOid;
	if (config->rag_similarity <= 0.0)
		config->rag_similarity = 0.5;
	if (config->rag_topn <= 0)
		config->rag_topn = 5;
}

static bool
read_llm_config(Oid relid, LLMConfig *config)
{
	int			ret;
	bool		found = false;
	char		query[2048];

	memset(config, 0, sizeof(LLMConfig));

	ret = SPI_connect();
	if (ret != SPI_OK_CONNECT)
	{
		fill_config_defaults(config);
		return false;
	}

	if (OidIsValid(relid))
	{
		snprintf(query, sizeof(query),
				 "SELECT api_url, api_key, model_name, temperature, max_tokens, "
				 "system_prompt, prompt_template, history_count, "
				 "rag_table, rag_similarity, rag_topn "
				 "FROM jolix_llm_config WHERE scope = 'table' AND relid = %u",
				 relid);

		ret = SPI_execute(query, true, 1);
		if (ret == SPI_OK_SELECT && SPI_processed > 0)
			found = true;
	}

	if (!found)
	{
		snprintf(query, sizeof(query),
				 "SELECT api_url, api_key, model_name, temperature, max_tokens, "
				 "system_prompt, prompt_template, history_count, "
				 "rag_table, rag_similarity, rag_topn "
				 "FROM jolix_llm_config WHERE scope = 'system' LIMIT 1");

		ret = SPI_execute(query, true, 1);
		if (ret == SPI_OK_SELECT && SPI_processed > 0)
			found = true;
	}

	if (found)
	{
		HeapTuple	tuple = SPI_tuptable->vals[0];
		TupleDesc	tupdesc = SPI_tuptable->tupdesc;
		bool		isnull;
		Datum		val;

		val = SPI_getbinval(tuple, tupdesc, 1, &isnull);
		config->api_url = isnull ? NULL : spi_pstrdup(TextDatumGetCString(val));

		val = SPI_getbinval(tuple, tupdesc, 2, &isnull);
		config->api_key = isnull ? NULL : spi_pstrdup(TextDatumGetCString(val));

		val = SPI_getbinval(tuple, tupdesc, 3, &isnull);
		config->model = isnull ? NULL : spi_pstrdup(TextDatumGetCString(val));

		val = SPI_getbinval(tuple, tupdesc, 4, &isnull);
		config->temperature = isnull ? jolix_predict_temperature : DatumGetFloat8(val);

		val = SPI_getbinval(tuple, tupdesc, 5, &isnull);
		config->max_tokens = isnull ? jolix_predict_max_tokens : DatumGetInt32(val);

		val = SPI_getbinval(tuple, tupdesc, 6, &isnull);
		config->system_prompt = isnull ? NULL : spi_pstrdup(TextDatumGetCString(val));

		val = SPI_getbinval(tuple, tupdesc, 7, &isnull);
		config->prompt_template = isnull ? NULL : spi_pstrdup(TextDatumGetCString(val));

		val = SPI_getbinval(tuple, tupdesc, 8, &isnull);
		config->history_count = isnull ? 0 : DatumGetInt32(val);

		val = SPI_getbinval(tuple, tupdesc, 9, &isnull);
		config->rag_table = isnull ? InvalidOid : DatumGetObjectId(val);

		val = SPI_getbinval(tuple, tupdesc, 10, &isnull);
		config->rag_similarity = isnull ? 0.5 : DatumGetFloat8(val);

		val = SPI_getbinval(tuple, tupdesc, 11, &isnull);
		config->rag_topn = isnull ? 5 : DatumGetInt32(val);
	}

	SPI_finish();

	fill_config_defaults(config);

	return found;
}

Datum
llm_infer(PG_FUNCTION_ARGS)
{
	text	   *system_prompt_text;
	text	   *user_input_text;
	int			history_count = 0;
	char	   *table_name = NULL;
	bool		table_name_allocated = false;
	char	   *system_prompt;
	char	   *user_input;
	char	   *json_body;
	char	   *response;
	char	   *content;
	LLMConfig	config;
	char	  **history_roles = NULL;
	char	  **history_contents = NULL;
	int			actual_history_count = 0;

	memset(&config, 0, sizeof(LLMConfig));

	if (PG_ARGISNULL(0))
		system_prompt = NULL;
	else
	{
		system_prompt_text = PG_GETARG_TEXT_PP(0);
		system_prompt = text_to_cstring(system_prompt_text);
	}

	if (PG_NARGS() == 2)
	{
		if (PG_ARGISNULL(1))
			user_input = NULL;
		else
		{
			user_input_text = PG_GETARG_TEXT_PP(1);
			user_input = text_to_cstring(user_input_text);
		}
	}
	else if (PG_NARGS() == 3)
	{
		if (PG_ARGISNULL(1))
			user_input = NULL;
		else
		{
			user_input_text = PG_GETARG_TEXT_PP(1);
			user_input = text_to_cstring(user_input_text);
		}
		history_count = PG_GETARG_INT32(2);
	}
	else if (PG_NARGS() == 4)
	{
		if (PG_ARGISNULL(1))
			user_input = NULL;
		else
		{
			user_input_text = PG_GETARG_TEXT_PP(1);
			user_input = text_to_cstring(user_input_text);
		}
		history_count = PG_GETARG_INT32(2);
		if (PG_ARGISNULL(3))
			table_name = NULL;
		else
		{
			table_name = text_to_cstring(PG_GETARG_TEXT_PP(3));
			table_name_allocated = true;
		}
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("llm_infer requires 2, 3, or 4 arguments")));
	}

	if (table_name == NULL || strlen(table_name) == 0)
	{
		if (jolix_predict_current_table != NULL && strlen(jolix_predict_current_table) > 0)
		{
			table_name = pstrdup(jolix_predict_current_table);
			table_name_allocated = true;
		}
		else if (jolix_predict_llm_history_table != NULL && strlen(jolix_predict_llm_history_table) > 0)
		{
			table_name = pstrdup(jolix_predict_llm_history_table);
			table_name_allocated = true;
		}
	}

	read_llm_config(InvalidOid, &config);

	if (system_prompt == NULL || strlen(system_prompt) == 0)
	{
		if (config.system_prompt != NULL && strlen(config.system_prompt) > 0)
		{
			if (system_prompt != NULL)
				pfree(system_prompt);
			system_prompt = pstrdup(config.system_prompt);
		}
	}

	if (user_input == NULL || strlen(user_input) == 0)
	{
		if (config.prompt_template != NULL && strlen(config.prompt_template) > 0)
		{
			if (user_input != NULL)
				pfree(user_input);
			user_input = pstrdup(config.prompt_template);
		}
	}

	if (system_prompt == NULL)
		system_prompt = pstrdup("");
	if (user_input == NULL)
		user_input = pstrdup("");

	if (strlen(config.api_url) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("jolix_predict.llm_api_url is not configured"),
				 errhint("Set jolix_predict.llm_api_url or use set_llm_config() to configure the API endpoint.")));

	if (history_count > 0 && table_name != NULL && strlen(table_name) > 0)
	{
		MemoryContext oldcontext;
		MemoryContext histcontext;

		histcontext = AllocSetContextCreate(CurrentMemoryContext,
											"llm_infer_history",
											ALLOCSET_DEFAULT_SIZES);
		oldcontext = MemoryContextSwitchTo(histcontext);

		actual_history_count = read_predict_history_from_table(
			table_name, history_count,
			&history_roles, &history_contents);

		MemoryContextSwitchTo(oldcontext);
	}

	PG_TRY();
	{
		json_body = build_chat_request(config.model, system_prompt,
									   actual_history_count, history_roles, history_contents,
									   user_input,
									   config.temperature, config.max_tokens);

		response = llm_http_post(config.api_url, config.api_key, json_body);

		content = parse_chat_response(response);

		if (content != NULL)
		{
			char	   *saved_content;

			saved_content = MemoryContextStrdup(CurTransactionContext, content);
			content = saved_content;
		}
	}
	PG_CATCH();
	{
		ErrorData  *errdata = CopyErrorData();

		ereport(WARNING,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("llm_infer: LLM inference failed: %s", errdata->message)));
		FlushErrorState();
		content = NULL;
	}
	PG_END_TRY();

	if (content != NULL)
	{
		MemoryContext oldcontext;
		MemoryContext reccontext;

		reccontext = AllocSetContextCreate(CurrentMemoryContext,
										   "llm_infer_record_history",
										   ALLOCSET_DEFAULT_SIZES);
		oldcontext = MemoryContextSwitchTo(reccontext);

		if (table_name != NULL && strlen(table_name) > 0)
		{
			record_predict_history_to_table(table_name, "user", user_input);
			record_predict_history_to_table(table_name, "assistant", content);
		}

		MemoryContextSwitchTo(oldcontext);
		MemoryContextDelete(reccontext);
	}

	if (table_name_allocated && table_name != NULL)
		pfree(table_name);

	if (content == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(content));
}

static int
read_predict_history_from_table(const char *table_name, int count,
								char ***out_roles, char ***out_contents)
{
	int			ret;
	int			history_count = 0;
	Oid			argtypes[2] = {TEXTOID, INT4OID};
	Datum		values[2];
	char		nulls[2] = {' ', ' '};

	*out_roles = NULL;
	*out_contents = NULL;

	if (count <= 0 || table_name == NULL || strlen(table_name) == 0)
		return 0;

	ret = SPI_connect();
	if (ret != SPI_OK_CONNECT)
		return 0;

	values[0] = CStringGetTextDatum(table_name);
	values[1] = Int32GetDatum(count * 2);

	ret = SPI_execute_with_args(
		"SELECT role, content FROM jolix_llm_history "
		"WHERE table_name = $1 ORDER BY created_at DESC LIMIT $2",
		2, argtypes, values, nulls, true, count * 2);

	if (ret == SPI_OK_SELECT && SPI_processed > 0)
	{
		int			nrows = Min(SPI_processed, (uint64) (count * 2));
		int			i;

		*out_roles = (char **) palloc(sizeof(char *) * nrows);
		*out_contents = (char **) palloc(sizeof(char *) * nrows);

		for (i = nrows - 1; i >= 0; i--)
		{
			HeapTuple	tuple = SPI_tuptable->vals[i];
			TupleDesc	tupdesc = SPI_tuptable->tupdesc;
			bool		isnull;
			char	   *role_str;
			char	   *content_str;

			role_str = SPI_getvalue(tuple, tupdesc, 1);
			if (role_str == NULL)
				continue;

			SPI_getbinval(tuple, tupdesc, 2, &isnull);
			if (isnull)
			{
				pfree(role_str);
				continue;
			}
			content_str = SPI_getvalue(tuple, tupdesc, 2);
			if (content_str == NULL)
			{
				pfree(role_str);
				continue;
			}

			(*out_roles)[history_count] = pstrdup(role_str);
			(*out_contents)[history_count] = pstrdup(content_str);
			history_count++;

			pfree(role_str);
			pfree(content_str);
		}
	}

	SPI_finish();
	return history_count;
}

static void
record_predict_history_to_table(const char *table_name, const char *role, const char *content)
{
	int			ret;
	Oid			argtypes[3] = {TEXTOID, TEXTOID, TEXTOID};
	Datum		values[3];
	char		nulls[3] = {' ', ' ', ' '};
	char	   *truncated_content;

	if (table_name == NULL || strlen(table_name) == 0)
		return;
	if (role == NULL || content == NULL)
		return;

	maybe_cleanup_expired_history();

	if (strlen(content) > MAX_HISTORY_CONTENT_LEN)
	{
		truncated_content = pnstrdup(content, MAX_HISTORY_CONTENT_LEN);
	}
	else
	{
		truncated_content = pstrdup(content);
	}

	ret = SPI_connect();
	if (ret != SPI_OK_CONNECT)
	{
		pfree(truncated_content);
		return;
	}

	values[0] = CStringGetTextDatum(table_name);
	values[1] = CStringGetTextDatum(role);
	values[2] = CStringGetTextDatum(truncated_content);

	pfree(truncated_content);

	SPI_execute_with_args(
		"INSERT INTO jolix_llm_history (table_name, role, content) VALUES ($1, $2, $3)",
		3, argtypes, values, nulls, false, 0);

	SPI_finish();
}

Datum
llm_infer_with_history(PG_FUNCTION_ARGS)
{
	text	   *system_prompt_text;
	text	   *user_input_text;
	int			history_count = PG_GETARG_INT32(2);
	text	   *table_name_text;
	char	   *system_prompt;
	char	   *user_input;
	char	   *table_name;
	char	   *json_body;
	char	   *response;
	char	   *content;
	LLMConfig	config;
	char	  **history_roles = NULL;
	char	  **history_contents = NULL;
	int			actual_history_count = 0;

	memset(&config, 0, sizeof(LLMConfig));

	if (PG_ARGISNULL(0))
		system_prompt = NULL;
	else
	{
		system_prompt_text = PG_GETARG_TEXT_PP(0);
		system_prompt = text_to_cstring(system_prompt_text);
	}

	if (PG_ARGISNULL(1))
		user_input = NULL;
	else
	{
		user_input_text = PG_GETARG_TEXT_PP(1);
		user_input = text_to_cstring(user_input_text);
	}

	if (PG_ARGISNULL(3))
		table_name = NULL;
	else
	{
		table_name_text = PG_GETARG_TEXT_PP(3);
		table_name = text_to_cstring(table_name_text);
	}

	read_llm_config(InvalidOid, &config);

	if (system_prompt == NULL || strlen(system_prompt) == 0)
	{
		if (config.system_prompt != NULL && strlen(config.system_prompt) > 0)
		{
			if (system_prompt != NULL)
				pfree(system_prompt);
			system_prompt = pstrdup(config.system_prompt);
		}
	}

	if (user_input == NULL || strlen(user_input) == 0)
	{
		if (config.prompt_template != NULL && strlen(config.prompt_template) > 0)
		{
			if (user_input != NULL)
				pfree(user_input);
			user_input = pstrdup(config.prompt_template);
		}
	}

	if (system_prompt == NULL)
		system_prompt = pstrdup("");
	if (user_input == NULL)
		user_input = pstrdup("");
	if (table_name == NULL)
		table_name = pstrdup("");

	if (strlen(config.api_url) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("jolix_predict.llm_api_url is not configured"),
				 errhint("Set jolix_predict.llm_api_url or use set_llm_config() to configure the API endpoint.")));

	if (history_count > 0)
	{
		MemoryContext oldcontext;
		MemoryContext histcontext;

		histcontext = AllocSetContextCreate(CurrentMemoryContext,
											"llm_infer_history",
											ALLOCSET_DEFAULT_SIZES);
		oldcontext = MemoryContextSwitchTo(histcontext);

		actual_history_count = read_predict_history_from_table(
			table_name, history_count,
			&history_roles, &history_contents);

		MemoryContextSwitchTo(oldcontext);
	}

	PG_TRY();
	{
		json_body = build_chat_request(config.model, system_prompt,
									   actual_history_count, history_roles, history_contents,
									   user_input,
									   config.temperature, config.max_tokens);

		response = llm_http_post(config.api_url, config.api_key, json_body);

		content = parse_chat_response(response);

		if (content != NULL)
		{
			char	   *saved_content;

			saved_content = MemoryContextStrdup(CurTransactionContext, content);
			content = saved_content;
		}
	}
	PG_CATCH();
	{
		ErrorData  *errdata = CopyErrorData();

		ereport(WARNING,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("llm_infer_with_history: LLM inference failed: %s", errdata->message)));
		FlushErrorState();
		content = NULL;
	}
	PG_END_TRY();

	if (content != NULL)
	{
		MemoryContext oldcontext;
		MemoryContext reccontext;

		reccontext = AllocSetContextCreate(CurrentMemoryContext,
										   "llm_infer_record_history",
										   ALLOCSET_DEFAULT_SIZES);
		oldcontext = MemoryContextSwitchTo(reccontext);

		record_predict_history_to_table(table_name, "user", user_input);
		record_predict_history_to_table(table_name, "assistant", content);

		MemoryContextSwitchTo(oldcontext);
		MemoryContextDelete(reccontext);
	}

	if (content == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(content));
}

Datum
record_predict_history(PG_FUNCTION_ARGS)
{
	text	   *p_table_name = PG_GETARG_TEXT_PP(0);
	text	   *p_role = PG_GETARG_TEXT_PP(1);
	text	   *p_content = PG_GETARG_TEXT_PP(2);
	char	   *role;
	int			ret;
	Oid			argtypes[3] = {TEXTOID, TEXTOID, TEXTOID};
	Datum		values[3];
	char		nulls[3] = {' ', ' ', ' '};

	role = text_to_cstring(p_role);

	if (strcmp(role, "user") != 0 && strcmp(role, "assistant") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("role must be 'user' or 'assistant', got '%s'", role)));

	ret = SPI_connect();
	if (ret != SPI_OK_CONNECT)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not connect to SPI")));

	values[0] = PointerGetDatum(p_table_name);
	values[1] = PointerGetDatum(p_role);
	values[2] = PointerGetDatum(p_content);

	SPI_execute_with_args(
		"INSERT INTO jolix_llm_history (table_name, role, content) VALUES ($1, $2, $3)",
		3, argtypes, values, nulls, false, 0);

	SPI_finish();

	PG_RETURN_VOID();
}

Datum
clear_predict_history(PG_FUNCTION_ARGS)
{
	text	   *p_table_name = PG_ARGISNULL(0) ? NULL : PG_GETARG_TEXT_PP(0);
	int			ret;

	ret = SPI_connect();
	if (ret != SPI_OK_CONNECT)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not connect to SPI")));

	if (p_table_name != NULL)
	{
		Oid			argtypes[1] = {TEXTOID};
		Datum		values[1];
		char		nulls[1] = {' '};

		values[0] = PointerGetDatum(p_table_name);

		SPI_execute_with_args(
			"DELETE FROM jolix_llm_history WHERE table_name = $1",
			1, argtypes, values, nulls, false, 0);
	}
	else
	{
		SPI_execute("DELETE FROM jolix_llm_history", false, 0);
	}

	{
		int			deleted = SPI_processed;

		SPI_finish();
		PG_RETURN_INT32(deleted);
	}
}

static int
do_cleanup_expired_history(void)
{
	int			ret;
	int			deleted = 0;
	char		query[256];

	if (jolix_llm_history_retention_days <= 0)
		return 0;

	snprintf(query, sizeof(query),
			 "DELETE FROM jolix_llm_history WHERE created_at < now() - interval '%d days'",
			 jolix_llm_history_retention_days);

	ret = SPI_connect();
	if (ret != SPI_OK_CONNECT)
		return 0;

	ret = SPI_execute(query, false, 0);
	if (ret == SPI_OK_DELETE)
		deleted = SPI_processed;

	SPI_finish();
	return deleted;
}

static void
maybe_cleanup_expired_history(void)
{
	static TimestampTz last_cleanup_time = 0;
	TimestampTz		now;
	int				deleted;

	if (jolix_llm_history_retention_days <= 0)
		return;

	now = GetCurrentTimestamp();

	if (last_cleanup_time != 0)
	{
		long	secs;
		int		microsecs;

		TimestampDifference(last_cleanup_time, now, &secs, &microsecs);
		if (secs < HISTORY_CLEANUP_INTERVAL)
			return;
	}

	last_cleanup_time = now;

	PG_TRY();
	{
		deleted = do_cleanup_expired_history();
		if (deleted > 0)
			elog(LOG, "jolix_predict: auto-cleaned %d expired history records (retention=%d days)",
				 deleted, jolix_llm_history_retention_days);
	}
	PG_CATCH();
	{
		FlushErrorState();
	}
	PG_END_TRY();
}

Datum
cleanup_predict_history(PG_FUNCTION_ARGS)
{
	int			deleted;

	deleted = do_cleanup_expired_history();

	PG_RETURN_INT32(deleted);
}

static char *
find_embedding_column_name(Oid relid)
{
	Relation	rel;
	TupleDesc	tupdesc;
	int			attnum;
	char	   *colname = NULL;

	rel = relation_open(relid, AccessShareLock);
	tupdesc = RelationGetDescr(rel);

	for (attnum = 1; attnum <= tupdesc->natts; attnum++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

		if (attr->attisdropped)
			continue;
		if (attr->attembedding)
		{
			colname = pstrdup(NameStr(attr->attname));
			break;
		}
	}

	relation_close(rel, AccessShareLock);
	return colname;
}

static Datum
compute_embedding_for_text(Oid rag_table, const char *embedding_colname,
						   const char *input_text)
{
	Oid			func_oid;
	FmgrInfo	flinfo;
	Datum		result;

	func_oid = get_embedding_function_oid(rag_table, embedding_colname);
	if (!OidIsValid(func_oid))
	{
		FuncCandidateList clist;
		List	   *namelist;
		int			fgc_flags;

		namelist = list_make1(makeString("simple_embedding"));
		clist = FuncnameGetCandidates(namelist, 1, NIL, false, false, false, true, &fgc_flags);
		list_free(namelist);

		if (clist != NULL)
		{
			func_oid = clist->oid;
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("no embedding function configured for column \"%s\" of table \"%s\"",
							embedding_colname, get_rel_name(rag_table)),
					 errhint("Use EMBEDDING AS (function_name(column)) STORED syntax or set the embedding_function table option.")));
		}
	}

	fmgr_info(func_oid, &flinfo);

	result = FunctionCall1(&flinfo, CStringGetTextDatum(input_text));

	return result;
}

static char *
vector_datum_to_string(Datum vector_datum)
{
	Oid			typoutput;
	bool		typIsVarlena;
	Oid			vector_oid;

	vector_oid = TypenameGetTypid("vector");
	if (!OidIsValid(vector_oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type \"vector\" is not installed"),
				 errhint("Install the pgvector extension first: CREATE EXTENSION vector;")));

	getTypeOutputInfo(vector_oid, &typoutput, &typIsVarlena);
	return OidOutputFunctionCall(typoutput, vector_datum);
}

static char *
do_rag_retrieval(Oid rag_table, const char *user_input,
				 double rag_similarity, int rag_topn)
{
	int			ret;
	char	   *rag_context = NULL;
	char	   *embedding_colname;
	char	   *embedding_hidden_colname;
	char	   *relname;
	StringInfo	query_buf;
	Datum		embedding_datum;
	char	   *embedding_str;

	embedding_colname = find_embedding_column_name(rag_table);
	if (embedding_colname == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("RAG table \"%s\" does not have an EMBEDDING column",
						get_rel_name(rag_table)),
				 errhint("Create an EMBEDDING column on the table first.")));

	embedding_hidden_colname = psprintf("%s_embedding", embedding_colname);

	embedding_datum = compute_embedding_for_text(rag_table, embedding_colname, user_input);
	embedding_str = vector_datum_to_string(embedding_datum);

	relname = get_rel_name(rag_table);

	ret = SPI_connect();
	if (ret != SPI_OK_CONNECT)
	{
		pfree(embedding_colname);
		pfree(embedding_hidden_colname);
		pfree(embedding_str);
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not connect to SPI for RAG retrieval")));
	}

	query_buf = makeStringInfo();
	appendStringInfo(query_buf,
					 "SELECT * FROM %s ORDER BY %s <=> $1::vector LIMIT %d",
					 quote_identifier(relname),
					 quote_identifier(embedding_hidden_colname),
					 rag_topn);

	{
		Oid			argtypes[1] = {TEXTOID};
		Datum		values[1];
		char		nulls[1] = {' '};

		values[0] = CStringGetTextDatum(embedding_str);

		ret = SPI_execute_with_args(query_buf->data,
									1, argtypes, values, nulls,
									true, rag_topn);
	}

	if (ret == SPI_OK_SELECT && SPI_processed > 0)
	{
		StringInfo	rag_buf = makeStringInfo();
		int			i;
		TupleDesc	spi_tupdesc = SPI_tuptable->tupdesc;

		appendStringInfoString(rag_buf, "Retrieved context:\n");

		for (i = 0; i < (int) SPI_processed; i++)
		{
			HeapTuple	tuple = SPI_tuptable->vals[i];
			int			j;
			bool		first = true;

			appendStringInfo(rag_buf, "\n--- Result %d ---\n", i + 1);

			for (j = 1; j <= spi_tupdesc->natts; j++)
			{
				Form_pg_attribute attr = TupleDescAttr(spi_tupdesc, j - 1);
				char	   *col_value;

				if (attr->attisdropped)
					continue;
				if (attr->attembedding)
					continue;
				if (namestrcmp(&attr->attname, "_embedding") == 0 ||
					namestrcmp(&attr->attname, "_predict") == 0 ||
					namestrcmp(&attr->attname, "_actual") == 0)
					continue;
				if (strstr(NameStr(attr->attname), "_embedding") != NULL)
					continue;

				col_value = SPI_getvalue(tuple, spi_tupdesc, j);
				if (col_value == NULL)
					continue;

				if (!first)
					appendStringInfoCharMacro(rag_buf, '\n');
				appendStringInfo(rag_buf, "%s: %s",
								 NameStr(attr->attname), col_value);
				first = false;
			}
		}

		rag_context = pstrdup(rag_buf->data);
		pfree(rag_buf->data);
	}
	else
	{
		rag_context = pstrdup("No relevant context found.");
	}

	SPI_finish();

	pfree(embedding_colname);
	pfree(embedding_hidden_colname);
	pfree(embedding_str);
	pfree(relname);
	pfree(query_buf->data);

	return rag_context;
}

Datum
llm_rag_infer(PG_FUNCTION_ARGS)
{
	text	   *system_prompt_text;
	text	   *user_input_text;
	int			history_count;
	char	   *system_prompt;
	char	   *user_input;
	char	   *table_name = NULL;
	bool		table_name_allocated = false;
	Oid			rag_table = InvalidOid;
	char	   *rag_context;
	char	   *combined_user_input;
	char	   *json_body;
	char	   *response;
	char	   *content;
	LLMConfig	config;
	double		rag_similarity;
	int			rag_topn;
	int			actual_history_count = 0;
	char	  **history_roles = NULL;
	char	  **history_contents = NULL;
	MemoryContext oldcontext;
	MemoryContext ragcontext;

	memset(&config, 0, sizeof(LLMConfig));

	if (PG_ARGISNULL(0))
		system_prompt = NULL;
	else
	{
		system_prompt_text = PG_GETARG_TEXT_PP(0);
		system_prompt = text_to_cstring(system_prompt_text);
	}

	if (PG_ARGISNULL(1))
		user_input = NULL;
	else
	{
		user_input_text = PG_GETARG_TEXT_PP(1);
		user_input = text_to_cstring(user_input_text);
	}

	if (PG_NARGS() >= 3 && !PG_ARGISNULL(2))
		history_count = PG_GETARG_INT32(2);
	else
		history_count = 0;

	if (jolix_predict_current_table != NULL && strlen(jolix_predict_current_table) > 0)
	{
		table_name = pstrdup(jolix_predict_current_table);
		table_name_allocated = true;
	}

	if (table_name != NULL)
	{
		Oid			relid;
		RangeVar   *rv;

		rv = makeRangeVarFromNameList(stringToQualifiedNameList(table_name, NULL));
		relid = RangeVarGetRelid(rv, NoLock, true);
		if (OidIsValid(relid))
			rag_table = relid;
	}

	if (!OidIsValid(rag_table))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("no RAG table available for llm_rag_infer"),
				 errhint("llm_rag_infer must be called from a PREDICT column on a table with an EMBEDDING column, or set jolix_predict.current_table.")));

	read_llm_config(InvalidOid, &config);

	if (system_prompt == NULL || strlen(system_prompt) == 0)
	{
		if (config.system_prompt != NULL && strlen(config.system_prompt) > 0)
		{
			if (system_prompt != NULL)
				pfree(system_prompt);
			system_prompt = pstrdup(config.system_prompt);
		}
	}

	if (user_input == NULL || strlen(user_input) == 0)
	{
		if (config.prompt_template != NULL && strlen(config.prompt_template) > 0)
		{
			if (user_input != NULL)
				pfree(user_input);
			user_input = pstrdup(config.prompt_template);
		}
	}

	if (system_prompt == NULL)
		system_prompt = pstrdup("");
	if (user_input == NULL)
		user_input = pstrdup("");

	if (strlen(config.api_url) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("jolix_predict.llm_api_url is not configured"),
				 errhint("Set jolix_predict.llm_api_url or use set_llm_config() to configure the API endpoint.")));

	rag_similarity = config.rag_similarity;
	rag_topn = config.rag_topn;

	if (rag_topn <= 0)
		rag_topn = 5;
	if (rag_similarity < 0.0)
		rag_similarity = 0.0;
	if (rag_similarity > 2.0)
		rag_similarity = 2.0;

	ragcontext = AllocSetContextCreate(CurrentMemoryContext,
									   "llm_rag_infer",
									   ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(ragcontext);

	rag_context = do_rag_retrieval(rag_table, user_input,
								   rag_similarity, rag_topn);

	MemoryContextSwitchTo(oldcontext);

	combined_user_input = psprintf("%s\n\n%s", rag_context, user_input);

	if (history_count > 0 && table_name != NULL && strlen(table_name) > 0)
	{
		MemoryContext histcontext;

		histcontext = AllocSetContextCreate(CurrentMemoryContext,
											"llm_rag_infer_history",
											ALLOCSET_DEFAULT_SIZES);
		oldcontext = MemoryContextSwitchTo(histcontext);

		actual_history_count = read_predict_history_from_table(
			table_name, history_count,
			&history_roles, &history_contents);

		MemoryContextSwitchTo(oldcontext);
	}

	PG_TRY();
	{
		json_body = build_chat_request(config.model, system_prompt,
									   actual_history_count, history_roles, history_contents,
									   combined_user_input,
									   config.temperature, config.max_tokens);

		response = llm_http_post(config.api_url, config.api_key, json_body);

		content = parse_chat_response(response);

		if (content != NULL)
		{
			char	   *saved_content;

			saved_content = MemoryContextStrdup(CurTransactionContext, content);
			content = saved_content;
		}
	}
	PG_CATCH();
	{
		ErrorData  *errdata;

		errdata = CopyErrorData();
		FlushErrorState();

		ereport(WARNING,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("llm_rag_infer: LLM inference failed: %s", errdata->message)));

		content = NULL;
	}
	PG_END_TRY();

	MemoryContextDelete(ragcontext);

	if (content != NULL)
	{
		MemoryContext reccontext;

		reccontext = AllocSetContextCreate(CurrentMemoryContext,
										   "llm_rag_infer_record_history",
										   ALLOCSET_DEFAULT_SIZES);
		oldcontext = MemoryContextSwitchTo(reccontext);

		if (table_name != NULL && strlen(table_name) > 0)
		{
			record_predict_history_to_table(table_name, "user", user_input);
			record_predict_history_to_table(table_name, "assistant", content);
		}

		MemoryContextSwitchTo(oldcontext);
		MemoryContextDelete(reccontext);
	}

	if (table_name_allocated && table_name != NULL)
		pfree(table_name);

	if (content == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(content));
}

/* ========================================================================
 * Ldm local inference functions (k-NN based, no external LLM required)
 * ======================================================================== */

/* Neighbor structure for k-NN results */
typedef struct LdmNeighbor
{
	char   *predict_value;		/* PREDICT column value */
	double	distance;			/* distance from search vector */
} LdmNeighbor;

/*
 * find_embeddings_column_name - Find the first EMBEDDING/EMBEDDINGS column name
 *
 * Checks both attembedding and attembeddings fields, since ldm_infer
 * works with either EMBEDDING AS (...) or EMBEDDINGS AS (...) syntax.
 *
 * For EMBEDDING AS (single-column): the visible text column has attembedding=true,
 * and the vector is stored in the hidden "{col}_embedding" column.
 *
 * For EMBEDDINGS AS (multi-column): the column itself is the hidden vector column
 * with attembeddings=true, so we return its name directly.
 */
static char *
find_embeddings_column_name(Oid relid)
{
	Relation	rel;
	TupleDesc	tupdesc;
	int			attnum;
	char	   *colname = NULL;

	rel = relation_open(relid, AccessShareLock);
	tupdesc = RelationGetDescr(rel);

	for (attnum = 1; attnum <= tupdesc->natts; attnum++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

		if (attr->attisdropped)
			continue;

		if (attr->attembedding)
		{
			/* EMBEDDING AS: visible text column, vector is in hidden {col}_embedding */
			char   *hidden_colname = psprintf("%s_embedding", NameStr(attr->attname));
			AttrNumber hidden_attnum = get_attnum(relid, hidden_colname);

			if (AttributeNumberIsValid(hidden_attnum))
				colname = hidden_colname;
			else
				pfree(hidden_colname);
			break;
		}
		else if (attr->attembeddings)
		{
			/* EMBEDDINGS AS: column itself is the hidden vector column */
			colname = pstrdup(NameStr(attr->attname));
			break;
		}
	}

	relation_close(rel, AccessShareLock);
	return colname;
}

/*
 * find_predict_column_name - Find the first PREDICT column name
 */
static char *
find_predict_column_name(Oid relid)
{
	Relation	rel;
	TupleDesc	tupdesc;
	int			attnum;
	char	   *colname = NULL;

	rel = relation_open(relid, AccessShareLock);
	tupdesc = RelationGetDescr(rel);

	for (attnum = 1; attnum <= tupdesc->natts; attnum++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

		if (attr->attisdropped)
			continue;
		if (attr->attpredict)
		{
			colname = pstrdup(NameStr(attr->attname));
			break;
		}
	}

	relation_close(rel, AccessShareLock);
	return colname;
}

/*
 * get_ldm_reloption_string - Read a string reloption from table's StdRdOptions
 *
 * String reloptions are stored as offsets in StdRdOptions. The actual string
 * data is appended after the struct. If offset is 0, returns default_val.
 */
static char *
get_ldm_reloption_string(Oid relid, int offset_field, const char *default_val)
{
	Relation	rel;
	StdRdOptions *relopts;
	char	   *result = NULL;
	int			offset;

	rel = relation_open(relid, AccessShareLock);

	if (rel->rd_options == NULL)
	{
		relation_close(rel, AccessShareLock);
		return pstrdup(default_val);
	}

	relopts = (StdRdOptions *) rel->rd_options;

	/* Determine which offset field to read */
	if (offset_field == offsetof(StdRdOptions, ldm_model_offset))
		offset = relopts->ldm_model_offset;
	else if (offset_field == offsetof(StdRdOptions, ldm_task_offset))
		offset = relopts->ldm_task_offset;
	else
	{
		relation_close(rel, AccessShareLock);
		return pstrdup(default_val);
	}

	if (offset > 0)
		result = pstrdup((char *) relopts + offset);

	relation_close(rel, AccessShareLock);

	if (result == NULL || strlen(result) == 0)
	{
		if (result != NULL)
			pfree(result);
		return pstrdup(default_val);
	}

	return result;
}

/*
 * get_ldm_topn - Read ldm_topn from table's StdRdOptions
 */
static int
get_ldm_topn(Oid relid)
{
	Relation	rel;
	StdRdOptions *relopts;
	int			topn = 5;		/* default */

	rel = relation_open(relid, AccessShareLock);

	if (rel->rd_options != NULL)
	{
		relopts = (StdRdOptions *) rel->rd_options;
		topn = relopts->ldm_topn;
	}

	relation_close(rel, AccessShareLock);

	if (topn <= 0)
		topn = 5;
	return topn;
}

/*
 * do_ldm_similarity_search - Execute vector similarity search via SPI
 *
 * Returns an array of LdmNeighbor structs with predict values and distances.
 */
static LdmNeighbor *
do_ldm_similarity_search(Oid table_oid, const char *embeddings_colname,
						   const char *predict_colname,
						   Datum search_vector_datum,
						   int topn, int *num_neighbors)
{
	int				ret;
	char		   *relname;
	char		   *vector_str;
	StringInfo		query_buf;
	LdmNeighbor  *neighbors = NULL;

	*num_neighbors = 0;

	vector_str = vector_datum_to_string(search_vector_datum);
	relname = get_rel_name(table_oid);

	ret = SPI_connect();
	if (ret != SPI_OK_CONNECT)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_EXCEPTION),
				 errmsg("ldm_infer: SPI_connect failed")));

	/* Build similarity search query with distance */
	query_buf = makeStringInfo();
	appendStringInfo(query_buf,
		"SELECT %s, %s <=> $1::vector AS distance "
		"FROM %s "
		"WHERE %s IS NOT NULL "
		"ORDER BY distance LIMIT %d",
		quote_identifier(predict_colname),
		quote_identifier(embeddings_colname),
		quote_identifier(relname),
		quote_identifier(predict_colname),
		topn);

	{
		Oid		argtypes[1] = {TEXTOID};
		Datum	values[1];
		char	nulls[1] = {' '};

		values[0] = CStringGetTextDatum(vector_str);

		ret = SPI_execute_with_args(query_buf->data,
									1, argtypes, values, nulls,
									true, topn);
	}

	if (ret == SPI_OK_SELECT && SPI_processed > 0)
	{
		int i;

		neighbors = (LdmNeighbor *) palloc(sizeof(LdmNeighbor) * SPI_processed);

		for (i = 0; i < (int) SPI_processed; i++)
		{
			HeapTuple	tuple = SPI_tuptable->vals[i];
			bool		isnull_dist;

			/* Get PREDICT column value */
			neighbors[i].predict_value = SPI_getvalue(tuple, SPI_tuptable->tupdesc, 1);

			/* Get distance value */
			{
				Datum	dist_datum;

				dist_datum = SPI_getbinval(tuple, SPI_tuptable->tupdesc, 2, &isnull_dist);
				if (isnull_dist)
					neighbors[i].distance = DBL_MAX;
				else
					neighbors[i].distance = DatumGetFloat8(dist_datum);
			}
		}

		*num_neighbors = (int) SPI_processed;
	}

	SPI_finish();
	return neighbors;
}

/*
 * ldm_classify - Weighted majority vote for classification
 *
 * Weight = 1 / (distance + epsilon). The class with the highest
 * total weight wins.
 */
static char *
ldm_classify(LdmNeighbor *neighbors, int num_neighbors)
{
	double	epsilon = 1e-6;
	char  **labels;
	double *weights;
	int		n_labels = 0;
	int		i, j;
	char   *best_label;
	double	best_weight;

	labels = (char **) palloc(sizeof(char *) * num_neighbors);
	weights = (double *) palloc0(sizeof(double) * num_neighbors);

	for (i = 0; i < num_neighbors; i++)
	{
		double	weight = 1.0 / (neighbors[i].distance + epsilon);
		bool	found = false;

		/* Check if label already exists */
		for (j = 0; j < n_labels; j++)
		{
			if (strcmp(labels[j], neighbors[i].predict_value) == 0)
			{
				weights[j] += weight;
				found = true;
				break;
			}
		}

		if (!found)
		{
			labels[n_labels] = neighbors[i].predict_value;
			weights[n_labels] = weight;
			n_labels++;
		}
	}

	/* Find the label with the highest weight */
	best_label = labels[0];
	best_weight = weights[0];

	for (i = 1; i < n_labels; i++)
	{
		if (weights[i] > best_weight)
		{
			best_weight = weights[i];
			best_label = labels[i];
		}
	}

	pfree(labels);
	pfree(weights);

	return pstrdup(best_label);
}

/*
 * ldm_regress - Weighted average for regression
 */
static char *
ldm_regress(LdmNeighbor *neighbors, int num_neighbors)
{
	double	epsilon = 1e-6;
	double	weighted_sum = 0.0;
	double	weight_sum = 0.0;
	double	value;
	char   *result;
	int		i;

	for (i = 0; i < num_neighbors; i++)
	{
		double	weight = 1.0 / (neighbors[i].distance + epsilon);

		value = atof(neighbors[i].predict_value);
		weighted_sum += weight * value;
		weight_sum += weight;
	}

	if (weight_sum == 0.0)
		return pstrdup("0");

	result = psprintf("%.6g", weighted_sum / weight_sum);
	return result;
}

/*
 * ldm_anomaly_detect - Distance threshold based anomaly detection
 *
 * Classify as anomaly if either:
 * 1. Average distance to neighbors exceeds absolute threshold (0.5),
 *    meaning the input is far from all training data.
 * 2. Average distance is more than 3x the nearest neighbor distance,
 *    meaning the input is close to one neighbor but far from others.
 */
static char *
ldm_anomaly_detect(LdmNeighbor *neighbors, int num_neighbors)
{
	double	avg_distance = 0.0;
	int		i;

	if (num_neighbors == 0)
		return pstrdup("anomaly");

	for (i = 0; i < num_neighbors; i++)
		avg_distance += neighbors[i].distance;

	avg_distance /= num_neighbors;

	/* Absolute threshold: input is far from all training data */
	if (avg_distance > 0.5)
		return pstrdup("anomaly");

	/* Relative threshold: input is close to one but far from others */
	if (avg_distance > neighbors[0].distance * 3.0)
		return pstrdup("anomaly");

	return pstrdup("normal");
}

/*
 * ldm_extract - Nearest neighbor copy for extraction
 */
static char *
ldm_extract(LdmNeighbor *neighbors, int num_neighbors)
{
	if (num_neighbors == 0)
		return NULL;

	/* Return the nearest neighbor's PREDICT value */
	return pstrdup(neighbors[0].predict_value);
}

/*
 * ldm_infer - Local k-NN inference function (zero parameters)
 *
 * Uses vector similarity search to find similar historical rows,
 * then applies k-NN algorithm based on ldm_task:
 *   classification -> weighted majority vote
 *   regression     -> weighted average
 *   anomaly        -> distance threshold
 *   extraction     -> nearest neighbor copy
 *
 * No external LLM API is required. All inference is done locally.
 */
PG_FUNCTION_INFO_V1(ldm_infer);

Datum
ldm_infer(PG_FUNCTION_ARGS)
{
	char		   *table_name;
	Oid				table_oid;
	char		   *embeddings_colname;
	char		   *predict_colname;
	char		   *task;
	int				topn;
	Datum			search_vector_datum;
	LdmNeighbor  *neighbors;
	int				num_neighbors;
	char		   *result;

	/* 1. Get current table name from GUC */
	if (jolix_predict_current_table == NULL ||
		strlen(jolix_predict_current_table) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("ldm_infer: cannot determine current table"),
				 errhint("ldm_infer must be used in a PREDICT column expression.")));

	table_name = pstrdup(jolix_predict_current_table);

	/* Handle schema-qualified names (e.g., "public.mytable") */
	{
		List	   *names = stringToQualifiedNameList(table_name, NULL);
		RangeVar   *rv = makeRangeVarFromNameList(names);

		table_oid = RangeVarGetRelid(rv, AccessShareLock, true);
	}

	if (!OidIsValid(table_oid))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("ldm_infer: table \"%s\" not found", table_name)));

	/* 2. Auto-detect EMBEDDINGS column name */
	embeddings_colname = find_embeddings_column_name(table_oid);
	if (embeddings_colname == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("table \"%s\" does not have an EMBEDDINGS column",
						table_name),
				 errhint("ldm_infer requires a table with an EMBEDDINGS column.")));

	/* 3. Auto-detect PREDICT column name */
	predict_colname = find_predict_column_name(table_oid);
	if (predict_colname == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("table \"%s\" does not have a PREDICT column",
						table_name)));

	/* 4. Read WITH parameters */
	task = get_ldm_reloption_string(table_oid,
									  offsetof(StdRdOptions, ldm_task_offset),
									  "classification");
	topn = get_ldm_topn(table_oid);

	/* 5. Get search vector from GUC (set by predict_trigger) */
	if (jolix_predict_ldm_current_vector != NULL &&
		strlen(jolix_predict_ldm_current_vector) > 0)
	{
		/* Parse vector string from GUC back to vector Datum */
		Oid			vector_oid;
		Oid			typinput;
		Oid			typioparam;

		vector_oid = TypenameGetTypid("vector");
		if (!OidIsValid(vector_oid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("type \"vector\" is not installed")));

		getTypeInputInfo(vector_oid, &typinput, &typioparam);
		search_vector_datum = OidInputFunctionCall(typinput,
												   jolix_predict_ldm_current_vector,
												   typioparam, -1);
	}
	else
	{
		/* Fallback: query the latest row's EMBEDDINGS value via SPI */
		int		ret;
		char   *relname;
		StringInfo	query_buf;

		relname = get_rel_name(table_oid);
		ret = SPI_connect();
		if (ret != SPI_OK_CONNECT)
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_EXCEPTION),
					 errmsg("ldm_infer: SPI_connect failed for vector fallback")));

		query_buf = makeStringInfo();
		appendStringInfo(query_buf,
			"SELECT %s FROM %s ORDER BY ctid DESC LIMIT 1",
			quote_identifier(embeddings_colname),
			quote_identifier(relname));

		ret = SPI_execute(query_buf->data, true, 1);

		if (ret == SPI_OK_SELECT && SPI_processed > 0)
		{
			bool	vec_isnull;

			search_vector_datum = SPI_getbinval(SPI_tuptable->vals[0],
												SPI_tuptable->tupdesc, 1,
												&vec_isnull);
			if (vec_isnull)
			{
				SPI_finish();
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("ldm_infer: EMBEDDINGS column \"%s\" is NULL",
								embeddings_colname)));
			}
		}
		else
		{
			SPI_finish();
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("ldm_infer: could not get search vector from table \"%s\"",
							table_name)));
		}

		SPI_finish();
	}

	/* 6. Vector similarity search */
	neighbors = do_ldm_similarity_search(
		table_oid, embeddings_colname, predict_colname,
		search_vector_datum, topn, &num_neighbors);

	if (num_neighbors == 0)
	{
		ereport(WARNING,
				(errmsg("ldm_infer: no historical data found in table \"%s\"",
						table_name)));
		PG_RETURN_NULL();
	}

	/* 7. Select inference algorithm based on task */
	if (strcmp(task, "classification") == 0)
		result = ldm_classify(neighbors, num_neighbors);
	else if (strcmp(task, "regression") == 0)
		result = ldm_regress(neighbors, num_neighbors);
	else if (strcmp(task, "anomaly") == 0)
		result = ldm_anomaly_detect(neighbors, num_neighbors);
	else if (strcmp(task, "extraction") == 0)
		result = ldm_extract(neighbors, num_neighbors);
	else
		result = ldm_classify(neighbors, num_neighbors);	/* default */

	/* 8. Return result */
	if (result == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(result));
}

