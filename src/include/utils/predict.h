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

extern PGDLLIMPORT Oid get_embedding_function_oid(Oid relid, const char *colname);

#endif							/* PREDICT_H */
