/*-------------------------------------------------------------------------
 *
 * jolix_embedding.c
 *    Sentence Transformers embedding functions for PostgreSQL
 *
 * This module provides built-in embedding functions using sentence-transformers
 * library. It supports automatic model download and session-level caching.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    contrib/jolix_embedding/jolix_embedding.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "miscadmin.h"
#include "catalog/namespace.h"

#include <sys/stat.h>
#include <Python.h>

#include "vector.h"

PG_MODULE_MAGIC;

#define DEFAULT_MODEL_NAME "sentence-transformers/all-MiniLM-L6-v2"
#define DEFAULT_MODEL_PATH "/usr/local/pgsql/models"
#define HF_MIRROR_URL "https://hf-mirror.com"

static bool python_initialized = false;
static PyObject *model_cache_dict = NULL;

static char *jolix_embedding_model_name = NULL;
static char *jolix_embedding_model_path = NULL;

static Oid vector_type_oid = InvalidOid;

PG_FUNCTION_INFO_V1(sentence_transformers_embedding);
PG_FUNCTION_INFO_V1(sentence_transformers_embedding_with_model);
PG_FUNCTION_INFO_V1(st_text_placeholder_distance);

static void init_python(void);
static void ensure_model_dir(const char *model_path);
static PyObject *load_model(const char *model_name);
static Vector *embedding_to_vector(PyObject *embedding_list);
static void ensure_vector_type(void);

Datum
st_text_placeholder_distance(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("text distance operator cannot be used directly"),
			 errhint("This operator is reserved for EMBEDDING columns. Use: ORDER BY embedding_column <=> 'search text'. The query rewriter will automatically convert it to a vector distance query.")));
	PG_RETURN_NULL();
}

static void
ensure_vector_type(void)
{
	if (!OidIsValid(vector_type_oid))
	{
		vector_type_oid = TypenameGetTypid("vector");
		if (!OidIsValid(vector_type_oid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("type \"vector\" does not exist"),
					 errhint("Create the pgvector extension first: CREATE EXTENSION vector;")));
	}
}

static void
init_python(void)
{
	PyObject *main_module;

	if (python_initialized)
		return;

	Py_Initialize();
	if (!Py_IsInitialized())
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not initialize Python interpreter")));

	main_module = PyImport_AddModule("__main__");
	if (!main_module)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not get Python __main__ module")));

	model_cache_dict = PyDict_New();
	if (!model_cache_dict)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not create model cache dictionary")));

	python_initialized = true;
}

static void
ensure_model_dir(const char *model_path)
{
	struct stat st;

	if (stat(model_path, &st) != 0)
	{
		if (mkdir(model_path, 0755) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_IO_ERROR),
					 errmsg("could not create model directory: %s", model_path)));
	}
}

static PyObject *
load_model(const char *model_name)
{
	PyObject *transformers_module = NULL;
	PyObject *sentence_transformers_class = NULL;
	PyObject *model_instance = NULL;
	PyObject *args = NULL;
	PyObject *kwargs = NULL;
	PyObject *cache_key = NULL;
	PyObject *cached_model = NULL;
	const char *model_path;

	model_path = jolix_embedding_model_path ? jolix_embedding_model_path : DEFAULT_MODEL_PATH;

	cache_key = PyUnicode_FromString(model_name);
	if (!cache_key)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not create cache key")));

	cached_model = PyDict_GetItem(model_cache_dict, cache_key);
	if (cached_model)
	{
		Py_DECREF(cache_key);
		Py_INCREF(cached_model);
		return cached_model;
	}

	PyRun_SimpleString("import os");
	PyRun_SimpleString("os.environ['HF_ENDPOINT'] = '" HF_MIRROR_URL "'");

	transformers_module = PyImport_ImportModule("sentence_transformers");
	if (!transformers_module)
	{
		PyErr_Print();
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not import sentence_transformers module"),
				 errhint("Install sentence-transformers: pip install sentence-transformers")));
	}

	sentence_transformers_class = PyObject_GetAttrString(transformers_module, "SentenceTransformer");
	if (!sentence_transformers_class)
	{
		Py_DECREF(transformers_module);
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not get SentenceTransformer class")));
	}

	ensure_model_dir(model_path);

	elog(LOG, "Loading sentence-transformers model: %s (cache: %s)", model_name, model_path);

	args = Py_BuildValue("(s)", model_name);
	kwargs = Py_BuildValue("{s:s}", "cache_folder", model_path);

	model_instance = PyObject_Call(sentence_transformers_class, args, kwargs);

	Py_DECREF(transformers_module);
	Py_DECREF(sentence_transformers_class);
	Py_DECREF(args);
	if (kwargs)
		Py_DECREF(kwargs);

	if (!model_instance)
	{
		PyErr_Print();
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not load sentence-transformers model: %s", model_name)));
	}

	PyDict_SetItem(model_cache_dict, cache_key, model_instance);
	Py_DECREF(cache_key);

	return model_instance;
}

static Vector *
embedding_to_vector(PyObject *embedding_list)
{
	PyObject *item = NULL;
	Vector *result;
	int dim = 0;
	int i = 0;
	PyObject *list_obj;

	list_obj = PySequence_List(embedding_list);
	if (!list_obj)
	{
		PyErr_Print();
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not convert embedding to list")));
	}

	dim = PyList_Size(list_obj);
	if (dim <= 0 || dim > VECTOR_MAX_DIM)
	{
		Py_DECREF(list_obj);
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("invalid embedding dimension: %d", dim)));
	}

	result = (Vector *) palloc0(VECTOR_SIZE(dim));
	SET_VARSIZE(result, VECTOR_SIZE(dim));
	result->dim = dim;
	result->unused = 0;

	for (i = 0; i < dim; i++)
	{
		item = PyList_GetItem(list_obj, i);
		if (!item)
		{
			Py_DECREF(list_obj);
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("could not get embedding value at index %d", i)));
		}

		result->x[i] = (float) PyFloat_AsDouble(item);
		if (PyErr_Occurred())
		{
			Py_DECREF(list_obj);
			PyErr_Print();
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("could not convert embedding value to float at index %d", i)));
		}
	}

	Py_DECREF(list_obj);

	return result;
}

Datum
sentence_transformers_embedding(PG_FUNCTION_ARGS)
{
	text *input_text;
	char *input_str;
	const char *model_name;
	PyObject *model = NULL;
	PyObject *result = NULL;
	Vector *vector_result;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	ensure_vector_type();

	input_text = PG_GETARG_TEXT_P(0);
	input_str = text_to_cstring(input_text);

	init_python();

	model_name = jolix_embedding_model_name ? jolix_embedding_model_name : DEFAULT_MODEL_NAME;
	model = load_model(model_name);

	result = PyObject_CallMethod(model, "encode", "(s)", input_str);

	pfree(input_str);

	if (!result)
	{
		PyErr_Print();
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not generate embedding")));
	}

	vector_result = embedding_to_vector(result);
	Py_DECREF(result);

	PG_RETURN_POINTER(vector_result);
}

Datum
sentence_transformers_embedding_with_model(PG_FUNCTION_ARGS)
{
	text *input_text;
	text *model_name_text;
	char *input_str;
	char *model_name;
	PyObject *model = NULL;
	PyObject *result = NULL;
	Vector *vector_result;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	ensure_vector_type();

	input_text = PG_GETARG_TEXT_P(0);
	input_str = text_to_cstring(input_text);

	if (PG_ARGISNULL(1))
		model_name = pstrdup(jolix_embedding_model_name ? jolix_embedding_model_name : DEFAULT_MODEL_NAME);
	else
	{
		model_name_text = PG_GETARG_TEXT_P(1);
		model_name = text_to_cstring(model_name_text);
	}

	init_python();

	model = load_model(model_name);

	result = PyObject_CallMethod(model, "encode", "(s)", input_str);

	pfree(input_str);

	if (!result)
	{
		PyErr_Print();
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not generate embedding")));
	}

	vector_result = embedding_to_vector(result);
	Py_DECREF(result);

	PG_RETURN_POINTER(vector_result);
}

void
_PG_init(void)
{
	DefineCustomStringVariable("jolix_embedding.model_name",
							   "Default model name for sentence-transformers",
							   NULL,
							   &jolix_embedding_model_name,
							   DEFAULT_MODEL_NAME,
							   PGC_USERSET,
							   0,
							   NULL,
							   NULL,
							   NULL);

	DefineCustomStringVariable("jolix_embedding.model_path",
							   "Path to store downloaded models",
							   NULL,
							   &jolix_embedding_model_path,
							   DEFAULT_MODEL_PATH,
							   PGC_SIGHUP,
							   0,
							   NULL,
							   NULL,
							   NULL);
}
