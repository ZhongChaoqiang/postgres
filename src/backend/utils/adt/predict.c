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
#include "utils/syscache.h"
#include "access/relation.h"
#include "access/htup_details.h"
#include "catalog/pg_class.h"
#include "commands/trigger.h"
#include "parser/parse_type.h"
#include "parser/parse_relation.h"
#include "executor/spi.h"
#include "catalog/pg_proc.h"
#include "utils/guc.h"
#include "catalog/catversion.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_namespace.h"
#include "nodes/makefuncs.h"

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
 * get_predict_function - Get the predict function name from reloptions
 *
 * Returns the function name if set, or NULL if not set.
 */
static char *
get_predict_function(Oid relid)
{
	HeapTuple	tuple;
	Datum		reloptions;
	bool		isnull;
	char		*predict_func = NULL;

	/* Get the relation tuple from pg_class */
	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);

	/* Get reloptions */
	reloptions = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions,
		   &isnull);

	elog(LOG, "get_predict_function: relation %u, isnull = %d", relid, isnull);

	if (!isnull)
	{
		/* reloptions is a text array, so we need to deconstruct it */
		ArrayType  *array = DatumGetArrayTypeP(reloptions);
		Datum		*elems;
		bool		*nulls;
		int		nitems;
		int		i;

		/* Deconstruct the array */
		deconstruct_array(array, TEXTOID, -1, false, 'i', &elems, &nulls, &nitems);

		elog(LOG, "get_predict_function: found %d reloptions", nitems);

		/* Iterate through the array elements */
		for (i = 0; i < nitems; i++)
		{
			bool match;
			const char *expected;
			int j;
			text		*t;
			char		*text_str;
			int		text_len;
			char		*opt_str;
			int		len;
			
			elog(LOG, "get_predict_function: reloption %d, nulls[i] = %d, elems[i] = 0x%p", i, nulls[i], (void *)elems[i]);
			
			/* Check if the element is null */
			if (!nulls[i])
			{
				/* Get the text string from the datum */
				t = DatumGetTextP(elems[i]);
				text_str = VARDATA(t);
				text_len = VARSIZE(t) - VARHDRSZ;

				/* Log the actual reloption string */
				opt_str = palloc(text_len + 1);
				memcpy(opt_str, text_str, text_len);
				opt_str[text_len] = '\0';
				elog(LOG, "get_predict_function: reloption %d: '%s'", i, opt_str);
				
				/* Debug: Check the condition parts */
				elog(LOG, "get_predict_function: text_len = %d, text_len > 17 = %d", text_len, text_len > 17);
				elog(LOG, "get_predict_function: expected prefix = 'predict_function='");
				elog(LOG, "get_predict_function: actual prefix = '%.*s'", 17, opt_str);
				
				/* Debug: Check the first few characters with their ASCII values */
				elog(LOG, "get_predict_function: opt_str[0] = '%c' (%d), expected 'p' (%d)", opt_str[0], (int)opt_str[0], (int)'p');
				elog(LOG, "get_predict_function: opt_str[1] = '%c' (%d), expected 'r' (%d)", opt_str[1], (int)opt_str[1], (int)'r');
				elog(LOG, "get_predict_function: opt_str[2] = '%c' (%d), expected 'e' (%d)", opt_str[2], (int)opt_str[2], (int)'e');
				
				/* Check each character individually to avoid any string comparison issues */
				match = true;
				expected = "predict_function=";
				for (j = 0; j < 17; j++)
				{
					if (opt_str[j] != expected[j])
					{
						match = false;
						elog(LOG, "get_predict_function: mismatch at position %d: '%c' (%d) != '%c' (%d)", j, opt_str[j], (int)opt_str[j], expected[j], (int)expected[j]);
						break;
					}
				}
				elog(LOG, "get_predict_function: prefix match = %d", match);
				
				/* Look for predict_function keyword using individual character comparison */
				if (text_len > 17 && match)
				{
					/* Extract the function name */
					len = text_len - 17;
					predict_func = palloc(len + 1);
					memcpy(predict_func, opt_str + 17, len);
					predict_func[len] = '\0';
					elog(LOG, "get_predict_function: found predict function: '%s'", predict_func);
					break;
				}
				else
				{
					elog(LOG, "get_predict_function: condition not met, text_len=%d, match=%d", text_len, match);
				}
				
				pfree(opt_str);
			}
			else
			{
				elog(LOG, "get_predict_function: reloption %d is null", i);
			}
		}

		/* Free the array elements */
		pfree(elems);
		pfree(nulls);
	}

	ReleaseSysCache(tuple);

	elog(LOG, "get_predict_function: returning '%s'", predict_func ? predict_func : "NULL");
	return predict_func;
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
