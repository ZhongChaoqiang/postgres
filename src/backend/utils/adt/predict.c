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
#include "parser/parse_func.h"
#include "nodes/pg_list.h"
#include "executor/spi.h"
#include "catalog/pg_proc.h"
#include "utils/guc.h"
#include "catalog/catversion.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_namespace.h"
#include "nodes/makefuncs.h"
#include "access/reloptions.h"

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

	if (!isnull)
	{
		/* reloptions is a text array, so we need to deconstruct it */
		ArrayType  *array = DatumGetArrayTypeP(reloptions);
		Datum		*elems;
		bool		*nulls;
		int		nitems;
		int		i;
		const char *prefix = "predict_function=";
		const size_t prefix_len = 17;

		/* Deconstruct the array */
		deconstruct_array(array, TEXTOID, -1, false, 'i', &elems, &nulls, &nitems);

		/* Iterate through the array elements */
		for (i = 0; i < nitems; i++)
		{
			if (!nulls[i])
			{
				text		*t = DatumGetTextP(elems[i]);
				char		*text_str = VARDATA(t);
				int		text_len = VARSIZE(t) - VARHDRSZ;

				/* Check if the option matches the expected prefix */
				if (text_len > prefix_len && strncmp(text_str, prefix, prefix_len) == 0)
				{
					/* Extract the function name directly from text_str */
					predict_func = palloc(text_len - prefix_len + 1);
					memcpy(predict_func, text_str + prefix_len, text_len - prefix_len);
					predict_func[text_len - prefix_len] = '\0';
					break;
				}
			}
		}

		/* Free the array elements */
		pfree(elems);
		pfree(nulls);
	}

	ReleaseSysCache(tuple);
	return predict_func;
}

/*
 * predict_trigger - trigger function for PREDICT columns
 *
 * This is a BEFORE INSERT trigger that handles PREDICT columns:
 * - When predict_timing is deferred: saves user input data directly
 * - When predict_timing is immediate: calls predict_function and saves result
 */
PG_FUNCTION_INFO_V1(predict_trigger);

Datum
predict_trigger(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	TupleDesc		tupdesc;
	HeapTuple		rettuple;
	HeapTuple		newtuple;
	Relation		rel;
	int				attnum;
	Oid				typeid;
	Datum				coldatum;
	Datum				resultdatum;
	bool				isnull;
	bool				replisnull;

	if (!CALLED_AS_TRIGGER(fcinfo))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"predict_trigger\" was not called by trigger manager")));

	if (!TRIGGER_FIRED_BEFORE(trigdata->tg_event) ||
		!TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"predict_trigger\" must be called as BEFORE INSERT trigger")));

	if (!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"predict_trigger\" must be a ROW-level trigger")));

	rel = trigdata->tg_relation;
	tupdesc = RelationGetDescr(rel);
	newtuple = trigdata->tg_trigtuple;

	for (attnum = 1; attnum <= tupdesc->natts; attnum++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

		if (attr->attisdropped)
			continue;

		if (attr->attpredict)
		{
			typeid = attr->atttypid;

			coldatum = heap_getattr(newtuple, attnum, tupdesc, &isnull);
			if (isnull)
				return PointerGetDatum(newtuple);

			StdRdOptions *relopts = (StdRdOptions *) rel->rd_options;
			StdRdOptPredictTiming predict_timing = STDRD_OPTION_PREDICT_TIMING_DEFERRED;
			
			if (relopts != NULL)
				predict_timing = relopts->predict_timing;

			if (predict_timing == STDRD_OPTION_PREDICT_TIMING_DEFERRED)
			{
				resultdatum = coldatum;
			}
			else
			{
				char *predict_func_name = get_predict_function(rel->rd_id);
				
				if (predict_func_name == NULL)
				{
					resultdatum = coldatum;
				}
				else
				{
					Oid predict_func_oid = InvalidOid;
					char *search_func_name;
					char *funcname_only;
					FuncCandidateList clist;
					int fgc_flags;
					
					if (strchr(predict_func_name, '.') == NULL)
						search_func_name = psprintf("public.%s", predict_func_name);
					else
						search_func_name = pstrdup(predict_func_name);
					
					funcname_only = strrchr(search_func_name, '.');
					if (funcname_only != NULL)
						funcname_only++;
					else
						funcname_only = search_func_name;
					
					clist = FuncnameGetCandidates(list_make1(makeString(funcname_only)), 
												 1, NIL, false, false, false, true, &fgc_flags);
					
					if (clist != NULL)
					{
						for (; clist != NULL; clist = clist->next)
						{
							if (clist->nargs == 1 && clist->args[0] == typeid)
							{
								predict_func_oid = clist->oid;
								break;
							}
						}
					}
					
					pfree(search_func_name);
					
					if (OidIsValid(predict_func_oid))
					{
						FmgrInfo predict_func;
						fmgr_info(predict_func_oid, &predict_func);
						resultdatum = FunctionCall1(&predict_func, coldatum);
					}
					else
					{
						resultdatum = coldatum;
					}
					
					pfree(predict_func_name);
				}
			}
			
			replisnull = false;
			rettuple = heap_modify_tuple_by_cols(newtuple, tupdesc, 1, &attnum,
							&resultdatum, &replisnull);
			return PointerGetDatum(rettuple);
		}
	}

	return PointerGetDatum(newtuple);
}
