#ifndef TSVECTORCMDS_H
#define TSVECTORCMDS_H

#include "fmgr.h"
#include "nodes/parsenodes.h"

/*
 * InjectTimeseriesColumns
 *
 * Detects the presence of "timeseries.source" in a CREATE TABLE statement's
 * WITH clause.  When present, injects all system-required columns
 * (slice_start, slice_end, carry columns, the vector column and the quality
 * metadata columns) plus a PRIMARY KEY (slice_start, carry columns) constraint
 * into stmt->tableElts.  Columns already declared by the user are skipped.
 *
 * Returns a List of statements (the HNSW vector index) that must be executed
 * after DefineRelation() has created the table.  Returns NIL when the
 * statement is not a timeseries vector table.
 */
extern List *InjectTimeseriesColumns(CreateStmt *stmt);

/*
 * timeseries_strip_reloptions
 *
 * Remove all "timeseries.*" entries from a reloptions text[] Datum, returning
 * the remaining array (or (Datum)0 when empty).  Used to keep the standard
 * heap_reloptions() validation path from rejecting the custom namespace.
 */
extern Datum timeseries_strip_reloptions(Datum options);

/*
 * timeseries_merge_reloptions
 *
 * Combine a clean (non-"timeseries.*") reloptions array "base" with the
 * "timeseries.*" entries found in "old_options" (which may be (Datum)0), and
 * with any new "timeseries.*" options in defList.  Entries whose name appears
 * in defList are replaced (SET) or removed (RESET).  Returns the merged text[]
 * Datum (or (Datum)0 when empty).
 */
extern Datum timeseries_merge_reloptions(Datum base, Datum old_options,
										  List *defList, bool isReset);

#endif