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
#include "utils/predict.h"
#include "utils/regproc.h"
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
#include "catalog/namespace.h"
#include "nodes/makefuncs.h"
#include "access/reloptions.h"
#include "funcapi.h"

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
	attnum = attnameAttNum(rel, text_to_cstring(colname), false);

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
 * Supports two formats:
 *   1. Single function name: "func_name" - applies to all PREDICT columns
 *   2. Column-specific: "col_a:func_a;col_b:func_b" - applies func_a to col_a, func_b to col_b
 *
 * If colname is provided (non-NULL), looks up the function for that specific column.
 * If colname is NULL, returns the single function name (format 1) or NULL (format 2).
 */
static char *
get_predict_function(Oid relid, const char *colname)
{
	HeapTuple	tuple;
	Datum		reloptions;
	bool		isnull;
	char	   *predict_func = NULL;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);

	reloptions = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions,
		   &isnull);

	elog(LOG, "predict_function: get_predict_function for relid=%u, colname=%s, reloptions isnull=%s",
		 relid, colname ? colname : "(null)", isnull ? "true" : "false");

	if (!isnull)
	{
		ArrayType  *array = DatumGetArrayTypeP(reloptions);
		Datum	   *elems;
		bool	   *nulls;
		int			nitems;
		int			i;
		const char *prefix = "predict_function=";
		const size_t prefix_len = strlen(prefix);

		deconstruct_array(array, TEXTOID, -1, false, 'i', &elems, &nulls, &nitems);

		elog(LOG, "predict_function: reloptions has %d items", nitems);

		for (i = 0; i < nitems; i++)
		{
			if (!nulls[i])
			{
				char	   *text_str = TextDatumGetCString(elems[i]);
				int			text_len = strlen(text_str);

				elog(LOG, "predict_function: reloption[%d] = '%s'", i, text_str);

				if (text_len > prefix_len && strncmp(text_str, prefix, prefix_len) == 0)
				{
					char	   *value = text_str + prefix_len;

					if (colname == NULL)
					{
						if (strchr(value, ':') == NULL)
							predict_func = pstrdup(value);
					}
					else
					{
						if (strchr(value, ':') == NULL)
						{
							predict_func = pstrdup(value);
						}
						else
						{
							char	   *copy = pstrdup(value);
							char	   *token;
							char	   *saveptr;

							token = strtok_r(copy, ";", &saveptr);
							while (token != NULL)
							{
								char	   *colon = strchr(token, ':');

								if (colon != NULL)
								{
									char	   *key = token;
									*colon = '\0';
									char	   *func = colon + 1;

									if (strcmp(key, colname) == 0)
									{
										predict_func = pstrdup(func);
										break;
									}
								}
								token = strtok_r(NULL, ";", &saveptr);
							}
							pfree(copy);
						}
					}
					pfree(text_str);
					break;
				}
				pfree(text_str);
			}
		}

		pfree(elems);
		pfree(nulls);
	}

	ReleaseSysCache(tuple);
	
	if (predict_func == NULL)
		elog(LOG, "predict_function: no predict_function found for relid=%u, colname=%s", 
			 relid, colname ? colname : "(null)");
	else
		elog(LOG, "predict_function: found predict_function = '%s' for colname=%s", 
			 predict_func, colname ? colname : "(all)");
	
	return predict_func;
}

/*
 * get_predict_function_oid - Get the OID of the predict function
 *
 * Returns the function OID if set, or InvalidOid if not set.
 * colname specifies which PREDICT column to look up the function for.
 */
Oid
get_predict_function_oid(Oid relid, const char *colname)
{
	char	   *funcname;
	Oid			funcoid = InvalidOid;
	List	   *namelist;
	FuncCandidateList clist;
	int			fgc_flags;

	funcname = get_predict_function(relid, colname);
	if (funcname == NULL)
	{
		elog(LOG, "predict_function: no predict_function set for relid=%u, colname=%s", 
			 relid, colname ? colname : "(null)");
		return InvalidOid;
	}

	elog(LOG, "predict_function: looking for function '%s' for column '%s'", 
		 funcname, colname ? colname : "(all)");

	if (strchr(funcname, '.') != NULL)
	{
		namelist = stringToQualifiedNameList(funcname, NULL);
	}
	else
	{
		namelist = list_make1(makeString(funcname));
	}

	clist = FuncnameGetCandidates(namelist, 1, NIL, false, false, false, true, &fgc_flags);

	elog(LOG, "predict_function: FuncnameGetCandidates result, clist=%p", clist);

	if (clist != NULL)
	{
		for (; clist != NULL; clist = clist->next)
		{
			elog(LOG, "predict_function: candidate oid=%u, nargs=%d", clist->oid, clist->nargs);
			if (clist->nargs == 1)
			{
				funcoid = clist->oid;
				break;
			}
		}
	}

	if (!OidIsValid(funcoid))
	{
		char	   *query;
		Datum		result;
		bool		isnull;
		MemoryContext oldcontext;
		MemoryContext querycontext;
		int			ret;

		elog(LOG, "predict_function: trying direct lookup via pg_proc");

		querycontext = AllocSetContextCreate(CurrentMemoryContext,
											 "predict_func_lookup",
											 ALLOCSET_DEFAULT_SIZES);
		oldcontext = MemoryContextSwitchTo(querycontext);

		query = psprintf(
			"SELECT oid FROM pg_proc "
			"WHERE proname = '%s' "
			"AND pronargs = 1 "
			"ORDER BY oid LIMIT 1",
			funcname);

		elog(LOG, "predict_function: SPI query: %s", query);

		ret = SPI_connect();
		elog(LOG, "predict_function: SPI_connect returned %d", ret);
		
		if (ret == SPI_OK_CONNECT)
		{
			ret = SPI_execute(query, true, 1);
			elog(LOG, "predict_function: SPI_execute returned %d, SPI_processed=%lu", 
				 ret, SPI_processed);
			
			if (ret == SPI_OK_SELECT && SPI_processed > 0)
			{
				result = SPI_getbinval(SPI_tuptable->vals[0],
									   SPI_tuptable->tupdesc, 1, &isnull);
				if (!isnull)
					funcoid = DatumGetObjectId(result);
				elog(LOG, "predict_function: SPI lookup result=%u", funcoid);
			}
			SPI_finish();
		}

		MemoryContextSwitchTo(oldcontext);
		MemoryContextDelete(querycontext);
	}

	elog(LOG, "predict_function: get_predict_function_oid final result=%u", funcoid);

	pfree(funcname);
	return funcoid;
}

/*
 * get_embedding_function - Get the embedding function name from reloptions
 *
 * Returns the function name if set, or NULL if not set.
 * Supports two formats:
 *   1. Single function name: "func_name" - applies to all EMBEDDING columns
 *   2. Column-specific: "col_a:func_a;col_b:func_b" - applies func_a to col_a, func_b to col_b
 *
 * If colname is provided (non-NULL), looks up the function for that specific column.
 * If colname is NULL, returns the single function name (format 1) or NULL (format 2).
 */
static char *
get_embedding_function(Oid relid, const char *colname)
{
	HeapTuple	tuple;
	Datum		reloptions;
	bool		isnull;
	char	   *embedding_func = NULL;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);

	reloptions = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions,
		   &isnull);

	if (!isnull)
	{
		ArrayType  *array = DatumGetArrayTypeP(reloptions);
		Datum	   *elems;
		bool	   *nulls;
		int			nitems;
		int			i;
		const char *prefix = "embedding_function=";
		const size_t prefix_len = strlen(prefix);

		deconstruct_array(array, TEXTOID, -1, false, 'i', &elems, &nulls, &nitems);

		for (i = 0; i < nitems; i++)
		{
			if (!nulls[i])
			{
				char	   *text_str = TextDatumGetCString(elems[i]);
				int			text_len = strlen(text_str);

				if (text_len > prefix_len && strncmp(text_str, prefix, prefix_len) == 0)
				{
					char	   *value = text_str + prefix_len;

					if (colname == NULL)
					{
						if (strchr(value, ':') == NULL)
							embedding_func = pstrdup(value);
					}
					else
					{
						if (strchr(value, ':') == NULL)
						{
							embedding_func = pstrdup(value);
						}
						else
						{
							char	   *copy = pstrdup(value);
							char	   *token;
							char	   *saveptr;

							token = strtok_r(copy, ";", &saveptr);
							while (token != NULL)
							{
								char	   *colon = strchr(token, ':');

								if (colon != NULL)
								{
									char	   *key = token;
									*colon = '\0';
									char	   *func = colon + 1;

									if (strcmp(key, colname) == 0)
									{
										embedding_func = pstrdup(func);
										break;
									}
								}
								token = strtok_r(NULL, ";", &saveptr);
							}
							pfree(copy);
						}
					}
					pfree(text_str);
					break;
				}
				pfree(text_str);
			}
		}

		pfree(elems);
		pfree(nulls);
	}

	ReleaseSysCache(tuple);
	
	return embedding_func;
}

/*
 * get_embedding_function_oid - Get the OID of the embedding function
 *
 * Returns the function OID if set, or InvalidOid if not set.
 * colname specifies which EMBEDDING column to look up the function for.
 */
Oid
get_embedding_function_oid(Oid relid, const char *colname)
{
	char	   *funcname;
	Oid			funcoid = InvalidOid;
	List	   *namelist;
	FuncCandidateList clist;
	int			fgc_flags;

	funcname = get_embedding_function(relid, colname);
	if (funcname == NULL)
	{
		elog(LOG, "embedding_trigger: no embedding_function set for relid=%u, colname=%s", relid, colname ? colname : "(null)");
		return InvalidOid;
	}

	elog(LOG, "embedding_trigger: looking for function '%s' for column '%s'", funcname, colname ? colname : "(all)");

	if (strchr(funcname, '.') != NULL)
	{
		namelist = stringToQualifiedNameList(funcname, NULL);
	}
	else
	{
		namelist = list_make1(makeString(funcname));
	}

	clist = FuncnameGetCandidates(namelist, 1, NIL, false, false, false, true, &fgc_flags);

	if (clist != NULL)
	{
		elog(LOG, "embedding_trigger: found %d function candidates", clist->next ? 2 : 1);
		for (; clist != NULL; clist = clist->next)
		{
			HeapTuple	proc_tuple;
			Form_pg_proc proc_form;
			
			elog(LOG, "embedding_trigger: candidate oid=%u, nargs=%d", clist->oid, clist->nargs);
			
			if (clist->nargs != 1)
				continue;
			
			/* Check if the argument type is text */
			proc_tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(clist->oid));
			if (!HeapTupleIsValid(proc_tuple))
				continue;
			
			proc_form = (Form_pg_proc) GETSTRUCT(proc_tuple);
			if (proc_form->proargtypes.values[0] == TEXTOID)
			{
				funcoid = clist->oid;
				ReleaseSysCache(proc_tuple);
				elog(LOG, "embedding_trigger: found text-accepting function oid=%u", funcoid);
				break;
			}
			
			ReleaseSysCache(proc_tuple);
		}
	}
	else
	{
		elog(LOG, "embedding_trigger: no function candidates found");
	}

	if (!OidIsValid(funcoid))
	{
		char	   *query;
		Datum		result;
		bool		isnull;
		MemoryContext oldcontext;
		MemoryContext querycontext;
		int			ret;

		querycontext = AllocSetContextCreate(CurrentMemoryContext,
											 "embedding_func_lookup",
											 ALLOCSET_DEFAULT_SIZES);
		oldcontext = MemoryContextSwitchTo(querycontext);

		query = psprintf(
			"SELECT oid FROM pg_proc "
			"WHERE proname = '%s' "
			"AND pronargs = 1 "
			"ORDER BY oid LIMIT 1",
			funcname);

		ret = SPI_connect();
		
		if (ret == SPI_OK_CONNECT)
		{
			ret = SPI_execute(query, true, 1);
			
			if (ret == SPI_OK_SELECT && SPI_processed > 0)
			{
				result = SPI_getbinval(SPI_tuptable->vals[0],
									   SPI_tuptable->tupdesc, 1, &isnull);
				if (!isnull)
					funcoid = DatumGetObjectId(result);
			}
			SPI_finish();
		}

		MemoryContextSwitchTo(oldcontext);
		MemoryContextDelete(querycontext);
	}

	pfree(funcname);
	return funcoid;
}

/*
 * find_column_by_name - find column number by name (1-based)
 * Returns InvalidAttrNumber if not found
 */
static AttrNumber
find_column_by_name(TupleDesc tupdesc, const char *colname)
{
	int		attnum;

	for (attnum = 1; attnum <= tupdesc->natts; attnum++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);
		if (attr->attisdropped)
			continue;
		if (namestrcmp(&attr->attname, colname) == 0)
			return attnum;
	}
	return InvalidAttrNumber;
}

/*
 * predict_trigger - trigger function for PREDICT columns
 *
 * This is a BEFORE INSERT/UPDATE trigger that handles PREDICT columns:
 * - Copies user input value to the _actual column
 * - When PREDICT column is NULL and predict_timing is immediate:
 *   calls predict_function and saves result to _predict column
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
	AttrNumber		target_attnum;
	Datum			coldatum;
	Datum			actual_datum;
	Datum			predict_datum;
	bool			isnull;
	bool			actual_isnull;
	bool			predict_isnull;
	StdRdOptions   *relopts;
	StdRdOptPredictTiming predict_timing;

	if (!CALLED_AS_TRIGGER(fcinfo))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"predict_trigger\" was not called by trigger manager")));

	if (!TRIGGER_FIRED_BEFORE(trigdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"predict_trigger\" must be called as BEFORE trigger")));

	if (!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"predict_trigger\" must be a ROW-level trigger")));

	if (!TRIGGER_FIRED_BY_INSERT(trigdata->tg_event) && !TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"predict_trigger\" must be called for INSERT or UPDATE")));

	rel = trigdata->tg_relation;
	tupdesc = RelationGetDescr(rel);

	if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		newtuple = trigdata->tg_newtuple;
	else
		newtuple = trigdata->tg_trigtuple;

	/* Get target attnum from trigger argument if provided */
	target_attnum = InvalidAttrNumber;
	if (trigdata->tg_trigger->tgnargs > 0)
	{
		char *arg_str = trigdata->tg_trigger->tgargs[0];
		target_attnum = atoi(arg_str);
	}

	/* Get predict_timing option */
	relopts = (StdRdOptions *) rel->rd_options;
	predict_timing = STDRD_OPTION_PREDICT_TIMING_DEFERRED;
	if (relopts != NULL)
		predict_timing = relopts->predict_timing;

	for (attnum = 1; attnum <= tupdesc->natts; attnum++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);
		char	   *actual_colname;
		char	   *predict_colname;
		AttrNumber	actual_attnum;
		AttrNumber	predict_attnum;

		if (attr->attisdropped)
			continue;

		if (!attr->attpredict)
			continue;

		/* If target_attnum is specified, only process that column */
		if (target_attnum != InvalidAttrNumber && attnum != target_attnum)
			continue;

		/* This is a PREDICT column */

		/* Get user input value from the new tuple */
		if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
			coldatum = heap_getattr(trigdata->tg_newtuple, attnum, tupdesc, &isnull);
		else
			coldatum = heap_getattr(trigdata->tg_trigtuple, attnum, tupdesc, &isnull);
		actual_datum = coldatum;
		actual_isnull = isnull;

		/* Find _actual and _predict columns */
		actual_colname = psprintf("%s_actual", NameStr(attr->attname));
		predict_colname = psprintf("%s_predict", NameStr(attr->attname));

		actual_attnum = find_column_by_name(tupdesc, actual_colname);
		predict_attnum = find_column_by_name(tupdesc, predict_colname);

		pfree(actual_colname);
		pfree(predict_colname);

		/* Determine if we need to call predict function */
		predict_datum = (Datum) 0;
		predict_isnull = true;

		if (isnull && predict_timing == STDRD_OPTION_PREDICT_TIMING_IMMEDIATE)
		{
			char *predict_func_name = get_predict_function(rel->rd_id, NameStr(attr->attname));

			if (predict_func_name != NULL)
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
						if (clist->nargs == 1)
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
					Datum row_datum;
					
					fmgr_info(predict_func_oid, &predict_func);
					
					if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
						row_datum = heap_copy_tuple_as_datum(trigdata->tg_newtuple, tupdesc);
					else
						row_datum = heap_copy_tuple_as_datum(trigdata->tg_trigtuple, tupdesc);
					
					predict_datum = FunctionCall1(&predict_func, row_datum);
					predict_isnull = false;
				}

				pfree(predict_func_name);
			}
		}

		/* Modify tuple to set PREDICT column, _actual and _predict columns */
		if (actual_attnum != InvalidAttrNumber && predict_attnum != InvalidAttrNumber)
		{
			int			modify_attnums[3];
			Datum		modify_values[3];
			bool		modify_nulls[3];
			int			nmodify = 0;

			/* Set _actual column */
			modify_attnums[nmodify] = actual_attnum;
			modify_values[nmodify] = actual_datum;
			modify_nulls[nmodify] = actual_isnull;
			nmodify++;

			/* Set _predict column */
			modify_attnums[nmodify] = predict_attnum;
			modify_values[nmodify] = predict_datum;
			modify_nulls[nmodify] = predict_isnull;
			nmodify++;

			/* Set PREDICT column if we have a prediction */
			if (!predict_isnull)
			{
				modify_attnums[nmodify] = attnum;
				modify_values[nmodify] = predict_datum;
				modify_nulls[nmodify] = false;
				nmodify++;
			}

			rettuple = heap_modify_tuple_by_cols(newtuple, tupdesc, nmodify,
												 modify_attnums, modify_values, modify_nulls);
			newtuple = rettuple;
		}
		else if (actual_attnum != InvalidAttrNumber)
		{
			int			modify_attnums[2];
			Datum		modify_values[2];
			bool		modify_nulls[2];
			int			nmodify = 0;

			/* Set _actual column */
			modify_attnums[nmodify] = actual_attnum;
			modify_values[nmodify] = actual_datum;
			modify_nulls[nmodify] = actual_isnull;
			nmodify++;

			/* Set PREDICT column if we have a prediction */
			if (!predict_isnull)
			{
				modify_attnums[nmodify] = attnum;
				modify_values[nmodify] = predict_datum;
				modify_nulls[nmodify] = false;
				nmodify++;
			}

			rettuple = heap_modify_tuple_by_cols(newtuple, tupdesc, nmodify,
												 modify_attnums, modify_values, modify_nulls);
			newtuple = rettuple;
		}
		else if (predict_attnum != InvalidAttrNumber && !predict_isnull)
		{
			int			modify_attnums[2];
			Datum		modify_values[2];
			bool		modify_nulls[2];
			int			nmodify = 0;

			/* Set _predict column */
			modify_attnums[nmodify] = predict_attnum;
			modify_values[nmodify] = predict_datum;
			modify_nulls[nmodify] = false;
			nmodify++;

			/* Set PREDICT column */
			modify_attnums[nmodify] = attnum;
			modify_values[nmodify] = predict_datum;
			modify_nulls[nmodify] = false;
			nmodify++;

			rettuple = heap_modify_tuple_by_cols(newtuple, tupdesc, nmodify,
												 modify_attnums, modify_values, modify_nulls);
			newtuple = rettuple;
		}
		else if (!predict_isnull)
		{
			int			modify_attnums[1];
			Datum		modify_values[1];
			bool		modify_nulls[1];

			modify_attnums[0] = attnum;
			modify_values[0] = predict_datum;
			modify_nulls[0] = false;

			rettuple = heap_modify_tuple_by_cols(newtuple, tupdesc, 1,
												 modify_attnums, modify_values, modify_nulls);
			newtuple = rettuple;
		}
	}

	return PointerGetDatum(newtuple);
}

PG_FUNCTION_INFO_V1(text_vector_l2_distance);

Datum
text_vector_l2_distance(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("text vector distance operator should be rewritten"),
			 errdetail("This operator should be automatically rewritten to use vector type."),
			 errhint("This is an internal error if you see this message.")));
	
	PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(text_vector_cosine_distance);

Datum
text_vector_cosine_distance(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("text vector distance operator should be rewritten"),
			 errdetail("This operator should be automatically rewritten to use vector type."),
			 errhint("This is an internal error if you see this message.")));
	
	PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(text_vector_ip_distance);

Datum
text_vector_ip_distance(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("text vector distance operator should be rewritten"),
			 errdetail("This operator should be automatically rewritten to use vector type."),
			 errhint("This is an internal error if you see this message.")));
	
	PG_RETURN_NULL();
}

/*
 * embedding_trigger - trigger function for EMBEDDING columns
 *
 * This is a BEFORE INSERT/UPDATE trigger that handles EMBEDDING columns:
 * - Calls embedding_function and saves result to _embedding column
 */
PG_FUNCTION_INFO_V1(embedding_trigger);

Datum
embedding_trigger(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	TupleDesc		tupdesc;
	HeapTuple		rettuple;
	HeapTuple		newtuple;
	Relation		rel;
	int				attnum;

	if (!CALLED_AS_TRIGGER(fcinfo))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"embedding_trigger\" was not called by trigger manager")));

	if (!TRIGGER_FIRED_BEFORE(trigdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"embedding_trigger\" must be called as BEFORE trigger")));

	if (!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"embedding_trigger\" must be a ROW-level trigger")));

	if (!TRIGGER_FIRED_BY_INSERT(trigdata->tg_event) && !TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"embedding_trigger\" must be called for INSERT or UPDATE")));

	rel = trigdata->tg_relation;
	tupdesc = RelationGetDescr(rel);

	if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		newtuple = trigdata->tg_newtuple;
	else
		newtuple = trigdata->tg_trigtuple;

	for (attnum = 1; attnum <= tupdesc->natts; attnum++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);
		char	   *embedding_colname;
		AttrNumber	embedding_attnum;

		if (attr->attisdropped)
			continue;

		if (!attr->attembedding)
			continue;

		/* This is an EMBEDDING column */

		/* Find _embedding column */
		embedding_colname = psprintf("%s_embedding", NameStr(attr->attname));
		embedding_attnum = find_column_by_name(tupdesc, embedding_colname);
		pfree(embedding_colname);

		if (embedding_attnum == InvalidAttrNumber)
			continue;

		/* Get embedding function OID */
		{
			Oid embedding_func_oid = get_embedding_function_oid(rel->rd_id, NameStr(attr->attname));

			if (OidIsValid(embedding_func_oid))
			{
				FmgrInfo embedding_func;
				Datum col_datum;
				bool col_isnull;
				Datum embedding_datum;
				char *col_value;
				
				fmgr_info(embedding_func_oid, &embedding_func);
				
				/* Get the value of the current embedding column */
				col_datum = heap_getattr(newtuple, attnum, tupdesc, &col_isnull);
				
				if (!col_isnull)
				{
					/* Get the text value for logging */
					col_value = TextDatumGetCString(col_datum);
					elog(LOG, "embedding_trigger: calling function for column '%s' with value '%s'", 
						 NameStr(attr->attname), col_value);
					pfree(col_value);
					
					/* Call embedding function with the column value */
					embedding_datum = FunctionCall1(&embedding_func, col_datum);
					
					elog(LOG, "embedding_trigger: function returned successfully");
					
					/* Set _embedding column */
					{
						int			modify_attnums[1];
						Datum		modify_values[1];
						bool		modify_nulls[1];

						modify_attnums[0] = embedding_attnum;
						modify_values[0] = embedding_datum;
						modify_nulls[0] = false;

						rettuple = heap_modify_tuple_by_cols(newtuple, tupdesc, 1,
															 modify_attnums, modify_values, modify_nulls);
						newtuple = rettuple;
					}
				}
				else
				{
					elog(LOG, "embedding_trigger: column '%s' is null, skipping", NameStr(attr->attname));
				}
			}
			else
			{
				elog(LOG, "embedding_trigger: no embedding function found for column '%s'", NameStr(attr->attname));
			}
		}
	}

	return PointerGetDatum(newtuple);
}
