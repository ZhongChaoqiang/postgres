/*-------------------------------------------------------------------------
 *
 * tsvector_funcs.c
 *    Timeseries vector helper functions:
 *      ts2v_moment                 - time-series -> vector
 *      timeseries_vector_run       - manual trigger
 *      tsvector_trigger_func       - AFTER INSERT trigger on source table
 *      tsvector_worker_main        - Background Worker entry point
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
#include <string.h>

#include "access/htup_details.h"
#include "access/xact.h"
#include "utils/snapmgr.h"
#include "catalog/namespace.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/value.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "storage/proc.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

PG_MODULE_MAGIC;

/* ====================================================================
 * pgvector Vector type definition (compatible with pgvector's Vector struct).
 * ==================================================================== */
#define VECTOR_MAX_DIM 16000
#define VECTOR_SIZE(_dim) (offsetof(Vector, x) + sizeof(float) * (_dim))

typedef struct Vector
{
	int32		vl_len_;
	int16		dim;
	int16		unused;
	float		x[FLEXIBLE_ARRAY_MEMBER];
} Vector;

/* ====================================================================
 * GUC parameters
 * ==================================================================== */
static int   tsvector_workers = 1;
static int   tsvector_max_in_flight = 64;
static int   tsvector_trigger_cache_size = 512;

/* forward declarations (BGW module section appears before helpers) */
static int run_vector_table(Oid vec_oid);
static StringInfo build_vector_run_query(Oid vec_oid, int bucket_interval,
										 int vector_len, const char *source_name,
										 const char *vector_column,
										 const char *vectorize_fn,
										 List *carry_cols,
										 const char *value_column,
										 const char *time_column);

static void
tsvector_define_gucs(void)
{
	DefineCustomIntVariable("timeseries.workers",
							"Number of timeseries vector background workers.",
							NULL,
							&tsvector_workers,
							1, 1, 16,
							PGC_SIGHUP, 0, NULL, NULL, NULL);

	DefineCustomIntVariable("timeseries.max_in_flight",
							"Maximum in-flight tasks per worker (backpressure threshold).",
							NULL,
							&tsvector_max_in_flight,
							64, 4, 1024,
							PGC_SIGHUP, 0, NULL, NULL, NULL);

	DefineCustomIntVariable("timeseries.trigger_cache_size",
							"LRU cache entries per trigger backend.",
							NULL,
							&tsvector_trigger_cache_size,
							512, 16, 8192,
							PGC_SIGHUP, 0, NULL, NULL, NULL);
}

/* ====================================================================
 * LRU cache module (per-backend, process-local).
 * Key: vec_oid + slice_start + carry_hash
 * Value: bool is_done
 * ==================================================================== */

typedef struct LruEntry
{
	Oid			vec_oid;
	TimestampTz slice_start;
	uint32		carry_hash;
	bool		is_done;
	struct LruEntry *prev;
	struct LruEntry *next;
} LruEntry;

typedef struct LruCache
{
	HTAB	   *htab;
	LruEntry   *head;
	LruEntry   *tail;
	int			max_entries;
	int			count;
} LruCache;

static LruCache *trigger_cache = NULL;

static LruCache *
lru_create(int max_entries)
{
	HASHCTL		ctl;
	LruCache   *cache;

	cache = palloc0(sizeof(LruCache));
	cache->max_entries = max_entries;

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(Oid) + sizeof(TimestampTz) + sizeof(uint32);
	ctl.entrysize = sizeof(LruEntry);
	cache->htab = hash_create("tsvector_trigger_cache",
							  max_entries, &ctl, HASH_ELEM | HASH_BLOBS);
	return cache;
}

static void
lru_unlink(LruCache *cache, LruEntry *e)
{
	if (e->prev)
		e->prev->next = e->next;
	else
		cache->head = e->next;
	if (e->next)
		e->next->prev = e->prev;
	else
		cache->tail = e->prev;
	e->prev = NULL;
	e->next = NULL;
}

static void
lru_push_head(LruCache *cache, LruEntry *e)
{
	e->prev = NULL;
	e->next = cache->head;
	if (cache->head)
		cache->head->prev = e;
	cache->head = e;
	if (!cache->tail)
		cache->tail = e;
}

static int
lru_get(LruCache *cache, Oid vec_oid, TimestampTz slice_start,
		uint32 carry_hash)
{
	LruEntry	key;
	LruEntry   *entry;

	if (!cache)
		return -1;

	memset(&key, 0, sizeof(key));
	key.vec_oid = vec_oid;
	key.slice_start = slice_start;
	key.carry_hash = carry_hash;

	entry = hash_search(cache->htab, &key, HASH_FIND, NULL);
	if (!entry)
		return -1;

	lru_unlink(cache, entry);
	lru_push_head(cache, entry);
	return entry->is_done;
}

static void
lru_put(LruCache *cache, Oid vec_oid, TimestampTz slice_start,
		uint32 carry_hash, bool is_done)
{
	LruEntry	key;
	LruEntry   *entry;

	if (!cache)
		return;

	memset(&key, 0, sizeof(key));
	key.vec_oid = vec_oid;
	key.slice_start = slice_start;
	key.carry_hash = carry_hash;

	entry = hash_search(cache->htab, &key, HASH_FIND, NULL);
	if (entry)
	{
		entry->is_done = is_done;
		lru_unlink(cache, entry);
		lru_push_head(cache, entry);
		return;
	}

	if (cache->count >= cache->max_entries && cache->tail)
	{
		LruEntry   *victim = cache->tail;

		lru_unlink(cache, victim);
		hash_search(cache->htab, victim, HASH_REMOVE, NULL);
		cache->count--;
	}

	entry = hash_search(cache->htab, &key, HASH_ENTER, NULL);
	entry->is_done = is_done;
	lru_push_head(cache, entry);
	cache->count++;
}

/* ====================================================================
 * Helper: read a single relopt from pg_class via its own SPI context.
 *
 * Each call opens and closes its own SPI_connect/SPI_finish, so it's
 * safe to call from inside or outside any caller's SPI context — no
 * stack nesting and no "non-empty SPI stack" warnings at commit time.
 * ==================================================================== */

static char *
read_relopt_raw(Oid relid, const char *name)
{
	StringInfo	si;
	char	   *result = NULL;
	char		key_prefix[256];
	int			ret;

	snprintf(key_prefix, sizeof(key_prefix), "%s=", name);

	si = makeStringInfo();
	appendStringInfo(si,
		"SELECT opt FROM pg_class, "
		"LATERAL unnest(reloptions) AS opt "
		"WHERE oid = %u AND opt LIKE '%s%%' "
		"LIMIT 1",
		relid, key_prefix);

	if (SPI_connect() == SPI_OK_CONNECT)
	{
		ret = SPI_execute(si->data, true, 1);
		if (ret == SPI_OK_SELECT && SPI_processed > 0)
		{
			char   *opt = SPI_getvalue(SPI_tuptable->vals[0],
									   SPI_tuptable->tupdesc, 1);

			if (opt)
			{
				const char *eq = strchr(opt, '=');

				if (eq)
				{
					/*
					 * Copy into TopMemoryContext before SPI_finish frees
					 * the SPI context that pstrdup would otherwise use.
					 * Without this, the returned pointer becomes dangling
					 * and may be overwritten by subsequent relopt reads.
					 */
					MemoryContext old_ctx = CurrentMemoryContext;
					MemoryContextSwitchTo(TopMemoryContext);
					result = pstrdup(eq + 1);
					MemoryContextSwitchTo(old_ctx);
				}
				pfree(opt);
			}
			SPI_freetuptable(SPI_tuptable);
		}
		SPI_finish();
	}

	pfree(si->data);
	pfree(si);
	return result;
}

static int
read_relopt_int(Oid relid, const char *name, int def)
{
	char	   *val;

	val = read_relopt_raw(relid, name);
	if (!val)
		return def;

	{
		int			v = atoi(val);

		pfree(val);
		return v;
	}
}

static char *
read_relopt_text(Oid relid, const char *name)
{
	char	   *val;

	val = read_relopt_raw(relid, name);
	if (!val)
		return NULL;

	/* Strip surrounding quotes if present */
	if (val[0] == '\'' && val[strlen(val) - 1] == '\'')
	{
		val[strlen(val) - 1] = '\0';
		memmove(val, val + 1, strlen(val));
	}
	return val;
}

static bool
read_relopt_bool(Oid relid, const char *name, bool def)
{
	char	   *val;
	bool		result;

	val = read_relopt_raw(relid, name);
	if (!val)
		return def;

	result = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
	pfree(val);
	return result;
}

static bool
check_slice_done(Oid vec_oid, TimestampTz slice_start, uint32 carry_hash)
{
	StringInfo	si = makeStringInfo();
	int			ret;
	bool		is_done = false;

	appendStringInfo(si,
		"SELECT 1 FROM %s WHERE slice_start = %lld AND carry_hash = %u LIMIT 1",
		get_rel_name(vec_oid), (long long) slice_start, carry_hash);

	ret = SPI_execute(si->data, true, 1);
	if (ret == SPI_OK_SELECT && SPI_processed > 0)
		is_done = true;

	pfree(si->data);
	return is_done;
}

/* ====================================================================
 * Trigger function: AFTER INSERT on source table
 * ==================================================================== */

PG_FUNCTION_INFO_V1(tsvector_trigger_func);

/*
 * Simplified trigger: just sends a pg_notify hint to the BGW.
 * The BGW is responsible for discovering which slices need computation.
 * This keeps the trigger very fast (O(1) per row).
 */
Datum
tsvector_trigger_func(PG_FUNCTION_ARGS)
{
	TriggerData *tdata = (TriggerData *) fcinfo->context;
	Relation	source_rel;
	Oid			src_oid;
	char	   *src_name;

	if (!CALLED_AS_TRIGGER(fcinfo))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("tsvector_trigger_func: must be called as trigger")));

	if (!TRIGGER_FIRED_BY_INSERT(tdata->tg_event) || TRIGGER_FIRED_BEFORE(tdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("tsvector_trigger_func: must be AFTER INSERT")));

	source_rel = tdata->tg_relation;
	src_oid = RelationGetRelid(source_rel);

	if (!TRIGGER_FIRED_FOR_ROW(tdata->tg_event))
		return (Datum) tdata->tg_trigtuple;

	src_name = get_rel_name(src_oid);

	elog(DEBUG2, "tsvector_trigger_func: INSERT on %s (oid=%u)",
		 src_name ? src_name : "?", src_oid);

	/* Notify BGW: minimal payload, BGW does the real work */
	if (SPI_connect() == SPI_OK_CONNECT)
	{
		StringInfo	payload = makeStringInfo();

		appendStringInfo(payload,
			"{\"oid\":%u,\"name\":\"%s\"}",
			src_oid, src_name ? src_name : "");

		SPI_execute_with_args(
			"SELECT pg_notify('tsvector_task', $1::text)",
			1, (Oid[]) {TEXTOID},
			(Datum[]) {CStringGetDatum(payload->data)},
			" ", false, 0);

		pfree(payload->data);
		SPI_finish();
	}

	return (Datum) tdata->tg_newtuple;
}

/* ====================================================================
 * Background Worker discovery & execution
 *
 * Design: simple poller that scans pg_class for every vector table
 * (one with a timeseries.source reloption) and calls run_vector_table()
 * on it. run_vector_table() uses ON CONFLICT DO UPDATE, so repeated
 * calls are idempotent — we don't need slice-level tracking.
 *
 * The trigger still sends pg_notify('tsvector_task', src_oid) as a
 * low-latency hint; the BGW does LISTEN on that channel so a fresh
 * insert wakes it up immediately instead of waiting for the poll
 * interval.
 * ==================================================================== */

/* ====================================================================
 * execute_task — thin wrapper, kept as a named entry point for clarity.
 * The actual work is all in run_vector_table().
 * ==================================================================== */
static void
execute_task(Oid vec_oid, const char *vec_name)
{
	int			nrows = run_vector_table(vec_oid);

	elog(LOG, "tsvector_worker: processed %s (oid=%u), rows=%d",
		 vec_name, vec_oid, nrows);
}

/* ====================================================================
 * BGW main
 *
 * Two-phase poller (1s interval):
 *   Phase 1 — scan pg_class for vec table OIDs, then SPI_finish
 *   Phase 2 — for each vec_oid, call run_vector_table() which does its
 *             own SPI_connect/SPI_finish (no nesting)
 *
 * Every vec table gets its own clean SPI context; one failing table
 * (e.g. vec dropped) doesn't break the scan loop.
 *
 * BGW transaction/snapshot model:
 *   PostgreSQL BGWs have no automatic transaction or active snapshot.
 *   Before any SPI_execute we MUST:
 *     StartTransactionCommand() + PushActiveSnapshot(GetTransactionSnapshot())
 *   and after:
 *     PopActiveSnapshot() + CommitTransactionCommand()
 *
 * Note: PostgreSQL does NOT allow LISTEN/NOTIFY inside background
 * workers, so we use pure polling. The trigger still sends pg_notify
 * as a hint for any future frontend-based consumer.
 * ==================================================================== */

PGDLLEXPORT void
tsvector_worker_main(Datum main_arg)
{
	BackgroundWorkerInitializeConnection("postgres", NULL, 0);

	elog(LOG, "tsvector_worker: started, polling for timeseries vector tables");

	while (true)
	{
		/* ============================================================
		 * Phase 1 — discover vec table OIDs (own transaction + SPI)
		 * ============================================================ */
		Oid			vec_oids[64];
		int			n_vecs = 0;
		int			i;

		StartTransactionCommand();
		PushActiveSnapshot(GetTransactionSnapshot());

		SPI_connect();
		{
			StringInfo	q = makeStringInfo();
			int			ret;

			appendStringInfo(q,
				"SELECT c.oid FROM pg_class c, LATERAL unnest(c.reloptions) AS opt "
				"WHERE opt LIKE 'timeseries.source=%%' "
				"  AND c.relkind = 'r'::char "
				"LIMIT 64");

			ret = SPI_execute(q->data, true, 0);

			if (ret == SPI_OK_SELECT && SPI_processed > 0)
			{
				TupleDesc	tupdesc = SPI_tuptable->tupdesc;
				int			t;

				for (t = 0; t < SPI_processed && n_vecs < 64; t++)
				{
					HeapTuple	htup = SPI_tuptable->vals[t];
					bool		isnull;
					Oid			oid;

					oid = DatumGetObjectId(SPI_getbinval(htup, tupdesc, 1, &isnull));
					if (!isnull && OidIsValid(oid))
						vec_oids[n_vecs++] = oid;
				}
				SPI_freetuptable(SPI_tuptable);
			}
			pfree(q->data);
			pfree(q);
		}
		SPI_finish();

		PopActiveSnapshot();
		CommitTransactionCommand();
		/* --- Phase 1 transaction cleanly closed --- */

		elog(LOG, "tsvector_worker: discovered %d vec tables", n_vecs);

		/* ============================================================
		 * Phase 2 — run each vec table in its OWN transaction + SPI
		 * ============================================================ */
		for (i = 0; i < n_vecs; i++)
		{
			StartTransactionCommand();
			PushActiveSnapshot(GetTransactionSnapshot());

			PG_TRY();
			{
				int			nrows = run_vector_table(vec_oids[i]);

				elog(LOG, "tsvector_worker: vec_oid=%u -> %d rows",
					 vec_oids[i], nrows);
			}
			PG_CATCH();
			{
				FlushErrorState();
			}
			PG_END_TRY();

			PopActiveSnapshot();
			CommitTransactionCommand();
			/* --- per-table transaction cleanly closed --- */
		}

		/* ---- sleep 1s, wake early on postmaster death ---- */
		WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
				  1000L, WAIT_EVENT_PG_SLEEP);
		ResetLatch(MyLatch);
	}
}

/* ====================================================================
 * ts2v_moment — convert timeseries rows to moment-based vector
 * ==================================================================== */

static double
softsign(double v)
{
	return v / (1.0 + fabs(v));
}

PG_FUNCTION_INFO_V1(ts2v_moment);

Datum
ts2v_moment(PG_FUNCTION_ARGS)
{
	ArrayType  *input = PG_GETARG_ARRAYTYPE_P(0);
	int			target_dim = PG_GETARG_INT32(1);
	Vector	   *result;
	float	   *out;
	int			input_dim;
	double		mean = 0.0;
	double		min_v = INFINITY;
	double		max_v = -INFINITY;
	double		sum_sq = 0.0;
	double		skew_num = 0.0;
	double		kurt_num = 0.0;
	Datum	   *elems;
	bool	   *nulls;
	int			count;
	int			i;

	if (target_dim <= 0 || target_dim > VECTOR_MAX_DIM)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("target_dim must be between 1 and %d", VECTOR_MAX_DIM)));

	deconstruct_array(input, FLOAT8OID, 8, true, 'd',
					  &elems, &nulls, &count);
	input_dim = count;
	if (input_dim <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("input array is empty")));

	/* First pass: basic stats */
	for (i = 0; i < count; i++)
	{
		double		v;

		if (nulls[i])
			continue;

		v = DatumGetFloat8(elems[i]);
		mean += v;
		if (v < min_v)
			min_v = v;
		if (v > max_v)
			max_v = v;
		sum_sq += v * v;
	}
	mean /= count;

	{
		double		var;
		double		stddev;

		var = sum_sq / count - mean * mean;
		stddev = sqrt(var);

		for (i = 0; i < count; i++)
		{
			double		v;
			double		d;

			if (nulls[i])
				continue;

			v = DatumGetFloat8(elems[i]);
			d = (v - mean) / (stddev > 0 ? stddev : 1.0);

			skew_num += d * d * d;
			kurt_num += d * d * d * d;
		}
		skew_num /= count;
		kurt_num = kurt_num / count - 3.0;
	}

	/* Build output vector */
	result = palloc0(VECTOR_SIZE(target_dim));
	SET_VARSIZE(result, VECTOR_SIZE(target_dim));
	result->dim = target_dim;
	result->unused = 0;
	out = result->x;

	/* First 6 dims: global features */
	out[0] = (float) softsign(mean);
	out[1] = (float) softsign(sqrt(sum_sq / count - mean * mean));
	out[2] = (float) softsign(min_v);
	out[3] = (float) softsign(max_v);
	out[4] = (float) softsign(skew_num);
	out[5] = (float) softsign(kurt_num);

	/* Remaining dims: quantile-based bucketing */
	{
		double		qrange;
		int			n_buckets;

		n_buckets = target_dim - 6;
		if (n_buckets <= 0)
		{
			pfree(elems);
			pfree(nulls);
			PG_RETURN_POINTER(result);
		}

		qrange = max_v - min_v;
		if (qrange < 1e-15)
			qrange = 1.0;

		for (i = 0; i < count && n_buckets > 0; i++)
		{
			int			b;
			double		v;

			if (nulls[i])
				continue;

			v = DatumGetFloat8(elems[i]);
			b = (int) ((v - min_v) / qrange * (n_buckets - 1));
			if (b < 0)
				b = 0;
			if (b >= n_buckets)
				b = n_buckets - 1;

			out[6 + b] += 1.0f;
		}

		/* Normalize */
		{
			float		max_h = 0.0f;

			for (i = 6; i < target_dim; i++)
				if (out[i] > max_h)
					max_h = out[i];
			if (max_h > 0)
				for (i = 6; i < target_dim; i++)
					out[i] /= max_h;
		}
	}

	pfree(elems);
	pfree(nulls);
	PG_RETURN_POINTER(result);
}

/* ====================================================================
 * parse_comma_list — split a comma-separated string into a List of strings.
 * Empty entries are skipped. Caller is responsible for pfree'ing each
 * element and the list itself.
 * ==================================================================== */
static List *
parse_comma_list(const char *s)
{
	List	   *result = NIL;
	const char *p;
	const char *start;

	if (s == NULL || *s == '\0')
		return result;

	p = s;
	while (*p)
	{
		/* skip leading whitespace */
		while (*p == ' ' || *p == '\t')
			p++;
		start = p;
		/* read until comma or end */
		while (*p && *p != ',')
			p++;
		if (p > start)
			result = lappend(result, pnstrdup(start, p - start));
		if (*p == ',')
			p++;
	}
	return result;
}

/* ====================================================================
 * build_vector_run_query — assemble the INSERT...SELECT query that
 * populates a timeseries vector table from its source table.
 *
 * Column layout matches what InjectTimeseriesColumns() injects:
 *   slice_start, slice_end, [carry columns...], embedding, _row_count
 * Primary key: (slice_start, [carry columns...])
 *
 * All memory used by this function lives in the SPI context because
 * appendStringInfo grows the StringInfo's own palloc'd buffer and we
 * never free intermediate pieces — the returned StringInfo is owned by
 * the caller who is responsible for pfree(query->data)/pfree(query).
 * ==================================================================== */
static StringInfo
build_vector_run_query(Oid vec_oid, int bucket_interval,
						int vector_len,
						const char *source_name,
						const char *vector_column,
						const char *vectorize_fn,
						List *carry_cols,
						const char *value_column,
						const char *time_column)
{
	StringInfo	q = makeStringInfo();
	ListCell   *lc;

	if (vector_column == NULL || *vector_column == '\0')
		vector_column = "embedding";

	if (vectorize_fn == NULL || *vectorize_fn == '\0')
		vectorize_fn = "ts2v_moment";

	if (value_column == NULL || *value_column == '\0')
		elog(ERROR,
			 "timeseries.value_column reloption not set; please specify "
			 "which source column contains the numeric values to aggregate");

	/* time_column defaults to 'time' if not set — preserves backward compat. */
	if (time_column == NULL || *time_column == '\0')
		time_column = "time";

	/* quote_ident for SQL-safety — fn names come from user reloptions */
	StringInfo	fn_quoted = makeStringInfo();
	appendStringInfoString(fn_quoted, quote_identifier(vectorize_fn));

	/* --- helper strings built via pstrdup / psprintf (both palloc) --- */
	StringInfo	vec_col_clause = makeStringInfo();	/* ", carry1, carry2" in target list */
	StringInfo	grp_clause = makeStringInfo();		/* ", carry1, carry2" in GROUP BY   */
	StringInfo	pk_clause = makeStringInfo();		/* ", carry1, carry2" in ON CONFLICT */
	StringInfo	inner_sel = makeStringInfo();		/* inner SELECT expression list      */
	const char *vec_name = get_rel_name(vec_oid);

	/* comma-separated carry lists */
	foreach(lc, carry_cols)
	{
		const char *c = (const char *) lfirst(lc);

		appendStringInfoChar(vec_col_clause, ',');
		appendStringInfoChar(grp_clause, ',');
		appendStringInfoChar(pk_clause, ',');
		appendStringInfo(vec_col_clause, " %s", c);
		appendStringInfo(grp_clause, " %s", c);
		appendStringInfo(pk_clause, " %s", c);
	}

	/*
	 * inner SELECT: time, bucket_epoch, [carries...], value
	 *
	 * Use FLOOR() to avoid PostgreSQL's double-to-bigint cast rounding
	 * 496994.5 -> 496995 (ties-away-from-zero rounding). FLOOR gives the
	 * correct "bucket floor" semantics: every event at or after
	 * bucket_epoch belongs to that bucket.
	 */
	appendStringInfo(inner_sel,
					 "%s, FLOOR(EXTRACT(EPOCH FROM %s) / %d)::bigint * %d AS bucket_epoch",
					 time_column, time_column, bucket_interval, bucket_interval);
	foreach(lc, carry_cols)
	{
		const char *c = (const char *) lfirst(lc);
		appendStringInfo(inner_sel, ", %s", c);
	}
	appendStringInfo(inner_sel, ", %s", value_column);

	/* --- full INSERT ... SELECT --- */
	appendStringInfo(q,
		"INSERT INTO %s (slice_start, slice_end%s, %s, _row_count, _processed) "
		"SELECT "
		"  (to_timestamp(bucket_epoch))::timestamptz AS slice_start, "
		"  (to_timestamp(bucket_epoch + %d))::timestamptz AS slice_end"
		"%s, "
		"  %s(array_agg(%s ORDER BY %s), %d) AS embedding, "
		"  count(*) AS _row_count, "
		"  true AS _processed "
		"FROM (SELECT %s FROM %s) sub "
		"GROUP BY bucket_epoch%s "
		"ON CONFLICT (slice_start%s) DO UPDATE SET "
		"  %s = EXCLUDED.%s, "
		"  _row_count = EXCLUDED._row_count, "
		"  _processed = true",
		vec_name,
		vec_col_clause->data,
		vector_column,
		bucket_interval,
		vec_col_clause->data,
		fn_quoted->data,
		value_column,
			time_column,
		vector_len,
		inner_sel->data,
		source_name,
		grp_clause->data,
		pk_clause->data,
		vector_column, vector_column);

	/*
	 * Free intermediate StringInfo containers. The caller will free q.
	 * The StringInfo->data buffers are palloc'd in the top-level SPI
	 * memory context, so they will also be reclaimed when SPI_finish()
	 * is called — but freeing them explicitly here avoids leaving a
	 * bunch of unreferenced palloc chunks.
	 */
	pfree(vec_col_clause->data); pfree(vec_col_clause);
	pfree(grp_clause->data);     pfree(grp_clause);
	pfree(pk_clause->data);      pfree(pk_clause);
	pfree(inner_sel->data);     pfree(inner_sel);
	pfree(fn_quoted->data);     pfree(fn_quoted);

	return q;
}

/* ====================================================================
 * timeseries_vector_run — manual trigger for one vector table
 * ==================================================================== */

/* ====================================================================
 * run_vector_table — shared entry point used by both the SQL function
 * timeseries_vector_run() and the background worker execute_task().
 * Reads all reloptions, builds the dynamic INSERT ... SELECT via
 * build_vector_run_query(), executes it through SPI, and returns the
 * number of rows affected.
 *
 * Caller is responsible for SPI_connect() / SPI_finish() if running
 * outside a SQL function context; this helper always does its own.
 * ==================================================================== */
static int
run_vector_table(Oid vec_oid)
{
	int			bucket_interval;
	char	   *source_name;
	int			vector_len;
	char	   *vector_column;
	char	   *carry_str;
	char	   *value_column;
	const char *vectorize_fn;
	char	   *time_column;
	List	   *carry_cols;
	StringInfo	query;
	int			ret;
	int			nrows = 0;
	bool		spi_connected = false;

	/*
	 * Read relopts FIRST (each read_relopt_raw call manages its own
	 * SPI_connect/SPI_finish independently). We must NOT hold an outer
	 * SPI connection here, because PostgreSQL SPI does not allow nested
	 * SPI_connect() calls — they return SPI_ERROR_ATTACHED silently.
	 */
	bucket_interval = read_relopt_int(vec_oid, "timeseries.bucket_interval", 3600);
	source_name = read_relopt_text(vec_oid, "timeseries.source");
	vector_len = read_relopt_int(vec_oid, "timeseries.vector_len", 384);
	vector_column = read_relopt_text(vec_oid, "timeseries.vector_column");
	carry_str = read_relopt_text(vec_oid, "timeseries.carry_columns");
	value_column = read_relopt_text(vec_oid, "timeseries.value_column");
	vectorize_fn = read_relopt_text(vec_oid, "timeseries.vectorize_function");
	time_column = read_relopt_text(vec_oid, "timeseries.time_column");

	if (vectorize_fn == NULL || *vectorize_fn == '\0')
		vectorize_fn = "ts2v_moment";

	if (time_column == NULL || *time_column == '\0')
		time_column = "time";

	if (source_name == NULL || *source_name == '\0')
		return 0;

	carry_cols = parse_comma_list(carry_str);

	query = build_vector_run_query(vec_oid, bucket_interval, vector_len,
								   source_name, vector_column,
								   vectorize_fn,
								   carry_cols, value_column, time_column);

	/* Now connect SPI for the INSERT ... SELECT execution */
	SPI_connect();
	spi_connected = true;

	PG_TRY();
	{
		ret = SPI_execute(query->data, false, 0);
		nrows = (ret >= 0) ? SPI_processed : 0;
	}
	PG_CATCH();
	{
		FlushErrorState();
		if (spi_connected)
			SPI_finish();
		pfree(query->data);
		pfree(query);
		list_free_deep(carry_cols);
		return 0;
	}
	PG_END_TRY();

	SPI_finish();

	pfree(query->data);
	pfree(query);
	list_free_deep(carry_cols);

	return nrows;
}

/* ====================================================================
 * timeseries_vector_run — SQL-callable manual trigger
 * ==================================================================== */

PG_FUNCTION_INFO_V1(timeseries_vector_run);

Datum
timeseries_vector_run(PG_FUNCTION_ARGS)
{
	Oid			vec_oid = PG_GETARG_OID(0);
	int			nrows;

	/* Validate the table exists and has a timeseries.source relopt */
	{
		char	   *source_name;
		bool		has_source;

		SPI_connect();
		source_name = read_relopt_text(vec_oid, "timeseries.source");
		has_source = (source_name != NULL && *source_name != '\0');
		SPI_finish();

		if (!has_source)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("vector table %s missing 'source' relopt",
							get_rel_name(vec_oid))));
	}

	nrows = run_vector_table(vec_oid);

	PG_RETURN_INT32(nrows);
}

/* ====================================================================
 * Module init: register BGWs and GUCs
 * ==================================================================== */

static void
tsvector_worker_register(int slot)
{
	BackgroundWorker worker;

	memset(&worker, 0, sizeof(worker));

	snprintf(worker.bgw_name, BGW_MAXLEN, "tsvector_worker_%d", slot);
	strcpy(worker.bgw_type, "timeseries_vector");
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	worker.bgw_main_arg = Int32GetDatum(slot);
	worker.bgw_notify_pid = 0;
	strcpy(worker.bgw_library_name, "tsvector_funcs");
	strcpy(worker.bgw_function_name, "tsvector_worker_main");

	RegisterBackgroundWorker(&worker);
}

void
_PG_init(void)
{
	int			i;

	tsvector_define_gucs();

	for (i = 0; i < tsvector_workers; i++)
		tsvector_worker_register(i);
}
