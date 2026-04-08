/*-------------------------------------------------------------------------
 *
 * sequencesync.c
 *
 * Logical replication sequences sync functionality
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *      src/backend/replication/logical/sequencesync.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "replication/logicalworker.h"

/*
 * SequenceSyncWorkerMain - Main function for sequence sync worker
 *
 * Stub function for compatibility
 */
void
SequenceSyncWorkerMain(Datum main_arg)
{
    elog(ERROR, "SequenceSyncWorkerMain is not implemented in this version");
}
