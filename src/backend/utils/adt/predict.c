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
	TupleDesc		tupdesc;
	HeapTuple		rettuple;
	HeapTuple		newtuple;
	Relation		rel;
	int				attnum;
	Oid				typeid;
	Oid				arraytypeid;
	Datum				arraydatum;
	Datum				newarraydatum;
	Datum				firstelem;
	bool				isnull;
	int16				typlen;
	bool				typbyval;
	char				typalign;
	int16				arrlen;
	int				lowerIndex[1] = {1};
	bool				replisnull;

	/* Validate trigger context */
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

	/* Get trigger data */
	rel = trigdata->tg_relation;
	tupdesc = RelationGetDescr(rel);
	newtuple = trigdata->tg_trigtuple;

	/* Find the first PREDICT column */
	for (attnum = 1; attnum <= tupdesc->natts; attnum++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

		/* Skip dropped columns */
		if (attr->attisdropped)
			continue;

		/* Check if this is a PREDICT column */
		if (attr->attpredict)
		{
			/* Validate column is an array type */
			arraytypeid = attr->atttypid;
			typeid = get_base_element_type(arraytypeid);
			if (!OidIsValid(typeid))
				ereport(ERROR,
						(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
						 errmsg("column \"%s\" is not an array type for trigger \"predict_trigger\"",
								NameStr(attr->attname))));

			/* Get type information */
			get_typlenbyvalalign(typeid, &typlen, &typbyval, &typalign);
			get_typlenbyvalalign(arraytypeid, &arrlen, &typbyval, &typalign);

			/* Get array value from tuple */
			arraydatum = heap_getattr(newtuple, attnum, tupdesc, &isnull);
			if (isnull)
				return PointerGetDatum(newtuple); /* NULL array, no action needed */
			
			/* Get first element */
			firstelem = array_get_element(arraydatum, 1, lowerIndex, arrlen, 
						 typlen, typbyval, typalign, &isnull);
			if (isnull)
				return PointerGetDatum(newtuple); /* NULL first element, no action needed */

			/* Process predict function if available */
			Datum predict_result = firstelem;
			char *predict_func_name = get_predict_function(rel->rd_id);
			
			if (predict_func_name != NULL)
			{
				/* Look up the predict function OID */
				Oid predict_func_oid = InvalidOid;
				char *search_func_name;
				char *funcname_only;
				FuncCandidateList clist;
				int fgc_flags;
				
				/* Construct function name with schema if needed */
				if (strchr(predict_func_name, '.') == NULL)
					search_func_name = psprintf("public.%s", predict_func_name);
				else
					search_func_name = pstrdup(predict_func_name);
				
				/* Extract function name without schema */
				funcname_only = strrchr(search_func_name, '.');
				if (funcname_only != NULL)
					funcname_only++;
				else
					funcname_only = search_func_name;
				
				/* Find function candidates */
				clist = FuncnameGetCandidates(list_make1(makeString(funcname_only)), 
											 1, NIL, false, false, false, true, &fgc_flags);
				
				/* Check for matching function signature */
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
					/* Call the predict function */
					FmgrInfo predict_func;
					fmgr_info(predict_func_oid, &predict_func);
					
					/* Prepare argument based on typbyval */
					Datum predict_arg = typbyval ? firstelem : 
						Int32GetDatum(*((int32 *) DatumGetPointer(firstelem)));
					
					predict_result = FunctionCall1(&predict_func, predict_arg);
				}
				
				pfree(predict_func_name);
			}
			
			/* Create updated array */
			Datum newelems[2];
			
			/* First element stays the same */
			newelems[0] = typbyval ? firstelem : Int32GetDatum(*((int32 *) DatumGetPointer(firstelem)));
			
			/* Second element is the predict function result */
			newelems[1] = predict_result;
			
			/* Construct new array */
			newarraydatum = (typeid == INT4OID) ?
				PointerGetDatum(construct_array_builtin(newelems, 2, typeid)) :
				PointerGetDatum(construct_array(newelems, 2, typeid, typlen, typbyval, typalign));
			
			/* Update tuple and return */
			replisnull = false;
			rettuple = heap_modify_tuple_by_cols(newtuple, tupdesc, 1, &attnum,
							&newarraydatum, &replisnull);
			return PointerGetDatum(rettuple);
		}
	}

	/* No PREDICT column found, return original tuple */
	return PointerGetDatum(newtuple);
}
