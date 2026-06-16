/*-------------------------------------------------------------------------
 *
 * predict.h
 *	  Header file for PREDICT column support functions
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/predict.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PREDICT_H
#define PREDICT_H

#include "postgres.h"
#include "executor/tuptable.h"

extern PGDLLIMPORT Oid get_embedding_function_oid(Oid relid, const char *colname);

extern Datum embeddings_trigger(PG_FUNCTION_ARGS);

/* On-demand prediction for async predict columns during SELECT */
extern void ExecPredictOnDemand(struct RelationData *rel, TupleTableSlot *slot);

#endif							/* PREDICT_H */
