/*-------------------------------------------------------------------------
 *
 * pg_predict.c
 *    LLM inference functions for PostgreSQL
 *
 * This module provides:
 * - llm_infer(): Standalone LLM inference function
 * - llm_predict(): Default predict function for PREDICT columns
 * - llm_rag_infer(): RAG-enhanced LLM inference function
 * - llm_rag_predict(): RAG-enhanced predict function for PREDICT columns
 * - Configuration via pg_predict_config table and GUC parameters
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
 *    contrib/pg_predict/pg_predict.c
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
#include "utils/rel.h"
#include "utils/predict.h"

#include <curl/curl.h>
#include <string.h>
#include <stdlib.h>
#include <regex.h>

PG_MODULE_MAGIC;

static char *pg_predict_api_url = NULL;
static char *pg_predict_api_key = NULL;
static char *pg_predict_model = NULL;
static double pg_predict_temperature = 0.7;
static int pg_predict_max_tokens = 1024;
static int pg_predict_timeout = 60;

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

typedef struct ColValue
{
	char	   *name;
	char	   *value;
} ColValue;

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
static char *get_predict_column_name(Oid relid);
static char *spi_pstrdup(const char *str);
static char *format_row_as_text(HeapTupleData *tmptup, TupleDesc tupdesc,
								Oid relid, const char *predict_colname);
static char *apply_prompt_template(const char *template_str,
								   ColValue *col_values, int ncol_values);
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
PG_FUNCTION_INFO_V1(llm_predict_ext);
PG_FUNCTION_INFO_V1(llm_rag_infer);
PG_FUNCTION_INFO_V1(llm_rag_predict_ext);

void		_PG_init(void);

void
_PG_init(void)
{
	DefineCustomStringVariable("pg_predict.api_url",
							   "Default LLM API URL",
							   NULL,
							   &pg_predict_api_url,
							   "",
							   PGC_USERSET,
							   0,
							   NULL, NULL, NULL);

	DefineCustomStringVariable("pg_predict.api_key",
							   "Default LLM API key",
							   NULL,
							   &pg_predict_api_key,
							   "",
							   PGC_USERSET,
							   0,
							   NULL, NULL, NULL);

	DefineCustomStringVariable("pg_predict.model",
							   "Default LLM model name",
							   NULL,
							   &pg_predict_model,
							   "gpt-3.5-turbo",
							   PGC_USERSET,
							   0,
							   NULL, NULL, NULL);

	DefineCustomRealVariable("pg_predict.temperature",
							 "Default LLM temperature",
							 NULL,
							 &pg_predict_temperature,
							 0.7, 0.0, 2.0,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomIntVariable("pg_predict.max_tokens",
							"Default LLM max tokens",
							NULL,
							&pg_predict_max_tokens,
							1024, 1, 32768,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);

	DefineCustomIntVariable("pg_predict.timeout",
							"LLM API request timeout in seconds",
							NULL,
							&pg_predict_timeout,
							60, 1, 600,
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
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long) pg_predict_timeout);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

	res = curl_easy_perform(curl);

	if (res != CURLE_OK)
	{
		char	   *err_msg = psprintf("LLM API request failed: %s", curl_easy_strerror(res));

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
	if (config->api_url == NULL)
		config->api_url = pg_predict_api_url ? pstrdup(pg_predict_api_url) : pstrdup("");
	if (config->api_key == NULL)
		config->api_key = pg_predict_api_key ? pstrdup(pg_predict_api_key) : pstrdup("");
	if (config->model == NULL)
		config->model = pg_predict_model ? pstrdup(pg_predict_model) : pstrdup("gpt-3.5-turbo");
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

	snprintf(query, sizeof(query),
			 "SELECT api_url, api_key, model_name, temperature, max_tokens, "
			 "system_prompt, prompt_template, history_count, "
			 "rag_table, rag_similarity, rag_topn "
			 "FROM pg_predict_config WHERE scope = 'table' AND relid = %u",
			 relid);

	ret = SPI_execute(query, true, 1);
	if (ret == SPI_OK_SELECT && SPI_processed > 0)
	{
		found = true;
	}
	else
	{
		snprintf(query, sizeof(query),
				 "SELECT api_url, api_key, model_name, temperature, max_tokens, "
				 "system_prompt, prompt_template, history_count, "
				 "rag_table, rag_similarity, rag_topn "
				 "FROM pg_predict_config WHERE scope = 'system' LIMIT 1");

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
		config->temperature = isnull ? pg_predict_temperature : DatumGetFloat8(val);

		val = SPI_getbinval(tuple, tupdesc, 5, &isnull);
		config->max_tokens = isnull ? pg_predict_max_tokens : DatumGetInt32(val);

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

static char *
format_row_as_text(HeapTupleData *tmptup, TupleDesc tupdesc,
				   Oid relid, const char *predict_colname)
{
	StringInfo	buf = makeStringInfo();
	int			i;
	bool		first = true;

	appendStringInfoString(buf, "Input data:\n");

	for (i = 1; i <= tupdesc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i - 1);
		Datum		col_datum;
		bool		isnull;
		char	   *col_value;

		if (attr->attisdropped)
			continue;
		if (attr->attpredict)
			continue;
		if (predict_colname && namestrcmp(&attr->attname, predict_colname) == 0)
			continue;
		if (namestrcmp(&attr->attname, "_predict") == 0 ||
			namestrcmp(&attr->attname, "_actual") == 0 ||
			namestrcmp(&attr->attname, "_embedding") == 0)
			continue;

		col_datum = heap_getattr(tmptup, i, tupdesc, &isnull);
		if (isnull)
			continue;

		if (attr->atttypid == TEXTOID || attr->atttypid == VARCHAROID || attr->atttypid == BPCHAROID)
		{
			col_value = TextDatumGetCString(col_datum);
		}
		else
		{
			Oid			typoutput;
			bool		typIsVarlena;

			getTypeOutputInfo(attr->atttypid, &typoutput, &typIsVarlena);
			col_value = OidOutputFunctionCall(typoutput, col_datum);
		}

		if (!first)
			appendStringInfoCharMacro(buf, '\n');
		appendStringInfo(buf, "%s: %s", NameStr(attr->attname), col_value);
		first = false;

		pfree(col_value);
	}

	if (predict_colname)
		appendStringInfo(buf, "\n%s: ", predict_colname);

	return buf->data;
}

static char *
apply_prompt_template(const char *template_str,
					  ColValue *col_values, int ncol_values)
{
	StringInfo	result = makeStringInfo();
	const char *p;
	int			i;

	for (p = template_str; *p; p++)
	{
		if (*p == '{' && *(p + 1) == '{')
		{
			const char *start = p + 2;
			const char *end = strstr(start, "}}");

			if (end != NULL)
			{
				int			name_len = end - start;
				char	   *col_name = palloc(name_len + 1);

				memcpy(col_name, start, name_len);
				col_name[name_len] = '\0';

				for (i = 0; i < ncol_values; i++)
				{
					if (strcmp(col_values[i].name, col_name) == 0)
					{
						appendStringInfoString(result, col_values[i].value);
						break;
					}
				}

				pfree(col_name);
				p = end + 1;
				continue;
			}
		}

		appendStringInfoCharMacro(result, *p);
	}

	return result->data;
}

static char *
get_predict_column_name(Oid relid)
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

Datum
llm_infer(PG_FUNCTION_ARGS)
{
	text	   *system_prompt_text = PG_GETARG_TEXT_PP(0);
	int			history_count = 0;
	text	   *user_input_text;
	char	   *system_prompt;
	char	   *user_input;
	char	   *json_body;
	char	   *response;
	char	   *content;
	LLMConfig	config;
	char	  **history_roles = NULL;
	char	  **history_contents = NULL;

	memset(&config, 0, sizeof(LLMConfig));

	system_prompt = text_to_cstring(system_prompt_text);

	if (PG_NARGS() >= 3)
	{
		history_count = PG_GETARG_INT32(1);
		user_input_text = PG_GETARG_TEXT_PP(2);
	}
	else
	{
		user_input_text = PG_GETARG_TEXT_PP(1);
	}

	user_input = text_to_cstring(user_input_text);

	fill_config_defaults(&config);

	if (strlen(config.api_url) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_predict.api_url is not configured"),
				 errhint("Set pg_predict.api_url or use set_predict_config() to configure the API endpoint.")));

	json_body = build_chat_request(config.model, system_prompt,
								   history_count, history_roles, history_contents,
								   user_input,
								   config.temperature, config.max_tokens);

	response = llm_http_post(config.api_url, config.api_key, json_body);

	content = parse_chat_response(response);

	if (content == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("failed to parse LLM API response")));

	PG_RETURN_TEXT_P(cstring_to_text(content));
}

Datum
llm_predict_ext(PG_FUNCTION_ARGS)
{
	HeapTupleHeader rec_header = PG_GETARG_HEAPTUPLEHEADER(0);
	Oid			tup_type;
	int32		tup_typmod;
	TupleDesc	tupdesc;
	Oid			relid;
	LLMConfig	config;
	char	   *user_input = NULL;
	char	   *json_body;
	char	   *response;
	char	   *content = NULL;
	char	   *predict_colname = NULL;
	char	  **history_roles = NULL;
	char	  **history_contents = NULL;
	int			history_count = 0;
	int			i;
	MemoryContext oldcontext;
	MemoryContext querycontext;
	HeapTupleData tmptup;
	ColValue   *col_values = NULL;
	int			ncol_values = 0;

	tup_type = HeapTupleHeaderGetTypeId(rec_header);
	tup_typmod = HeapTupleHeaderGetTypMod(rec_header);

	relid = get_typ_typrelid(tup_type);
	if (!OidIsValid(relid))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("input record is not a composite type")));

	tupdesc = lookup_rowtype_tupdesc(tup_type, tup_typmod);

	tmptup.t_len = HeapTupleHeaderGetDatumLength(rec_header);
	ItemPointerSetInvalid(&(tmptup.t_self));
	tmptup.t_tableOid = InvalidOid;
	tmptup.t_data = rec_header;

	if (!read_llm_config(relid, &config))
	{
		ereport(DEBUG1,
				(errmsg("llm_predict: no config found for relid=%u, using defaults", relid)));
	}

	if (strlen(config.api_url) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("LLM API URL is not configured for this table"),
				 errhint("Use set_predict_config() to configure the API endpoint.")));

	predict_colname = get_predict_column_name(relid);

	col_values = (ColValue *) palloc(sizeof(ColValue) * tupdesc->natts);
	ncol_values = 0;

	for (i = 1; i <= tupdesc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i - 1);
		Datum		col_datum;
		bool		isnull;
		char	   *col_value;

		if (attr->attisdropped)
			continue;
		if (attr->attpredict)
			continue;
		if (namestrcmp(&attr->attname, "_predict") == 0 ||
			namestrcmp(&attr->attname, "_actual") == 0 ||
			namestrcmp(&attr->attname, "_embedding") == 0)
			continue;

		col_datum = heap_getattr(&tmptup, i, tupdesc, &isnull);
		if (isnull)
			continue;

		if (attr->atttypid == TEXTOID || attr->atttypid == VARCHAROID || attr->atttypid == BPCHAROID)
		{
			col_value = TextDatumGetCString(col_datum);
		}
		else
		{
			Oid			typoutput;
			bool		typIsVarlena;

			getTypeOutputInfo(attr->atttypid, &typoutput, &typIsVarlena);
			col_value = OidOutputFunctionCall(typoutput, col_datum);
		}

		col_values[ncol_values].name = pstrdup(NameStr(attr->attname));
		col_values[ncol_values].value = col_value;
		ncol_values++;
	}

	if (config.prompt_template && strlen(config.prompt_template) > 0)
	{
		user_input = apply_prompt_template(config.prompt_template, col_values, ncol_values);
	}
	else
	{
		user_input = format_row_as_text(&tmptup, tupdesc, relid, predict_colname);
	}

	ReleaseTupleDesc(tupdesc);

	history_count = config.history_count;

	if (history_count > 0)
	{
		predict_colname = get_predict_column_name(relid);

		querycontext = AllocSetContextCreate(CurrentMemoryContext,
											 "llm_predict_history",
											 ALLOCSET_DEFAULT_SIZES);
		oldcontext = MemoryContextSwitchTo(querycontext);

		if (SPI_connect() == SPI_OK_CONNECT)
		{
			char		query[2048];
			int			ret;
			char	   *relname = get_rel_name(relid);

			if (predict_colname != NULL)
			{
				snprintf(query, sizeof(query),
						 "SELECT * FROM %s ORDER BY ctid DESC LIMIT %d",
						 relname, history_count);
			}
			else
			{
				snprintf(query, sizeof(query),
						 "SELECT * FROM %s ORDER BY ctid DESC LIMIT %d",
						 relname, history_count);
			}

			ret = SPI_execute(query, true, history_count);
			history_count = 0;
			if (ret == SPI_OK_SELECT && SPI_processed > 0)
			{
				int			nrows = Min(SPI_processed, (uint64) config.history_count);

				history_roles = (char **) palloc(sizeof(char *) * nrows * 2);
				history_contents = (char **) palloc(sizeof(char *) * nrows * 2);

				for (i = nrows - 1; i >= 0; i--)
				{
					HeapTuple	hist_tuple = SPI_tuptable->vals[i];
					TupleDesc	hist_tupdesc = SPI_tuptable->tupdesc;
					StringInfo	hist_user_buf;
				bool		has_predict = false;
				int			j;
				bool		first;

					hist_user_buf = makeStringInfo();
					appendStringInfoString(hist_user_buf, "Input data:\n");

					first = true;
					for (j = 1; j <= hist_tupdesc->natts; j++)
					{
						Form_pg_attribute hattr = TupleDescAttr(hist_tupdesc, j - 1);
						char	   *hcol_value;

						if (hattr->attisdropped)
							continue;
						if (hattr->attpredict)
						{
							has_predict = true;
							continue;
						}
						if (namestrcmp(&hattr->attname, "_predict") == 0 ||
							namestrcmp(&hattr->attname, "_actual") == 0 ||
							namestrcmp(&hattr->attname, "_embedding") == 0)
							continue;

						hcol_value = SPI_getvalue(hist_tuple, hist_tupdesc, j);
						if (hcol_value == NULL)
							continue;

						if (!first)
							appendStringInfoCharMacro(hist_user_buf, '\n');
						appendStringInfo(hist_user_buf, "%s: %s",
										 NameStr(hattr->attname), hcol_value);
						first = false;
					}

					if (predict_colname)
						appendStringInfo(hist_user_buf, "\n%s: ", predict_colname);

					history_roles[history_count] = pstrdup("user");
					history_contents[history_count] = hist_user_buf->data;
					history_count++;

					if (has_predict && predict_colname)
					{
						bool		pred_isnull;
						int			pred_attnum = InvalidAttrNumber;

						for (j = 1; j <= hist_tupdesc->natts; j++)
						{
							Form_pg_attribute hattr = TupleDescAttr(hist_tupdesc, j - 1);

							if (!hattr->attisdropped && hattr->attpredict)
							{
								pred_attnum = j;
								break;
							}
						}

						if (pred_attnum != InvalidAttrNumber)
						{
							SPI_getbinval(hist_tuple, hist_tupdesc, pred_attnum, &pred_isnull);
							if (!pred_isnull)
							{
								char	   *pred_value = SPI_getvalue(hist_tuple, hist_tupdesc, pred_attnum);

								if (pred_value)
								{
									history_roles[history_count] = pstrdup("assistant");
									history_contents[history_count] = pstrdup(pred_value);
									history_count++;
								}
							}
						}
					}
				}
			}

			SPI_finish();
		}

		MemoryContextSwitchTo(oldcontext);
		MemoryContextDelete(querycontext);
	}

	PG_TRY();
	{
		json_body = build_chat_request(config.model, config.system_prompt,
									   history_count, history_roles, history_contents,
									   user_input,
									   config.temperature, config.max_tokens);

		response = llm_http_post(config.api_url, config.api_key, json_body);

		content = parse_chat_response(response);
	}
	PG_CATCH();
	{
		ereport(WARNING,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("llm_predict: LLM inference failed, returning NULL")));
		content = NULL;
	}
	PG_END_TRY();

	if (content == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(content));
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
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("no embedding function configured for column \"%s\" of table \"%s\"",
						embedding_colname, get_rel_name(rag_table)),
				 errhint("Set the embedding_function table option.")));

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
	text	   *system_prompt_text = PG_GETARG_TEXT_PP(0);
	int			history_count = PG_GETARG_INT32(1);
	text	   *user_input_text = PG_GETARG_TEXT_PP(2);
	Oid			rag_table = PG_GETARG_OID(3);
	float8		rag_similarity = PG_GETARG_FLOAT8(4);
	int			rag_topn = PG_GETARG_INT32(5);
	char	   *system_prompt;
	char	   *user_input;
	char	   *rag_context;
	char	   *combined_user_input;
	char	   *json_body;
	char	   *response;
	char	   *content;
	LLMConfig	config;
	char	  **history_roles = NULL;
	char	  **history_contents = NULL;
	MemoryContext oldcontext;
	MemoryContext ragcontext;

	memset(&config, 0, sizeof(LLMConfig));

	system_prompt = text_to_cstring(system_prompt_text);
	user_input = text_to_cstring(user_input_text);

	fill_config_defaults(&config);

	if (strlen(config.api_url) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_predict.api_url is not configured"),
				 errhint("Set pg_predict.api_url or use set_predict_config() to configure the API endpoint.")));

	if (!OidIsValid(rag_table))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid RAG table OID")));

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

	if (history_count > 0)
	{
		int			ret;
		char	   *relname;
		char		query[2048];

		ret = SPI_connect();
		if (ret == SPI_OK_CONNECT)
		{
			relname = get_rel_name(rag_table);
			snprintf(query, sizeof(query),
					 "SELECT * FROM %s ORDER BY ctid DESC LIMIT %d",
					 quote_identifier(relname),
					 history_count);

			ret = SPI_execute(query, true, history_count);
			history_count = 0;

			if (ret == SPI_OK_SELECT && SPI_processed > 0)
			{
				int			nrows = Min(SPI_processed, (uint64) PG_GETARG_INT32(1));
				int			i;

				history_roles = (char **) palloc(sizeof(char *) * nrows * 2);
				history_contents = (char **) palloc(sizeof(char *) * nrows * 2);

				for (i = nrows - 1; i >= 0; i--)
				{
					HeapTuple	hist_tuple = SPI_tuptable->vals[i];
					TupleDesc	hist_tupdesc = SPI_tuptable->tupdesc;
					StringInfo	hist_user_buf;
					int			j;
					bool		first = true;

					hist_user_buf = makeStringInfo();
					appendStringInfoString(hist_user_buf, "Input data:\n");

					for (j = 1; j <= hist_tupdesc->natts; j++)
					{
						Form_pg_attribute hattr = TupleDescAttr(hist_tupdesc, j - 1);
						char	   *hcol_value;

						if (hattr->attisdropped)
							continue;
						if (hattr->attembedding)
							continue;
						if (namestrcmp(&hattr->attname, "_embedding") == 0 ||
							namestrcmp(&hattr->attname, "_predict") == 0 ||
							namestrcmp(&hattr->attname, "_actual") == 0)
							continue;

						hcol_value = SPI_getvalue(hist_tuple, hist_tupdesc, j);
						if (hcol_value == NULL)
							continue;

						if (!first)
							appendStringInfoCharMacro(hist_user_buf, '\n');
						appendStringInfo(hist_user_buf, "%s: %s",
										 NameStr(hattr->attname), hcol_value);
						first = false;
					}

					history_roles[history_count] = pstrdup("user");
					history_contents[history_count] = hist_user_buf->data;
					history_count++;
				}
			}

			SPI_finish();
			pfree(relname);
		}
	}

	PG_TRY();
	{
		json_body = build_chat_request(config.model, system_prompt,
									   history_count, history_roles, history_contents,
									   combined_user_input,
									   config.temperature, config.max_tokens);

		response = llm_http_post(config.api_url, config.api_key, json_body);

		content = parse_chat_response(response);
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

	if (content == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(content));
}

Datum
llm_rag_predict_ext(PG_FUNCTION_ARGS)
{
	HeapTupleHeader rec_header = PG_GETARG_HEAPTUPLEHEADER(0);
	Oid			tup_type;
	int32		tup_typmod;
	TupleDesc	tupdesc;
	Oid			relid;
	LLMConfig	config;
	char	   *user_input = NULL;
	char	   *rag_context = NULL;
	char	   *combined_user_input = NULL;
	char	   *json_body;
	char	   *response;
	char	   *content = NULL;
	char	   *predict_colname = NULL;
	char	  **history_roles = NULL;
	char	  **history_contents = NULL;
	int			history_count = 0;
	int			i;
	MemoryContext oldcontext;
	MemoryContext querycontext;
	HeapTupleData tmptup;
	ColValue   *col_values = NULL;
	int			ncol_values = 0;

	tup_type = HeapTupleHeaderGetTypeId(rec_header);
	tup_typmod = HeapTupleHeaderGetTypMod(rec_header);

	relid = get_typ_typrelid(tup_type);
	if (!OidIsValid(relid))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("input record is not a composite type")));

	tupdesc = lookup_rowtype_tupdesc(tup_type, tup_typmod);

	tmptup.t_len = HeapTupleHeaderGetDatumLength(rec_header);
	ItemPointerSetInvalid(&(tmptup.t_self));
	tmptup.t_tableOid = InvalidOid;
	tmptup.t_data = rec_header;

	if (!read_llm_config(relid, &config))
	{
		ereport(DEBUG1,
				(errmsg("llm_rag_predict: no config found for relid=%u, using defaults", relid)));
	}

	if (strlen(config.api_url) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("LLM API URL is not configured for this table"),
				 errhint("Use set_predict_config() to configure the API endpoint.")));

	predict_colname = get_predict_column_name(relid);

	col_values = (ColValue *) palloc(sizeof(ColValue) * tupdesc->natts);
	ncol_values = 0;

	for (i = 1; i <= tupdesc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i - 1);
		Datum		col_datum;
		bool		isnull;
		char	   *col_value;

		if (attr->attisdropped)
			continue;
		if (attr->attpredict)
			continue;
		if (namestrcmp(&attr->attname, "_predict") == 0 ||
			namestrcmp(&attr->attname, "_actual") == 0 ||
			namestrcmp(&attr->attname, "_embedding") == 0)
			continue;

		col_datum = heap_getattr(&tmptup, i, tupdesc, &isnull);
		if (isnull)
			continue;

		if (attr->atttypid == TEXTOID || attr->atttypid == VARCHAROID || attr->atttypid == BPCHAROID)
		{
			col_value = TextDatumGetCString(col_datum);
		}
		else
		{
			Oid			typoutput;
			bool		typIsVarlena;

			getTypeOutputInfo(attr->atttypid, &typoutput, &typIsVarlena);
			col_value = OidOutputFunctionCall(typoutput, col_datum);
		}

		col_values[ncol_values].name = pstrdup(NameStr(attr->attname));
		col_values[ncol_values].value = col_value;
		ncol_values++;
	}

	if (config.prompt_template && strlen(config.prompt_template) > 0)
	{
		user_input = apply_prompt_template(config.prompt_template, col_values, ncol_values);
	}
	else
	{
		user_input = format_row_as_text(&tmptup, tupdesc, relid, predict_colname);
	}

	ReleaseTupleDesc(tupdesc);

	if (OidIsValid(config.rag_table))
	{
		MemoryContext ragcontext;

		ragcontext = AllocSetContextCreate(CurrentMemoryContext,
										   "llm_rag_predict_rag",
										   ALLOCSET_DEFAULT_SIZES);
		oldcontext = MemoryContextSwitchTo(ragcontext);

		rag_context = do_rag_retrieval(config.rag_table, user_input,
									   config.rag_similarity, config.rag_topn);

		MemoryContextSwitchTo(oldcontext);

		combined_user_input = psprintf("%s\n\n%s", rag_context, user_input);
	}
	else
	{
		combined_user_input = user_input;
	}

	history_count = config.history_count;

	if (history_count > 0)
	{
		predict_colname = get_predict_column_name(relid);

		querycontext = AllocSetContextCreate(CurrentMemoryContext,
											 "llm_rag_predict_history",
											 ALLOCSET_DEFAULT_SIZES);
		oldcontext = MemoryContextSwitchTo(querycontext);

		if (SPI_connect() == SPI_OK_CONNECT)
		{
			char		query[2048];
			int			ret;
			char	   *relname = get_rel_name(relid);

			snprintf(query, sizeof(query),
					 "SELECT * FROM %s ORDER BY ctid DESC LIMIT %d",
					 relname, history_count);

			ret = SPI_execute(query, true, history_count);
			history_count = 0;
			if (ret == SPI_OK_SELECT && SPI_processed > 0)
			{
				int			nrows = Min(SPI_processed, (uint64) config.history_count);

				history_roles = (char **) palloc(sizeof(char *) * nrows * 2);
				history_contents = (char **) palloc(sizeof(char *) * nrows * 2);

				for (i = nrows - 1; i >= 0; i--)
				{
					HeapTuple	hist_tuple = SPI_tuptable->vals[i];
					TupleDesc	hist_tupdesc = SPI_tuptable->tupdesc;
					StringInfo	hist_user_buf;
					int			j;
					bool		first;
					bool		has_predict = false;

					hist_user_buf = makeStringInfo();
					appendStringInfoString(hist_user_buf, "Input data:\n");

					first = true;
					for (j = 1; j <= hist_tupdesc->natts; j++)
					{
						Form_pg_attribute hattr = TupleDescAttr(hist_tupdesc, j - 1);
						char	   *hcol_value;

						if (hattr->attisdropped)
							continue;
						if (hattr->attpredict)
						{
							has_predict = true;
							continue;
						}
						if (namestrcmp(&hattr->attname, "_predict") == 0 ||
							namestrcmp(&hattr->attname, "_actual") == 0 ||
							namestrcmp(&hattr->attname, "_embedding") == 0)
							continue;

						hcol_value = SPI_getvalue(hist_tuple, hist_tupdesc, j);
						if (hcol_value == NULL)
							continue;

						if (!first)
							appendStringInfoCharMacro(hist_user_buf, '\n');
						appendStringInfo(hist_user_buf, "%s: %s",
										 NameStr(hattr->attname), hcol_value);
						first = false;
					}

					if (predict_colname)
						appendStringInfo(hist_user_buf, "\n%s: ", predict_colname);

					history_roles[history_count] = pstrdup("user");
					history_contents[history_count] = hist_user_buf->data;
					history_count++;

					if (has_predict && predict_colname)
					{
						bool		pred_isnull;
						int			pred_attnum = InvalidAttrNumber;

						for (j = 1; j <= hist_tupdesc->natts; j++)
						{
							Form_pg_attribute hattr = TupleDescAttr(hist_tupdesc, j - 1);

							if (!hattr->attisdropped && hattr->attpredict)
							{
								pred_attnum = j;
								break;
							}
						}

						if (pred_attnum != InvalidAttrNumber)
						{
							SPI_getbinval(hist_tuple, hist_tupdesc, pred_attnum, &pred_isnull);
							if (!pred_isnull)
							{
								char	   *pred_value = SPI_getvalue(hist_tuple, hist_tupdesc, pred_attnum);

								if (pred_value)
								{
									history_roles[history_count] = pstrdup("assistant");
									history_contents[history_count] = pstrdup(pred_value);
									history_count++;
								}
							}
						}
					}
				}
			}

			SPI_finish();
		}

		MemoryContextSwitchTo(oldcontext);
		MemoryContextDelete(querycontext);
	}

	PG_TRY();
	{
		json_body = build_chat_request(config.model, config.system_prompt,
									   history_count, history_roles, history_contents,
									   combined_user_input,
									   config.temperature, config.max_tokens);

		response = llm_http_post(config.api_url, config.api_key, json_body);

		content = parse_chat_response(response);
	}
	PG_CATCH();
	{
		ErrorData  *errdata;

		errdata = CopyErrorData();
		FlushErrorState();

		ereport(WARNING,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("llm_rag_predict: LLM inference failed: %s", errdata->message)));
		content = NULL;
	}
	PG_END_TRY();

	if (content == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(content));
}
