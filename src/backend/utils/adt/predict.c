/*-------------------------------------------------------------------------
 *
 * predict.c
 *    Functions to support PREDICT keyword
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    src/backend/utils/adt/predict.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "access/relation.h"
#include "access/htup_details.h"
#include "commands/trigger.h"
#include "parser/parse_type.h"
#include "parser/parse_relation.h"
#include "executor/spi.h"

/*
 * is_predict_column - check if a column has PREDICT attribute
 *
 * This function is used internally to determine if a column
 * should be treated as a PREDICT column.
 */
PG_FUNCTION_INFO_V1(is_predict_column);

Datum
is_predict_column(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	text	   *colname = PG_GETARG_TEXT_P(1);
	Relation	rel;
	TupleDesc	tupdesc;
	int			attnum;
	bool		is_predict;

	/* Open relation */
	rel = relation_open(relid, AccessShareLock);

	/* Get tuple descriptor */
	tupdesc = RelationGetDescr(rel);

	/* Find attribute number */
	attnum = attnameAttNum(tupdesc, text_to_cstring(colname), false);

	if (attnum == InvalidAttrNumber)
	{
		relation_close(rel, AccessShareLock);
		PG_RETURN_BOOL(false);
	}

	/* Check if column has PREDICT attribute */
	is_predict = TupleDescAttr(tupdesc, attnum - 1)->attpredict;

	relation_close(rel, AccessShareLock);

	PG_RETURN_BOOL(is_predict);
}

/*
 * predict_trigger_internal - trigger function to copy first array element to second
 *
 * This is a BEFORE INSERT trigger that copies the first element of a PREDICT
 * column's array to the second element position.
 */
static Datum
predict_trigger_internal(PG_FUNCTION_ARGS, const char *trigname)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	Trigger    *trigger;
	TupleDesc	tupdesc;
	HeapTuple	rettuple;
	HeapTuple	newtuple;
	Relation	rel;
	int			attnum;
	Oid			typeid;
	Oid			arraytypeid;
	Datum		arraydatum;
	Datum		newarraydatum;
	Datum		firstelem;
	bool		isnull;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	int16		arrlen;
	int			lowerIndex[1] = {1};
	int			upperIndex[1] = {2};
	bool		replisnull;

	/* Make sure this is being called as a trigger */
	if (!CALLED_AS_TRIGGER(fcinfo))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"%s\" was not called by trigger manager", trigname)));

	/* Check that this is a BEFORE INSERT trigger */
	if (!TRIGGER_FIRED_BEFORE(trigdata->tg_event) ||
		!TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"%s\" must be called as BEFORE INSERT trigger", trigname)));

	/* Check for the NEW tuple */
	if (!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"%s\" must be a ROW-level trigger", trigname)));

	rel = trigdata->tg_relation;
	tupdesc = RelationGetDescr(rel);

	/* Get the NEW tuple */
	newtuple = trigdata->tg_trigtuple;

	/* Find the first PREDICT column in the tuple descriptor */
	for (attnum = 1; attnum <= tupdesc->natts; attnum++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

		/* Skip dropped columns */
		if (attr->attisdropped)
			continue;

		/* Check if this is a PREDICT column */
		if (attr->attpredict)
		{
			/* Get column info */
			arraytypeid = attr->atttypid;
			typeid = get_base_element_type(arraytypeid);

			/* Verify it's an array type */
			if (!OidIsValid(typeid))
				ereport(ERROR,
						(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
						 errmsg("column \"%s\" is not an array type for trigger \"%s\"",
								NameStr(TupleDescAttr(tupdesc, attnum - 1)->attname),
								trigname)));

			/* Get type info */
			get_typlenbyvalalign(typeid, &typlen, &typbyval, &typalign);
			get_typlenbyvalalign(arraytypeid, &arrlen, &typbyval, &typalign);

			/* Get the array value from the new tuple */
			arraydatum = heap_getattr(newtuple, attnum, tupdesc, &isnull);

			if (isnull)
			{
				/* If array is NULL, we don't need to do anything */
				return PointerGetDatum(newtuple);
			}

			/* Get the first element (index 1) */
			firstelem = array_get_element(arraydatum, 1, lowerIndex, arrlen,
										 typlen, typbyval, typalign, &isnull);

			if (isnull)
			{
				/* If first element is NULL, we don't need to do anything */
				return PointerGetDatum(newtuple);
			}

			/* Set the second element (index 2) to the same value as first element */
			newarraydatum = array_set_element(arraydatum, 1, upperIndex,
											 firstelem, false,
											 arrlen, typlen, typbyval, typalign);

			/* Modify the tuple with the updated array value */
			replisnull = false;
			rettuple = heap_modify_tuple_by_cols(newtuple, tupdesc,
												 1, &attnum, &newarraydatum,
												 &replisnull);

			return PointerGetDatum(rettuple);
		}
	}

	/* No PREDICT column found, return the tuple unchanged */
	return PointerGetDatum(newtuple);
}

/*
 * predict_trigger - trigger function to copy first array element to second
 *
 * This is a BEFORE INSERT trigger that copies the first element of a PREDICT
 * column's array to the second element position.
 */
PG_FUNCTION_INFO_V1(predict_trigger);

Datum
predict_trigger(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	TupleDesc	tupdesc;
	HeapTuple	rettuple;
	HeapTuple	newtuple;
	Relation	rel;
	int			attnum;
	Oid			typeid;
	Oid			arraytypeid;
	Datum		arraydatum;
	Datum		newarraydatum;
	Datum		firstelem;
	bool		isnull;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	int16		arrlen;
	int			lowerIndex[1] = {1};
	int			upperIndex[1] = {2};
	bool		replisnull;

	/* Make sure this is being called as a trigger */
	if (!CALLED_AS_TRIGGER(fcinfo))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"predict_trigger\" was not called by trigger manager")));

	/* Check that this is a BEFORE INSERT trigger */
	if (!TRIGGER_FIRED_BEFORE(trigdata->tg_event) ||
		!TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"predict_trigger\" must be called as BEFORE INSERT trigger")));

	/* Check for the NEW tuple */
	if (!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"predict_trigger\" must be a ROW-level trigger")));

	rel = trigdata->tg_relation;
	tupdesc = RelationGetDescr(rel);

	/* Get the NEW tuple */
	newtuple = trigdata->tg_trigtuple;

	/* Find the first PREDICT column in the tuple descriptor */
	for (attnum = 1; attnum <= tupdesc->natts; attnum++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

		/* Skip dropped columns */
		if (attr->attisdropped)
			continue;

		/* Check if this is a PREDICT column */
		if (attr->attpredict)
		{
			/* Get column info */
			arraytypeid = attr->atttypid;
			typeid = get_base_element_type(arraytypeid);

			/* Verify it's an array type */
			if (!OidIsValid(typeid))
				ereport(ERROR,
						(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
						 errmsg("column \"%s\" is not an array type for trigger \"predict_trigger\"",
								NameStr(attr->attname))));

			/* Get type info */
			get_typlenbyvalalign(typeid, &typlen, &typbyval, &typalign);
			get_typlenbyvalalign(arraytypeid, &arrlen, &typbyval, &typalign);

			/* Get the array value from the new tuple */
			arraydatum = heap_getattr(newtuple, attnum, tupdesc, &isnull);

			if (isnull)
			{
				/* If the array is NULL, we don't need to do anything */
				return PointerGetDatum(newtuple);
			}

			/* Get the first element (index 1) */
			firstelem = array_get_element(arraydatum, 1, lowerIndex, arrlen,
										 typlen, typbyval, typalign, &isnull);

			if (isnull)
			{
				/* If the first element is NULL, we don't need to do anything */
				return PointerGetDatum(newtuple);
			}

			/* Set the second element (index 2) to the same value as first element */
			newarraydatum = array_set_element(arraydatum, 1, upperIndex,
										 firstelem, false,
										 arrlen, typlen, typbyval, typalign);

			/* Modify the tuple with the updated array value */
			replisnull = false;
			rettuple = heap_modify_tuple_by_cols(newtuple, tupdesc,
												 1, &attnum, &newarraydatum,
												 &replisnull);

			return PointerGetDatum(rettuple);
		}
	}

	/* No PREDICT column found, return the tuple unchanged */
	return PointerGetDatum(newtuple);
}
