/*-------------------------------------------------------------------------
 *
 * tsvector_funcs.c
 *    Timeseries vector helper functions: ts2v_moment and timeseries_vector_run.
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

#include <math.h>

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
 * ts2v_moment: Convert a float8[] array (a time series) to a vector of the
 * requested dimension using moment-based feature extraction.
 *
 * Statistical moments describe the shape of the distribution of the
 * values regardless of order within the window:
 *
 *   1. mean       -- 1st raw moment (location)
 *   2. stddev     -- sqrt of the 2nd central moment (scale)
 *   3. skewness   -- 3rd standardized moment (asymmetry)
 *   4. kurtosis   -- 4th standardized moment (tailedness)
 *   5..8.         -- 5th..8th standardized moments (higher-order shape)
 *
 * Standardized moments are translation/scale invariant, so they are
 * comparable across time slices with different levels and units.
 *
 * Each feature is bounded to [-1, 1] with the soft-sign transform
 * f(v) = v / (1 + |v|); the bounded feature vector is then cyclically
 * repeated to fill the requested vector_len.
 * ==================================================================== */

#define MOMENT_MAX_ORDER	8		/* highest standardized moment order */
#define MOMENT_NFEATURES	8		/* mean + stddev + orders 3..8 */

/* Bounded, monotonic, sign-preserving normalization to (-1, 1). */
static inline float
softsign(double v)
{
	return (float) (v / (1.0 + fabs(v)));
}

PG_FUNCTION_INFO_V1(ts2v_moment);
Datum
ts2v_moment(PG_FUNCTION_ARGS)
{
	ArrayType  *arr;
	int			target_dim;
	Vector	   *result;
	Datum	   *values;
	bool	   *nulls;
	int			nvalues;
	int			i;
	int			k;
	double	   *x;
	double		mean = 0.0;
	double		var = 0.0;
	double		std;
	double		mom[MOMENT_MAX_ORDER + 1];
	float		feat[MOMENT_NFEATURES];

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

	x = (double *) palloc(nvalues * sizeof(double));
	for (i = 0; i < nvalues; i++)
		x[i] = nulls[i] ? 0.0 : DatumGetFloat8(values[i]);

	/* 1st raw moment: mean */
	for (i = 0; i < nvalues; i++)
		mean += x[i];
	mean /= nvalues;

	/* 2nd central moment: variance, then stddev */
	for (i = 0; i < nvalues; i++)
	{
		double		d = x[i] - mean;

		var += d * d;
	}
	var /= nvalues;
	std = sqrt(var);

	/* Mean and stddev describe location and scale. */
	feat[0] = softsign(mean);
	feat[1] = softsign(std);

	/* 3rd..8th standardized moments describe shape (skewness, kurtosis, ...). */
	if (std > 0.0)
	{
		for (k = 0; k <= MOMENT_MAX_ORDER; k++)
			mom[k] = 0.0;

		for (i = 0; i < nvalues; i++)
		{
			double		z = (x[i] - mean) / std;
			double		p = 1.0;

			for (k = 0; k <= MOMENT_MAX_ORDER; k++)
			{
				mom[k] += p;
				p *= z;
			}
		}

		for (k = 3; k <= MOMENT_MAX_ORDER; k++)
			feat[k - 1] = softsign(mom[k] / nvalues);
	}
	else
	{
		/* Constant series: no shape variation, higher moments are zero. */
		for (k = 2; k < MOMENT_NFEATURES; k++)
			feat[k] = 0.0f;
	}

	/* Allocate and fill the result vector, repeating features cyclically. */
	result = (Vector *) palloc0(VECTOR_SIZE(target_dim));
	SET_VARSIZE(result, VECTOR_SIZE(target_dim));
	result->dim = target_dim;
	result->unused = 0;

	for (i = 0; i < target_dim; i++)
		result->x[i] = feat[i % MOMENT_NFEATURES];

	pfree(x);
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
	int			bucket_interval = 3600;	/* 秒 */
	char	   *vector_column = NULL;
	char	   *vectorize_func = NULL;
	int			completion_delay = 0;	/* 秒 */
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

	/* ---- 1. Read configuration from pg_class.reloptions ---- */
	query = makeStringInfo();
	appendStringInfo(query,
		"SELECT "
		"  (SELECT (regexp_match(opt, '^timeseries.source=(.+)$'))[1] "
		"     FROM unnest(reloptions) opt "
		"     WHERE regexp_match(opt, '^timeseries.source=(.+)$') IS NOT NULL) AS source, "
		"  (SELECT (regexp_match(opt, '^timeseries.bucket_interval=(.+)$'))[1] "
		"     FROM unnest(reloptions) opt "
		"     WHERE regexp_match(opt, '^timeseries.bucket_interval=(.+)$') IS NOT NULL) AS bucket_interval, "
		"  (SELECT (regexp_match(opt, '^timeseries.vector_column=(.+)$'))[1] "
		"     FROM unnest(reloptions) opt "
		"     WHERE regexp_match(opt, '^timeseries.vector_column=(.+)$') IS NOT NULL) AS vector_column, "
		"  (SELECT (regexp_match(opt, '^timeseries.vectorize_function=(.+)$'))[1] "
		"     FROM unnest(reloptions) opt "
		"     WHERE regexp_match(opt, '^timeseries.vectorize_function=(.+)$') IS NOT NULL) AS vectorize_func, "
		"  (SELECT (regexp_match(opt, '^timeseries.carry_columns=(.+)$'))[1] "
		"     FROM unnest(reloptions) opt "
		"     WHERE regexp_match(opt, '^timeseries.carry_columns=(.+)$') IS NOT NULL) AS carry_cols, "
		"  (SELECT (regexp_match(opt, '^timeseries.vector_len=(.+)$'))[1] "
		"     FROM unnest(reloptions) opt "
		"     WHERE regexp_match(opt, '^timeseries.vector_len=(.+)$') IS NOT NULL) AS vector_len, "
		"  (SELECT (regexp_match(opt, '^timeseries.completion_delay=(.+)$'))[1] "
		"     FROM unnest(reloptions) opt "
		"     WHERE regexp_match(opt, '^timeseries.completion_delay=(.+)$') IS NOT NULL) AS completion_delay "
		"FROM pg_class WHERE oid = %s::regclass",
		quote_literal_cstr(vec_table_name));

	if (SPI_execute(query->data, true, 1) != SPI_OK_SELECT || SPI_processed == 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("vector table \"%s\" not found or not a timeseries vector table",
						vec_table_name)));

	{
		bool		isnull;
		Datum		d;
		char	   *source_name;

		d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
		if (isnull)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("timeseries.source not set for table \"%s\"", vec_table_name)));
		source_name = TextDatumGetCString(d);

		/* Resolve source table name to OID */
		{
			Oid	source_oid_tmp = DirectFunctionCall1(regclassin,
													 CStringGetDatum(source_name));
			if (!OidIsValid(source_oid_tmp))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_TABLE),
						 errmsg("source table \"%s\" not found", source_name)));
			source_oid = source_oid_tmp;
		}

		d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull);
		bucket_interval = isnull ? 3600 : pg_strtoint32(TextDatumGetCString(d));

		d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 3, &isnull);
		vector_column = isnull ? pstrdup("embedding") : TextDatumGetCString(d);

		d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 4, &isnull);
		vectorize_func = isnull ? pstrdup("ts2v_moment") : TextDatumGetCString(d);

		/* carry_columns is comma-separated string */
		d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 5, &isnull);
		if (!isnull)
		{
			char	   *carry_str = TextDatumGetCString(d);
			char	   *copy = pstrdup(carry_str);
			char	   *tok;

			for (tok = strtok(copy, ", "); tok != NULL; tok = strtok(NULL, ", "))
				carry_cols = lappend(carry_cols, pstrdup(tok));
			pfree(copy);
		}

		d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 6, &isnull);
		vector_len = isnull ? 384 : pg_strtoint64(TextDatumGetCString(d));

		d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 7, &isnull);
		completion_delay = isnull ? 0 : pg_strtoint32(TextDatumGetCString(d));
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

	/* Build the full INSERT...SELECT query.
	 * bucket_interval is now an integer (seconds), so the time-bucket math
	 * simplifies: EXTRACT(epoch FROM bucket_interval::interval) == bucket_interval,
	 * and multiplying back uses make_interval(secs => bucket_interval).
	 */
	resetStringInfo(query);
	appendStringInfo(query,
		"INSERT INTO %s (%s) "
		"SELECT "
		"  'epoch'::timestamptz + "
		"    FLOOR(EXTRACT(epoch FROM %s) / %d) "
		"    * make_interval(secs => %d) AS slice_start, "
		"  'epoch'::timestamptz + "
		"    (FLOOR(EXTRACT(epoch FROM %s) / %d) + 1) "
		"    * make_interval(secs => %d) AS slice_end",
		vec_qualified,
		insert_cols->data,
		quote_identifier(time_col_name),
		bucket_interval,
		bucket_interval,
		quote_identifier(time_col_name),
		bucket_interval,
		bucket_interval);

	if (carry_list->len > 0)
		appendStringInfo(query, ", %s", carry_list->data);

	appendStringInfo(query,
		", %s(ARRAY[%s]::float8[], %d)",
		quote_identifier(vectorize_func),
		avg_exprs->data,
		vector_len);

	appendStringInfo(query,
		" FROM %s WHERE %s < NOW() - make_interval(secs => %d)"
		" GROUP BY %s"
		" ON CONFLICT DO NOTHING",
		source_qualified,
		quote_identifier(time_col_name),
		completion_delay,
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
