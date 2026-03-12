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

extern PGDLLIMPORT Oid get_predict_function_oid(Oid relid);

#endif							/* PREDICT_H */
