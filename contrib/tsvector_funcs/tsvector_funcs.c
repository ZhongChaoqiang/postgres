/*-------------------------------------------------------------------------
 *
 * tsvector_funcs.c
 *    Timeseries vector helper functions: ts2v and timeseries_vector_run.
 *
 * These functions are provided as a loadable shared library because the
 * return type `vector` (from pgvector) is not available at bootstrap time
 * and therefore cannot be registered through pg_proc.dat / LANGUAGE internal.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    contrib/tsvector_funcs/tsvector_funcs.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "nodes/value.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "fmgr.h"

PG_MODULE_MAGIC;

/*
 * pgvector Vector type definition (compatible with pgvector's Vector struct).
 * Defined locally to avoid header path dependency.
 */
#define VECTOR_MAX_DIM 16000
#define VECTOR_SIZE(_dim) (offsetof(Vector, x) + sizeof(float) * (_dim))

typedef struct Vector
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int16		dim;			/* number of dimensions */
	int16		unused;			/* reserved for future use, always zero */
	float		x[FLEXIBLE_ARRAY_MEMBER];
} Vector;

/* ====================================================================
 * ts2v: Convert a float8[] array to a vector of specified dimension.
 *
 * Applies min-max normalization to the input values, then pads by
 * repeating the pattern cyclically to fill the target dimension.
 * ==================================================================== */
PG_FUNCTION_INFO_V1(ts2v);
Datum
ts2v(PG_FUNCTION_ARGS)
{
	ArrayType  *arr;
	int			target_dim;
	Vector	   *result;
	Datum	   *values;
	bool	   *nulls;
	int			nvalues;
	int			i;
	float	   *input;
	float		min_val, max_val, range;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("input array must not be NULL")));

	arr = PG_GETARG_ARRAYTYPE_P(0);
	target_dim = PG_GETARG_INT32(1);

	if (target_dim <= 0 || target_dim > VECTOR_MAX_DIM)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("vector_len must be between 1 and %d", VECTOR_MAX_DIM)));

	deconstruct_array(arr, FLOAT8OID, 8, true, 'd', &values, &nulls, &nvalues);

	if (nvalues == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("input array must not be empty")));

	/* Extract float values */
	input = (float *) palloc(nvalues * sizeof(float));
	for (i = 0; i < nvalues; i++)
	{
		input[i] = nulls[i] ? 0.0f : (float) DatumGetFloat8(values[i]);
	}

	/* Min-max normalization to [0, 1] */
	min_val = max_val = input[0];
	for (i = 1; i < nvalues; i++)
	{
		if (input[i] < min_val)
			min_val = input[i];
		if (input[i] > max_val)
			max_val = input[i];
	}
	range = max_val - min_val;
	if (range > 0)
	{
		for (i = 0; i < nvalues; i++)
			input[i] = (input[i] - min_val) / range;
	}
	else
	{
		/* All values are the same; set to 0.5 (midpoint) */
		for (i = 0; i < nvalues; i++)
			input[i] = 0.5f;
	}

	/* Allocate and fill the result vector */
	result = (Vector *) palloc0(VECTOR_SIZE(target_dim));
	SET_VARSIZE(result, VECTOR_SIZE(target_dim));
	result->dim = target_dim;
	result->unused = 0;

	/* Pad by repeating the normalized pattern cyclically */
	for (i = 0; i < target_dim; i++)
		result->x[i] = input[i % nvalues];

	pfree(input);
	pfree(values);
	pfree(nulls);

	PG_RETURN_POINTER(result);
}

/* ====================================================================
 * timeseries_vector_run: Manually trigger vector computation for a
 * timeseries vector table.
 *
 * Reads metadata, finds complete unprocessed time slices, aggregates
 * numeric columns, calls the vectorize function, and inserts results.
 * ==================================================================== */
PG_FUNCTION_INFO_V1(timeseries_vector_run);
Datum
timeseries_vector_run(PG_FUNCTION_ARGS)
{
	text	   *vec_table_text = PG_GETARG_TEXT_PP(0);
	char	   *vec_table_name;
	StringInfo	query;
	StringInfo	carry_list;		/* "col1, col2, ..." */
	StringInfo	avg_exprs;		/* "avg(col1)::float8, avg(col2)::float8, ..." */
	StringInfo	insert_cols;	/* "slice_start, slice_end, carry_cols, vec_col" */
	StringInfo	group_by;		/* "1, 2, carry_cols" */
	int			processed = 0;
	Oid			source_oid;
	Oid			vec_oid;
	char	   *source_qualified = NULL;
	char	   *vec_qualified = NULL;
	char	   *bucket_interval = NULL;
	char	   *vector_column = NULL;
	char	   *vectorize_func = NULL;
	char	   *completion_delay = NULL;
	int			vector_len = 384;
	MemoryContext	oldcontext;
	char	   *time_col_name = NULL;
	List	   *carry_cols = NIL;
	List	   *numeric_cols = NIL;
	ListCell   *lc;
	int			ret;

	/* Save original context; vec_table_name is allocated here (before SPI_connect) */
	oldcontext = CurrentMemoryContext;
	vec_table_name = text_to_cstring(vec_table_text);

	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not connect to SPI")));

	/* ---- 1. Read metadata ---- */
	query = makeStringInfo();
	appendStringInfo(query,
		"SELECT source_table, bucket_interval, vector_column, "
		"vectorize_function, carry_columns, vector_len, completion_delay "
		"FROM _timescaledb_internal.timeseries_vector_tables "
		"WHERE vector_table = %s::regclass",
		quote_literal_cstr(vec_table_name));

	if (SPI_execute(query->data, true, 1) != SPI_OK_SELECT || SPI_processed == 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("no metadata found for vector table \"%s\"", vec_table_name)));

	{
		bool		isnull;
		Datum		d;

		/* source_table is REGCLASS (OID) */
		d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
		if (isnull)
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("source_table is NULL in metadata")));
		source_oid = DatumGetObjectId(d);

		d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull);
		bucket_interval = isnull ? pstrdup("1 hour") : TextDatumGetCString(d);

		d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 3, &isnull);
		vector_column = isnull ? pstrdup("embedding") : TextDatumGetCString(d);

		d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 4, &isnull);
		vectorize_func = isnull ? pstrdup("ts2v") : TextDatumGetCString(d);

		/* carry_columns is TEXT[] */
		d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 5, &isnull);
		if (!isnull)
		{
			ArrayType  *carry_arr = DatumGetArrayTypeP(d);
			ArrayIterator it = array_create_iterator(carry_arr, 0, NULL);
			Datum		elem;
			bool		elem_isnull;

			while (array_iterate(it, &elem, &elem_isnull))
			{
				if (!elem_isnull)
					carry_cols = lappend(carry_cols, TextDatumGetCString(elem));
			}
			array_free_iterator(it);
		}

		d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 6, &isnull);
		vector_len = isnull ? 384 : DatumGetInt32(d);

		d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 7, &isnull);
		completion_delay = isnull ? pstrdup("0 min") : TextDatumGetCString(d);
	}

	SPI_freetuptable(SPI_tuptable);

	/* Build qualified identifiers for source and vector tables */
	{
		char	   *relname;
		char	   *nspname;

		relname = get_rel_name(source_oid);
		nspname = get_namespace_name(get_rel_namespace(source_oid));
		source_qualified = quote_qualified_identifier(nspname, relname);

		vec_oid = get_relname_relid(vec_table_name, get_namespace_oid("public", false));
		if (!OidIsValid(vec_oid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("vector table \"%s\" not found", vec_table_name)));
		relname = get_rel_name(vec_oid);
		nspname = get_namespace_name(get_rel_namespace(vec_oid));
		vec_qualified = quote_qualified_identifier(nspname, relname);
	}

	/* ---- 2. Find time column (first TIMESTAMPTZ column) ---- */
	resetStringInfo(query);
	appendStringInfo(query,
		"SELECT a.attname FROM pg_attribute a "
		"JOIN pg_type t ON a.atttypid = t.oid "
		"WHERE a.attrelid = %s::regclass AND a.attnum > 0 AND NOT a.attisdropped "
		"AND t.typname = 'timestamptz' "
		"ORDER BY a.attnum LIMIT 1",
		quote_literal_cstr(source_qualified));

	if (SPI_execute(query->data, true, 1) == SPI_OK_SELECT && SPI_processed > 0)
	{
		time_col_name = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
	}
	SPI_freetuptable(SPI_tuptable);

	if (time_col_name == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("no timestamp column found in source table")));

	/* ---- 3. Find numeric columns (excluding time and carry) ---- */
	resetStringInfo(query);
	appendStringInfo(query,
		"SELECT a.attname FROM pg_attribute a "
		"JOIN pg_type t ON a.atttypid = t.oid "
		"WHERE a.attrelid = %s::regclass AND a.attnum > 0 AND NOT a.attisdropped "
		"AND t.typname IN ('int2','int4','int8','float4','float8','numeric') "
		"AND a.attname != %s "
		"ORDER BY a.attnum",
		quote_literal_cstr(source_qualified),
		quote_literal_cstr(time_col_name));

	if (SPI_execute(query->data, true, 0) == SPI_OK_SELECT)
	{
		int			i;

		for (i = 0; i < SPI_processed; i++)
		{
			char	   *colname = SPI_getvalue(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1);
			bool		is_carry = false;

			foreach(lc, carry_cols)
			{
				if (strcmp((char *) lfirst(lc), colname) == 0)
				{
					is_carry = true;
					break;
				}
			}
			if (!is_carry)
				numeric_cols = lappend(numeric_cols, colname);
		}
	}
	SPI_freetuptable(SPI_tuptable);

	if (numeric_cols == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("no numeric columns found in source table (excluding time and carry columns)")));

	/* ---- 4. Build INSERT...SELECT query ---- */
	carry_list = makeStringInfo();
	avg_exprs = makeStringInfo();
	insert_cols = makeStringInfo();
	group_by = makeStringInfo();

	/* Build carry columns list and insert column list */
	appendStringInfoString(insert_cols, "slice_start, slice_end");
	appendStringInfoString(group_by, "1, 2");

	foreach(lc, carry_cols)
	{
		char	   *colname = (char *) lfirst(lc);

		appendStringInfo(carry_list, "%s%s",
						 carry_list->len > 0 ? ", " : "",
						 quote_identifier(colname));
		appendStringInfo(insert_cols, ", %s", quote_identifier(colname));
		appendStringInfo(group_by, ", %s", quote_identifier(colname));
	}
	appendStringInfo(insert_cols, ", %s", quote_identifier(vector_column));

	/* Build avg expressions for numeric columns */
	foreach(lc, numeric_cols)
	{
		char	   *colname = (char *) lfirst(lc);

		appendStringInfo(avg_exprs, "%savg(%s)::float8",
						 avg_exprs->len > 0 ? ", " : "",
						 quote_identifier(colname));
	}

	/* Build the full INSERT...SELECT query */
	resetStringInfo(query);
	appendStringInfo(query,
		"INSERT INTO %s (%s) "
		"SELECT "
		"  'epoch'::timestamptz + "
		"    FLOOR(EXTRACT(epoch FROM %s) / EXTRACT(epoch FROM %s::interval)) "
		"    * %s::interval AS slice_start, "
		"  'epoch'::timestamptz + "
		"    (FLOOR(EXTRACT(epoch FROM %s) / EXTRACT(epoch FROM %s::interval)) + 1) "
		"    * %s::interval AS slice_end",
		vec_qualified,
		insert_cols->data,
		quote_identifier(time_col_name),
		quote_literal_cstr(bucket_interval),
		quote_literal_cstr(bucket_interval),
		quote_identifier(time_col_name),
		quote_literal_cstr(bucket_interval),
		quote_literal_cstr(bucket_interval));

	if (carry_list->len > 0)
		appendStringInfo(query, ", %s", carry_list->data);

	appendStringInfo(query,
		", %s(ARRAY[%s]::float8[], %d)",
		quote_identifier(vectorize_func),
		avg_exprs->data,
		vector_len);

	appendStringInfo(query,
		" FROM %s WHERE %s < NOW() - %s::interval"
		" GROUP BY %s"
		" ON CONFLICT DO NOTHING",
		source_qualified,
		quote_identifier(time_col_name),
		quote_literal_cstr(completion_delay),
		group_by->data);

	/* ---- 5. Execute ---- */
	ret = SPI_execute(query->data, false, 0);

	if (ret == SPI_OK_INSERT)
		processed = SPI_processed;
	else
		elog(WARNING, "timeseries_vector_run: unexpected SPI result %d", ret);

	/* Switch to original context so result text survives SPI_finish */
	MemoryContextSwitchTo(oldcontext);
	{
		char	   *msg = psprintf("Processed %d time slice(s) for vector table \"%s\"",
									processed, vec_table_name);
		text	   *ret_text = cstring_to_text(msg);

		SPI_finish();

		PG_RETURN_TEXT_P(ret_text);
	}
}
