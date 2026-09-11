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
static int	tsvector_workers = 1;
static int	tsvector_max_in_flight = 64;
static int	tsvector_trigger_cache_size = 512;

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
 * Helper: read reloptions via SPI (portable, works in BGW too)
 * ==================================================================== */

static char *
read_relopt_raw(Oid relid, const char *name)
{
	StringInfo	si = makeStringInfo();
	char	   *result = NULL;
	int			ret;
	char		escaped_name[256];
	char		qbuf[512];

	/*
	 * Simple approach: build SQL directly using fixed buffers to avoid
	 * any memory-allocation issues that cause segfaults in SPI context.
	 * Pattern: "SELECT opt FROM pg_class, LATERAL unnest(reloptions) AS opt
	 *           WHERE oid = <relid> AND opt LIKE '<name>=%' LIMIT 1"
	 * The '%' is SQL LIKE wildcard. Single % in SQL string, escaped as %% in C printf.
	 */
	snprintf(escaped_name, sizeof(escaped_name), "%s=", name);

	/* Build the SQL LIKE pattern with literal % */
	snprintf(qbuf, sizeof(qbuf), "'%s%%'", escaped_name);

	appendStringInfo(si,
		"SELECT opt FROM pg_class, "
		"LATERAL unnest(reloptions) AS opt "
		"WHERE oid = %u AND opt LIKE %s "
		"LIMIT 1",
		relid, qbuf);

	ret = SPI_execute(si->data, true, 1);
	if (ret == SPI_OK_SELECT && SPI_processed > 0)
	{
		char   *opt = SPI_getvalue(SPI_tuptable->vals[0],
								   SPI_tuptable->tupdesc, 1);

		if (opt)
		{
			const char *eq = strchr(opt, '=');

			if (eq)
				result = pstrdup(eq + 1);
		}
		SPI_freetuptable(SPI_tuptable);
	}
	pfree(si->data);
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
 * Background Worker structures
 * ==================================================================== */

typedef struct TaskKey
{
	Oid			vec_oid;
	TimestampTz slice_start;
	uint32		carry_hash;
} TaskKey;

typedef struct TaskSpec
{
	TaskKey		key;
	char	   *vector_table_name;
	int			delay_seconds;
	bool		recompute;
	int			bucket_interval;
} TaskSpec;

typedef struct WorkerState
{
	HTAB	   *in_flight;
	bool		backpressure_active;
} WorkerState;

static WorkerState worker_state = {0};

/* ====================================================================
 * execute_task: actually run vector computation for one task
 * ==================================================================== */

static void
execute_task(TaskSpec *task)
{
	Oid			vec_oid = task->key.vec_oid;
	int			bucket_interval;
	char	   *source_name;
	int			vector_len;
	StringInfo	query;
	int			ret;

	elog(LOG, "tsvector_worker: executing task vec=%s slice=%lld",
		 task->vector_table_name, (long long) task->key.slice_start);

	SPI_connect();

	bucket_interval = read_relopt_int(vec_oid, "timeseries.bucket_interval", 3600);
	source_name = read_relopt_text(vec_oid, "timeseries.source");
	vector_len = read_relopt_int(vec_oid, "timeseries.vector_len", 384);

	if (source_name == NULL)
	{
		SPI_finish();
		elog(WARNING, "tsvector_worker: no source relopt on %s, skipping",
			 task->vector_table_name);
		return;
	}

	query = makeStringInfo();
	appendStringInfo(query,
		"INSERT INTO %s (slice_start, carry_hash, vector, count) "
		"SELECT %lld::timestamptz, %u, "
		"ts2v_moment(array_agg(val1 ORDER BY time), %d), count(*) "
		"FROM %s WHERE time >= %lld::timestamptz "
		"AND time < (%lld::timestamptz + interval '%d seconds') "
		"ON CONFLICT (slice_start, carry_hash) DO UPDATE SET "
		"vector = EXCLUDED.vector, count = EXCLUDED.count",
		task->vector_table_name,
		(long long) task->key.slice_start, task->key.carry_hash,
		vector_len,
		source_name,
		(long long) task->key.slice_start,
		(long long) task->key.slice_start, bucket_interval);

	ret = SPI_execute(query->data, false, 0);
	if (ret < 0)
		elog(WARNING, "tsvector_worker: SPI_execute failed: %d", ret);

	pfree(query->data);
	SPI_finish();
}

/* ====================================================================
 * BGW main: poll-based approach using a task queue table
 * ==================================================================== */

static void
tsvector_worker_main(Datum main_arg)
{
	MemoryContext worker_ctx;
	MemoryContext old_ctx;

	BackgroundWorkerInitializeConnection("postgres", NULL, 0);

	worker_ctx = AllocSetContextCreate(TopMemoryContext, "tsvector_worker",
									   ALLOCSET_DEFAULT_SIZES);
	old_ctx = MemoryContextSwitchTo(worker_ctx);

	/* in_flight hash */
	{
		HASHCTL		ctl;

		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(TaskKey);
		ctl.entrysize = sizeof(TaskSpec);
		ctl.hcxt = worker_ctx;
		worker_state.in_flight = hash_create("tsvector_in_flight",
											 tsvector_max_in_flight,
											 &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	elog(LOG, "tsvector_worker: started (poll-based)");

	/*
	 * We use a simple approach: poll for pending tasks by checking
	 * pg_stat_activity for our own trigger output, or better yet,
	 * use a dedicated queue table that triggers INSERT into.
	 *
	 * For now, we do a simple poll: every 500ms connect via SPI
	 * and check for tasks that need execution. We discover what to compute
	 * by scanning vector tables for missing slices near "now".
	 *
	 * This is a reasonable hybrid: trigger still sends NOTIFY for low-latency
	 * hints, but the BGW uses polling as the primary discovery mechanism.
	 */

	while (true)
	{
		/* Sleep 500ms */
		WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
				  500L, WAIT_EVENT_PG_SLEEP);
		ResetLatch(MyLatch);

		/* Backpressure: skip if too many in-flight */
		if (hash_get_num_entries(worker_state.in_flight) >= tsvector_max_in_flight)
		{
			if (!worker_state.backpressure_active)
			{
				worker_state.backpressure_active = true;
				elog(LOG, "tsvector_worker: backpressure active");
			}
			continue;
		}

		if (worker_state.backpressure_active &&
			hash_get_num_entries(worker_state.in_flight) <
			tsvector_max_in_flight * 80 / 100)
		{
			worker_state.backpressure_active = false;
			elog(LOG, "tsvector_worker: backpressure relieved");
		}

		/* Try to discover pending tasks */
		SPI_connect();
		{
			TimestampTz now = GetCurrentTimestamp();
			TimestampTz horizon;
			StringInfo	q = makeStringInfo();
			int			ret;

			horizon = now - (10 * 60 * 1000000LL);	/* 10 minutes ago */

			/*
			 * Find source tables that have recent inserts but their vector
			 * tables don't have the corresponding slice yet.
			 */
			appendStringInfo(q,
				"SELECT vec.relname AS vec_table, "
				"       src.relname AS src_table, "
				"       src_stats.last_autovacuum, "
				"       vec_opts.bucket_interval "
				"FROM pg_class src "
				"JOIN pg_stat_user_tables src_stats ON src.oid = src_stats.relid "
				"JOIN pg_class vec ON vec.reloptions IS NOT NULL "
				"JOIN LATERAL unnest(vec.reloptions) AS opt "
				"  ON opt LIKE 'timeseries.source=%%' "
				"WHERE src_stats.last_autovacuum > %lld "
				"LIMIT 32",
				(long long) horizon);

			ret = SPI_execute(q->data, true, 0);
			if (ret == SPI_OK_SELECT && SPI_processed > 0)
			{
				TupleDesc	spi_tupdesc = SPI_tuptable->tupdesc;
				int			t;

				for (t = 0; t < SPI_processed; t++)
				{
					HeapTuple	htup = SPI_tuptable->vals[t];
					char	   *vec_table;
					bool		isnull;
					Oid			vec_oid;
					int			bucket_interval;
					TimestampTz curr_slice;
					TimestampTz prev_slice;
					uint32		carry_hash = 0;
					TaskKey		key;

					vec_table = SPI_getvalue(htup, spi_tupdesc, 1);
					vec_oid = vec_table ? get_relname_relid(vec_table, InvalidOid) : InvalidOid;

					if (!OidIsValid(vec_oid))
						continue;

					/* Read bucket_interval from relopts if not in result */
					bucket_interval = read_relopt_int(vec_oid, "timeseries.bucket_interval", 3600);

					{
						int64		bucket_us = (int64) bucket_interval * 1000000LL;
						TimestampTz row_time = now - bucket_us;

						curr_slice = (row_time / bucket_us) * bucket_us;
						prev_slice = curr_slice - bucket_us;
					}

					/* Try prev slice */
					memset(&key, 0, sizeof(key));
					key.vec_oid = vec_oid;
					key.slice_start = prev_slice;
					key.carry_hash = carry_hash;

					if (hash_search(worker_state.in_flight, &key, HASH_FIND, NULL) != NULL)
						continue;

					{
						StringInfo	si = makeStringInfo();
						int			sret;

						appendStringInfo(si,
							"SELECT 1 FROM %s WHERE slice_start = %lld LIMIT 1",
							vec_table, (long long) prev_slice);

						sret = SPI_execute(si->data, true, 1);
						if (sret == SPI_OK_SELECT && SPI_processed == 0)
						{
							TaskSpec	task;

							memset(&task, 0, sizeof(task));
							task.key = key;
							task.vector_table_name = pstrdup(vec_table);
							task.bucket_interval = bucket_interval;

							hash_search(worker_state.in_flight,
										&key, HASH_ENTER, NULL);

							SPI_finish();
							execute_task(&task);
							SPI_connect();

							hash_search(worker_state.in_flight,
										&key, HASH_REMOVE, NULL);

							pfree(task.vector_table_name);
						}
						pfree(si->data);
					}
				}
				SPI_freetuptable(SPI_tuptable);
			}
			else if (ret == SPI_OK_SELECT)
			{
				/* No results */
				SPI_freetuptable(SPI_tuptable);
			}
			pfree(q->data);
		}
		SPI_finish();
	}

	elog(LOG, "tsvector_worker: shutting down");
	MemoryContextSwitchTo(old_ctx);
	MemoryContextDelete(worker_ctx);
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
 * timeseries_vector_run — manual trigger for one vector table
 * ==================================================================== */

PG_FUNCTION_INFO_V1(timeseries_vector_run);

Datum
timeseries_vector_run(PG_FUNCTION_ARGS)
{
	Oid			vec_oid = PG_GETARG_OID(0);
	int			bucket_interval;
	char	   *source_name;
	int			vector_len;
	StringInfo	query;
	int			ret;

	/* SPI_connect must come BEFORE read_relopt_* which use SPI */
	SPI_connect();

	bucket_interval = read_relopt_int(vec_oid, "timeseries.bucket_interval", 3600);
	source_name = read_relopt_text(vec_oid, "timeseries.source");
	vector_len = read_relopt_int(vec_oid, "timeseries.vector_len", 384);

	if (source_name == NULL)
	{
		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("vector table %s missing 'source' relopt",
						get_rel_name(vec_oid))));
	}

	query = makeStringInfo();
	appendStringInfo(query,
		"INSERT INTO %s (slice_start, carry_hash, vector, count) "
		"SELECT "
		"  (to_timestamp(bucket_us / 1000000.0))::timestamptz, "
		"  0, "
		"  ts2v_moment(array_agg(val1 ORDER BY time), %d), "
		"  count(*) "
		"FROM (SELECT time, val1, "
		"       (EXTRACT(EPOCH FROM time) / %d)::bigint * %d AS bucket_us "
		"FROM %s) sub "
		"GROUP BY bucket_us "
		"ON CONFLICT (slice_start, carry_hash) DO UPDATE SET "
		"  vector = EXCLUDED.vector, count = EXCLUDED.count",
		get_rel_name(vec_oid),
		vector_len,
		bucket_interval, bucket_interval,
		source_name);

	ret = SPI_execute(query->data, false, 0);
	pfree(query->data);

	SPI_finish();

	PG_RETURN_BOOL(ret >= 0);
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
