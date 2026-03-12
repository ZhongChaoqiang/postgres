/*-------------------------------------------------------------------------
 *
 * async_predict.h
 *	  Asynchronous prediction worker for PREDICT columns
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/postmaster/async_predict.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ASYNC_PREDICT_H
#define ASYNC_PREDICT_H

#include "postgres.h"
#include "datatype/timestamp.h"
#include "utils/guc.h"

/*
 * GUC parameters
 */
extern PGDLLIMPORT int async_predict_workers;
extern PGDLLIMPORT int async_predict_naptime;
extern PGDLLIMPORT int async_predict_batch_size;
extern PGDLLIMPORT bool async_predict_enabled;

/* shared memory stuff */
extern Size AsyncPredictShmemSize(void);
extern void AsyncPredictShmemInit(void);

/* Registration functions */
extern void async_predict_register(void);

/* Background worker entry point */
extern void async_predict_launcher_main(Datum main_arg);
extern void async_predict_worker_main(Datum main_arg);

#endif							/* ASYNC_PREDICT_H */
