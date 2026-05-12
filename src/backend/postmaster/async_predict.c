/*-------------------------------------------------------------------------
 *
 * async_predict.c
 *	  Asynchronous prediction worker for PREDICT columns
 *
 * This module implements a background worker that scans tables with
 * PREDICT columns and processes rows where both the PREDICT column
 * and the _predict column are NULL.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/async_predict.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <signal.h>
#include <unistd.h>

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/async_predict.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/shmem.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timeout.h"
#include "utils/predict.h"
#include "executor/executor.h"
#include "nodes/execnodes.h"

#define MAX_DBNAME_LEN NAMEDATALEN

/*
 * GUC parameters
 */
int			async_predict_workers = 2;
int			async_predict_naptime = 60;
int			async_predict_batch_size = 100;
bool		async_predict_enabled = true;

/*
 * Shared memory state for async predict workers
 */
typedef struct AsyncPredictWorkerInfo
{
	Oid			dboid;
	char		dbname[MAX_DBNAME_LEN];
	Oid			relid;
	pid_t		pid;
	bool		in_use;
	bool		processing;
	TimestampTz last_scan;
	int64		processed_count;
	int64		error_count;
} AsyncPredictWorkerInfo;

typedef struct AsyncPredictShmemStruct
{
	int			num_workers;
	int			naptime;
	int			batch_size;
	bool		enabled;
	Oid			next_dboid;
	AsyncPredictWorkerInfo workers[FLEXIBLE_ARRAY_MEMBER];
} AsyncPredictShmemStruct;

static AsyncPredictShmemStruct *AsyncPredictShmem = NULL;

typedef struct PredictColumnInfo
{
	Oid			reloid;
	AttrNumber	attnum;
	AttrNumber	predict_attnum;
	Oid			funcoid;
} PredictColumnInfo;

/* Forward declarations */
static void async_predict_process_database(Oid dboid, int worker_slot);
static void async_predict_launcher_sighup(SIGNAL_ARGS);
static Oid async_predict_get_next_database(char *dbname_out);

/*
 * Calculate required shared memory size
 */
Size
AsyncPredictShmemSize(void)
{
	Size		size;

	size = offsetof(AsyncPredictShmemStruct, workers);
	size = add_size(size, mul_size(async_predict_workers,
								   sizeof(AsyncPredictWorkerInfo)));

	return size;
}

/*
 * Initialize shared memory for async predict workers
 */
void
AsyncPredictShmemInit(void)
{
	bool		found;
	int			i;

	AsyncPredictShmem = ShmemInitStruct("async_predict_shmem",
										AsyncPredictShmemSize(),
										&found);

	if (!found)
	{
		AsyncPredictShmem->num_workers = async_predict_workers;
		AsyncPredictShmem->naptime = async_predict_naptime;
		AsyncPredictShmem->batch_size = async_predict_batch_size;
		AsyncPredictShmem->enabled = async_predict_enabled;
		AsyncPredictShmem->next_dboid = InvalidOid;

		for (i = 0; i < async_predict_workers; i++)
		{
			AsyncPredictWorkerInfo *worker = &AsyncPredictShmem->workers[i];

			worker->dboid = InvalidOid;
			worker->dbname[0] = '\0';
			worker->relid = InvalidOid;
			worker->pid = 0;
			worker->in_use = false;
			worker->processing = false;
			worker->last_scan = 0;
			worker->processed_count = 0;
			worker->error_count = 0;
		}
	}
}

/*
 * Register async predict background workers
 */
void
async_predict_register(void)
{
	BackgroundWorker worker;

	if (!async_predict_enabled)
	{
		elog(LOG, "async_predict: disabled, not registering workers");
		return;
	}

	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	strcpy(worker.bgw_library_name, "postgres");
	strcpy(worker.bgw_function_name, "async_predict_launcher_main");
	strcpy(worker.bgw_name, "async predict launcher");
	strcpy(worker.bgw_type, "async_predict");
	worker.bgw_notify_pid = 0;
	worker.bgw_main_arg = (Datum) 0;

	RegisterBackgroundWorker(&worker);
	elog(LOG, "async_predict: registered launcher worker");
}

/*
 * Signal handler for SIGHUP
 */
static void
async_predict_launcher_sighup(SIGNAL_ARGS)
{
	int			save_errno = errno;

	ConfigReloadPending = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

/*
 * Get the next database to process
 * Returns InvalidOid if no database needs processing
 */
static Oid
async_predict_get_next_database(char *dbname_out)
{
	Relation	pg_database;
	SysScanDesc scan;
	HeapTuple	tuple;
	Oid			result = InvalidOid;
	bool		found_current = false;
	Oid			first_dboid = InvalidOid;
	char		first_dbname[MAX_DBNAME_LEN] = {0};

	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());

	pg_database = table_open(DatabaseRelationId, AccessShareLock);
	scan = systable_beginscan(pg_database, InvalidOid, false, NULL, 0, NULL);

	while ((tuple = systable_getnext(scan)) != NULL)
	{
		Form_pg_database dbform = (Form_pg_database) GETSTRUCT(tuple);
		Oid			dboid = dbform->oid;

		if (dbform->datistemplate || !dbform->datallowconn)
			continue;

		if (!OidIsValid(first_dboid))
		{
			first_dboid = dboid;
			strlcpy(first_dbname, NameStr(dbform->datname), MAX_DBNAME_LEN);
		}

		if (!OidIsValid(AsyncPredictShmem->next_dboid))
		{
			result = dboid;
			if (dbname_out)
				strlcpy(dbname_out, NameStr(dbform->datname), MAX_DBNAME_LEN);
			break;
		}

		if (!found_current)
		{
			if (dboid == AsyncPredictShmem->next_dboid)
				found_current = true;
			continue;
		}

		result = dboid;
		if (dbname_out)
			strlcpy(dbname_out, NameStr(dbform->datname), MAX_DBNAME_LEN);
		break;
	}

	systable_endscan(scan);
	table_close(pg_database, AccessShareLock);

	PopActiveSnapshot();
	CommitTransactionCommand();

	if (!OidIsValid(result) && OidIsValid(first_dboid))
	{
		result = first_dboid;
		if (dbname_out)
			strlcpy(dbname_out, first_dbname, MAX_DBNAME_LEN);
	}

	return result;
}

/*
 * Launcher main function
 */
void
async_predict_launcher_main(Datum main_arg)
{
	int			worker_slot;
	BackgroundWorker worker;
	BackgroundWorkerHandle *handle;

	pqsignal(SIGHUP, async_predict_launcher_sighup);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	BackgroundWorkerInitializeConnection(NULL, NULL, 0);

	elog(DEBUG1, "async_predict: launcher started, workers=%d, naptime=%d, batch_size=%d, enabled=%s",
		 async_predict_workers, async_predict_naptime, async_predict_batch_size,
		 async_predict_enabled ? "true" : "false");

	while (true)
	{
		CHECK_FOR_INTERRUPTS();

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		WaitLatch(MyLatch,
				  WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
				  async_predict_naptime * 1000L,
				  WAIT_EVENT_BGWORKER_SHUTDOWN);
		ResetLatch(MyLatch);

		if (!async_predict_enabled)
			continue;

		for (worker_slot = 0; worker_slot < async_predict_workers; worker_slot++)
		{
			AsyncPredictWorkerInfo *wi = &AsyncPredictShmem->workers[worker_slot];
			Oid			dboid;
			char		dbname[MAX_DBNAME_LEN];

			LWLockAcquire(AsyncPredictLock, LW_EXCLUSIVE);
			if (!wi->in_use)
			{
				dboid = async_predict_get_next_database(dbname);
				if (OidIsValid(dboid))
				{
					memset(&worker, 0, sizeof(worker));
					worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
						BGWORKER_BACKEND_DATABASE_CONNECTION;
					worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
					worker.bgw_restart_time = BGW_NEVER_RESTART;
					strcpy(worker.bgw_library_name, "postgres");
					strcpy(worker.bgw_function_name, "async_predict_worker_main");
					snprintf(worker.bgw_name, BGW_MAXLEN,
							 "async predict worker %d", worker_slot);
					strcpy(worker.bgw_type, "async_predict_worker");
					worker.bgw_main_arg = Int32GetDatum(worker_slot);
					worker.bgw_notify_pid = 0;

					if (RegisterDynamicBackgroundWorker(&worker, &handle))
					{
						wi->in_use = true;
						wi->pid = 0;
						wi->dboid = dboid;
						strlcpy(wi->dbname, dbname, MAX_DBNAME_LEN);
						AsyncPredictShmem->next_dboid = dboid;
						elog(DEBUG1, "async_predict: started worker %d for database '%s' (oid=%u)",
							 worker_slot, dbname, dboid);
					}
					else
					{
						elog(LOG, "async_predict: failed to start worker %d", worker_slot);
					}
				}
			}
			LWLockRelease(AsyncPredictLock);
		}
	}
}

/*
 * Process a single table for async prediction
 */
static void
async_predict_process_table(Relation rel, AttrNumber predict_attnum,
							AttrNumber result_attnum, Oid funcoid,
							int worker_slot)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	TableScanDesc scan;
	HeapTuple	tuple;
	int			processed = 0;
	int			skipped_not_null = 0;
	int			total_rows = 0;
	MemoryContext cbcontext;
	MemoryContext oldcontext;
	char	   *relname = RelationGetRelationName(rel);
	Oid			reloid = RelationGetRelid(rel);
	AsyncPredictWorkerInfo *wi = &AsyncPredictShmem->workers[worker_slot];
	int			ret;
	StringInfoData query;
	ExprState  *predict_exprstate = NULL;
	EState	   *predict_estate = NULL;
	bool		use_inline_expr = false;

	cbcontext = AllocSetContextCreate(CurrentMemoryContext,
									  "async_predict callback",
									  ALLOCSET_DEFAULT_SIZES);

	if (!OidIsValid(funcoid))
	{
		Form_pg_attribute predict_attr = TupleDescAttr(tupdesc, predict_attnum - 1);

		if (predict_attr->attgenerated == ATTRIBUTE_GENERATED_PREDICT)
		{
			Expr	   *expr;

			expr = (Expr *) build_column_default(rel, predict_attnum);
			if (expr != NULL)
			{
				predict_estate = CreateExecutorState();
				predict_exprstate = ExecPrepareExpr(expr, predict_estate);
				use_inline_expr = true;
			}
			else
			{
				elog(DEBUG1, "async_predict: no inline expression for column %d of table %s",
					 predict_attnum, relname);
				return;
			}
		}
		else
		{
			elog(DEBUG1, "async_predict: no predict function for table %s", relname);
			return;
		}
	}

	SPI_connect();

	scan = table_beginscan(rel, GetActiveSnapshot(), 0, NULL);

	oldcontext = MemoryContextSwitchTo(cbcontext);

	while (processed < async_predict_batch_size)
	{
		tuple = heap_getnext(scan, ForwardScanDirection);

		if (tuple == NULL)
			break;

		{
			bool		predict_isnull;

			total_rows++;

			predict_isnull = heap_attisnull(tuple, predict_attnum, tupdesc);

			if (!predict_isnull)
			{
				skipped_not_null++;
				continue;
			}

			{
				Datum		predict_datum;
				bool		predict_datum_isnull = false;
				Oid			typoutput;
				bool		typIsVarlena;
				char	   *predict_str;
				char	   *result_colname;
				char	   *predict_colname;
				char	   *rel_quoted;

				if (use_inline_expr)
				{
					TupleTableSlot *slot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsHeapTuple);
					ExprContext *econtext;
					Datum		val;

					ExecStoreHeapTuple(tuple, slot, false);
					econtext = GetPerTupleExprContext(predict_estate);
					econtext->ecxt_scantuple = slot;

					val = ExecEvalExpr(predict_exprstate, econtext, &predict_datum_isnull);

					if (!predict_datum_isnull)
					{
						Form_pg_attribute predict_attr = TupleDescAttr(tupdesc, predict_attnum - 1);
						predict_datum = datumCopy(val, predict_attr->attbyval, predict_attr->attlen);
					}

					ExecDropSingleTupleTableSlot(slot);
				}
				else
				{
					Datum		row_datum;

					row_datum = heap_copy_tuple_as_datum(tuple, tupdesc);
					predict_datum = OidFunctionCall1(funcoid, row_datum);
				}

				if (predict_datum_isnull)
				{
					MemoryContextReset(cbcontext);
					continue;
				}

				{
					Form_pg_attribute predict_attr = TupleDescAttr(tupdesc, predict_attnum - 1);
					getTypeOutputInfo(predict_attr->atttypid, &typoutput, &typIsVarlena);
				}
				predict_str = OidOutputFunctionCall(typoutput, predict_datum);

				result_colname = get_attname(reloid, result_attnum, false);
				predict_colname = get_attname(reloid, predict_attnum, false);

				rel_quoted = quote_identifier(relname);

				initStringInfo(&query);
				appendStringInfo(&query,
								 "UPDATE %s SET %s = %s, %s = %s WHERE ctid = '(%u,%u)'",
								 rel_quoted,
								 quote_identifier(result_colname),
								 predict_str,
								 quote_identifier(predict_colname),
								 predict_str,
								 ItemPointerGetBlockNumber(&tuple->t_self),
								 ItemPointerGetOffsetNumber(&tuple->t_self));

				pfree(result_colname);
				pfree(predict_colname);

				ret = SPI_execute(query.data, false, 0);

				pfree(query.data);

				if (ret == SPI_OK_UPDATE)
					processed++;
				else
					elog(LOG, "async_predict: SPI_execute failed for table %s (result=%d)", relname, ret);

				CommandCounterIncrement();
			}

			MemoryContextReset(cbcontext);
		}
	}

	table_endscan(scan);

	MemoryContextSwitchTo(oldcontext);
	MemoryContextDelete(cbcontext);

	SPI_finish();

	if (predict_estate != NULL)
		FreeExecutorState(predict_estate);

	LWLockAcquire(AsyncPredictLock, LW_EXCLUSIVE);
	wi->processing = false;
	wi->relid = InvalidOid;
	wi->processed_count += processed;
	LWLockRelease(AsyncPredictLock);

	elog(DEBUG1, "async_predict: finished table %s: total_rows=%d, processed=%d, skipped_not_null=%d",
		 relname, total_rows, processed, skipped_not_null);
}

/*
 * Process a single database for async prediction
 */
static void
async_predict_process_database(Oid dboid, int worker_slot)
{
	Relation	pg_class;
	Relation	pg_attribute;
	SysScanDesc class_scan;
	SysScanDesc attr_scan;
	HeapTuple	class_tuple;
	HeapTuple	attr_tuple;
	ScanKeyData key;
	AsyncPredictWorkerInfo *wi = &AsyncPredictShmem->workers[worker_slot];
	int			table_count = 0;
	int			predict_col_count = 0;

	pg_class = table_open(RelationRelationId, AccessShareLock);

	ScanKeyInit(&key,
				Anum_pg_class_relkind,
				BTEqualStrategyNumber, F_CHAREQ,
				CharGetDatum(RELKIND_RELATION));
	class_scan = systable_beginscan(pg_class, InvalidOid, false, NULL, 1, &key);

	while ((class_tuple = systable_getnext(class_scan)) != NULL)
	{
		Form_pg_class class_form = (Form_pg_class) GETSTRUCT(class_tuple);
		Oid			reloid = class_form->oid;
		char	   *relname = NameStr(class_form->relname);

		CHECK_FOR_INTERRUPTS();

		table_count++;

		pg_attribute = table_open(AttributeRelationId, AccessShareLock);
		ScanKeyInit(&key,
					Anum_pg_attribute_attrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(reloid));
		attr_scan = systable_beginscan(pg_attribute, InvalidOid, false, NULL, 1, &key);

		while ((attr_tuple = systable_getnext(attr_scan)) != NULL)
		{
			Form_pg_attribute attr_form = (Form_pg_attribute) GETSTRUCT(attr_tuple);
			AttrNumber	predict_attnum;
			Oid			funcoid;
			Relation	rel;
			char	   *predict_colname;
			char	   *attrname;

			if (attr_form->attisdropped)
				continue;

			if (!attr_form->attpredict)
				continue;

			predict_col_count++;
			attrname = NameStr(attr_form->attname);

			predict_colname = psprintf("%s_predict", attrname);
			predict_attnum = get_attnum(reloid, predict_colname);

			if (predict_attnum == InvalidAttrNumber)
			{
				elog(DEBUG1, "async_predict: _predict column %s not found", predict_colname);
				pfree(predict_colname);
				continue;
			}

			pfree(predict_colname);

			funcoid = InvalidOid;

			if (attr_form->attgenerated == ATTRIBUTE_GENERATED_PREDICT)
			{
				rel = try_relation_open(reloid, RowExclusiveLock);
				if (!rel)
				{
					elog(DEBUG1, "async_predict: could not open table %s", relname);
					continue;
				}

				LWLockAcquire(AsyncPredictLock, LW_EXCLUSIVE);
				wi->relid = reloid;
				wi->processing = true;
				wi->last_scan = GetCurrentTimestamp();
				LWLockRelease(AsyncPredictLock);

				async_predict_process_table(rel, attr_form->attnum, predict_attnum,
										   InvalidOid, worker_slot);

				table_close(rel, RowExclusiveLock);
			}
			else
			{
				funcoid = get_predict_function_oid(reloid, attrname);
				if (!OidIsValid(funcoid))
				{
					elog(DEBUG1, "async_predict: no predict function for table %s", relname);
					continue;
				}

				rel = try_relation_open(reloid, RowExclusiveLock);
				if (!rel)
				{
					elog(DEBUG1, "async_predict: could not open table %s", relname);
					continue;
				}

				LWLockAcquire(AsyncPredictLock, LW_EXCLUSIVE);
				wi->relid = reloid;
				wi->processing = true;
				wi->last_scan = GetCurrentTimestamp();
				LWLockRelease(AsyncPredictLock);

				async_predict_process_table(rel, attr_form->attnum, predict_attnum,
										   funcoid, worker_slot);

				table_close(rel, RowExclusiveLock);
			}
		}

		systable_endscan(attr_scan);
		table_close(pg_attribute, AccessShareLock);
	}

	systable_endscan(class_scan);
	table_close(pg_class, AccessShareLock);

	elog(DEBUG1, "async_predict: worker %d finished database %u: scanned %d tables, found %d PREDICT columns",
		 worker_slot, dboid, table_count, predict_col_count);
}

/*
 * Worker main function - processes one database and exits
 */
void
async_predict_worker_main(Datum main_arg)
{
	int			worker_slot = DatumGetInt32(main_arg);
	AsyncPredictWorkerInfo *wi;
	char		dbname[MAX_DBNAME_LEN];
	Oid			dboid;

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	wi = &AsyncPredictShmem->workers[worker_slot];

	if (!wi->in_use || wi->dbname[0] == '\0')
	{
		elog(WARNING, "async_predict: worker slot %d not properly initialized", worker_slot);
		proc_exit(0);
	}

	wi->pid = MyProcPid;
	dboid = wi->dboid;
	strlcpy(dbname, wi->dbname, MAX_DBNAME_LEN);

	elog(DEBUG1, "async_predict: worker started for database '%s' (oid=%u)", dbname, dboid);

	BackgroundWorkerInitializeConnection(dbname, NULL, 0);

	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());

	async_predict_process_database(dboid, worker_slot);

	PopActiveSnapshot();
	CommitTransactionCommand();
	pgstat_report_stat(false);

	LWLockAcquire(AsyncPredictLock, LW_EXCLUSIVE);
	wi->in_use = false;
	wi->dboid = InvalidOid;
	wi->dbname[0] = '\0';
	wi->pid = 0;
	LWLockRelease(AsyncPredictLock);

	elog(DEBUG1, "async_predict: worker finished processing database '%s'", dbname);

	proc_exit(0);
}
