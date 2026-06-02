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
#include "utils/datum.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/varlena.h"
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
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
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
#include "executor/executor.h"
#include "rewrite/rewriteHandler.h"

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
									char	   *func;
									*colon = '\0';
									func = colon + 1;

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
 *   evaluates the PREDICT AS expression and saves result to _predict column
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

		/* Get the value of the predict column from the new tuple */
		coldatum = heap_getattr(newtuple, attnum, tupdesc, &isnull);

		/*
		 * For UPDATE: determine if the user explicitly modified the predict
		 * column by comparing old and new values.
		 *
		 * - If old was NULL and new is non-NULL: this is likely a prediction
		 *   being filled in (by async worker or trigger). Check if _predict
		 *   column already has the same value - if so, the async worker
		 *   already set everything, skip processing. Otherwise, set _predict.
		 * - If old was non-NULL and new differs: user explicitly changed it,
		 *   save to _actual.
		 * - If old and new are the same: predict column not modified,
		 *   recompute prediction.
		 */
		if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event) && !isnull)
		{
			Datum	olddatum;
			bool	oldisnull;

			olddatum = heap_getattr(trigdata->tg_trigtuple, attnum, tupdesc, &oldisnull);

			if (oldisnull)
			{
				Datum		predict_col_val;
				bool		predict_col_isnull;
				char	   *predict_colname_tmp;
				AttrNumber	predict_col_attnum_tmp;

				predict_colname_tmp = psprintf("%s_predict", NameStr(attr->attname));
				predict_col_attnum_tmp = find_column_by_name(tupdesc, predict_colname_tmp);
				pfree(predict_colname_tmp);

				if (predict_col_attnum_tmp != InvalidAttrNumber)
				{
					predict_col_val = heap_getattr(newtuple, predict_col_attnum_tmp,
												   tupdesc, &predict_col_isnull);
					if (!predict_col_isnull &&
						datumIsEqual(coldatum, predict_col_val,
									attr->attbyval, attr->attlen))
					{
						continue;
					}
				}

				isnull = true;
			}
			else if (datumIsEqual(coldatum, olddatum, attr->attbyval, attr->attlen))
			{
				isnull = true;
			}
		}

		/* Find _actual and _predict columns */
		actual_colname = psprintf("%s_actual", NameStr(attr->attname));
		predict_colname = psprintf("%s_predict", NameStr(attr->attname));

		actual_attnum = find_column_by_name(tupdesc, actual_colname);
		predict_attnum = find_column_by_name(tupdesc, predict_colname);

		pfree(actual_colname);
		pfree(predict_colname);

		predict_datum = (Datum) 0;
		predict_isnull = true;
		actual_datum = (Datum) 0;

		if (!isnull)
		{
			actual_datum = coldatum;
		}
		else if (predict_timing == STDRD_OPTION_PREDICT_TIMING_IMMEDIATE)
		{
			if (attr->attgenerated == ATTRIBUTE_GENERATED_PREDICT)
			{
				Expr	   *expr;
				EState	   *estate;
				ExprState  *exprstate;
				ExprContext *econtext;
				TupleTableSlot *slot;
				Datum		val;
				bool		val_isnull;
				char	   *saved_current_table = NULL;
				const char *relname_str;

				relname_str = RelationGetRelationName(rel);
				{
					const char *cur_val = GetConfigOption("jolix_predict.current_table", true, false);
					if (cur_val && strlen(cur_val) > 0)
						saved_current_table = pstrdup(cur_val);
				}
				SetConfigOption("jolix_predict.current_table", relname_str,
								PGC_USERSET, PGC_S_SESSION);

				PG_TRY();
				{
					expr = (Expr *) build_column_default(rel, attnum);
					if (expr != NULL)
					{
						estate = CreateExecutorState();
						exprstate = ExecPrepareExpr(expr, estate);

						slot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsHeapTuple);
						ExecStoreHeapTuple(newtuple, slot, false);

						econtext = GetPerTupleExprContext(estate);
						econtext->ecxt_scantuple = slot;

						val = ExecEvalExpr(exprstate, econtext, &val_isnull);

						if (!val_isnull)
						{
							predict_datum = datumCopy(val, attr->attbyval, attr->attlen);
							predict_isnull = false;
						}

						ExecDropSingleTupleTableSlot(slot);
						FreeExecutorState(estate);
					}
				}
				PG_CATCH();
				{
					if (saved_current_table)
						SetConfigOption("jolix_predict.current_table", saved_current_table,
										PGC_USERSET, PGC_S_SESSION);
					else
						SetConfigOption("jolix_predict.current_table", "",
										PGC_USERSET, PGC_S_SESSION);
					PG_RE_THROW();
				}
				PG_END_TRY();

				if (saved_current_table)
				{
					SetConfigOption("jolix_predict.current_table", saved_current_table,
									PGC_USERSET, PGC_S_SESSION);
					pfree(saved_current_table);
				}
				else
					SetConfigOption("jolix_predict.current_table", "",
									PGC_USERSET, PGC_S_SESSION);
			}
		}

		/* Modify tuple to set PREDICT column, _actual and _predict columns */
		{
			int			modify_attnums[3];
			Datum		modify_values[3];
			bool		modify_nulls[3];
			int			nmodify = 0;

			if (!isnull)
			{
				/* User provided a value: save to _actual column */
				if (actual_attnum != InvalidAttrNumber)
				{
					modify_attnums[nmodify] = actual_attnum;
					modify_values[nmodify] = actual_datum;
					modify_nulls[nmodify] = false;
					nmodify++;
				}
			}
			else
			{
				/* No user value: set _predict column and predict column */
				if (predict_attnum != InvalidAttrNumber)
				{
					modify_attnums[nmodify] = predict_attnum;
					modify_values[nmodify] = predict_datum;
					modify_nulls[nmodify] = predict_isnull;
					nmodify++;
				}

				if (!predict_isnull)
				{
					modify_attnums[nmodify] = attnum;
					modify_values[nmodify] = predict_datum;
					modify_nulls[nmodify] = false;
					nmodify++;
				}
			}

			if (nmodify > 0)
			{
				rettuple = heap_modify_tuple_by_cols(newtuple, tupdesc, nmodify,
													 modify_attnums, modify_values, modify_nulls);
				newtuple = rettuple;
			}
		}
	}

	return PointerGetDatum(newtuple);
}

PG_FUNCTION_INFO_V1(llm_predict);
PG_FUNCTION_INFO_V1(llm_rag_predict);

static Oid
find_extension_function_oid(const char *funcname, int nargs)
{
	Oid					funcoid = InvalidOid;
	FuncCandidateList	clist;
	int					fgc_flags;
	char			   *short_name;

	short_name = strrchr(funcname, '.');
	if (short_name != NULL)
		short_name++;
	else
		short_name = (char *) funcname;

	clist = FuncnameGetCandidates(list_make1(makeString(short_name)),
								  nargs, NIL, false, false, false, true, &fgc_flags);

	if (clist != NULL)
	{
		for (; clist != NULL; clist = clist->next)
		{
			if (clist->nargs == nargs)
			{
				funcoid = clist->oid;
				break;
			}
		}
	}

	return funcoid;
}

Datum
llm_predict(PG_FUNCTION_ARGS)
{
	Oid			ext_func_oid;
	FmgrInfo	ext_func;
	Datum		row_datum;
	Datum		result;

	ext_func_oid = find_extension_function_oid("pg_predict.llm_predict_ext", 1);
	if (!OidIsValid(ext_func_oid))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("llm_predict requires the pg_predict extension"),
				 errhint("Install pg_predict extension first: CREATE EXTENSION pg_predict;")));

	fmgr_info(ext_func_oid, &ext_func);

	row_datum = PG_GETARG_DATUM(0);
	result = FunctionCall1(&ext_func, row_datum);

	if (DatumGetPointer(result) == NULL)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(result);
}

Datum
llm_rag_predict(PG_FUNCTION_ARGS)
{
	Oid			ext_func_oid;
	FmgrInfo	ext_func;
	Datum		row_datum;
	Datum		result;

	ext_func_oid = find_extension_function_oid("pg_predict.llm_rag_predict_ext", 1);
	if (!OidIsValid(ext_func_oid))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("llm_rag_predict requires the pg_predict extension"),
				 errhint("Install pg_predict extension first: CREATE EXTENSION pg_predict;")));

	fmgr_info(ext_func_oid, &ext_func);

	row_datum = PG_GETARG_DATUM(0);
	result = FunctionCall1(&ext_func, row_datum);

	if (DatumGetPointer(result) == NULL)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(result);
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
 * - Evaluates the EMBEDDING AS expression and saves result to _embedding column
 */
PG_FUNCTION_INFO_V1(embedding_trigger);
PG_FUNCTION_INFO_V1(embeddings_trigger);

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

		embedding_colname = psprintf("%s_embedding", NameStr(attr->attname));
		embedding_attnum = find_column_by_name(tupdesc, embedding_colname);
		pfree(embedding_colname);

		if (embedding_attnum == InvalidAttrNumber)
			continue;

		if (attr->attgenerated == ATTRIBUTE_GENERATED_EMBEDDING)
		{
			Expr	   *expr;
			EState	   *estate;
			ExprState  *exprstate;
			ExprContext *econtext;
			TupleTableSlot *slot;
			Datum		val;
			bool		val_isnull;

			expr = (Expr *) TupleDescGetDefault(tupdesc, attnum);
			if (expr != NULL)
			{
				if (IsA(expr, CoerceViaIO))
				{
					CoerceViaIO *coerce = (CoerceViaIO *) expr;
					expr = coerce->arg;
				}

				estate = CreateExecutorState();
				exprstate = ExecPrepareExpr(expr, estate);

				slot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsHeapTuple);
				ExecStoreHeapTuple(newtuple, slot, false);

				econtext = GetPerTupleExprContext(estate);
				econtext->ecxt_scantuple = slot;

				val = ExecEvalExpr(exprstate, econtext, &val_isnull);

				if (!val_isnull)
				{
					int			modify_attnums[1];
					Datum		modify_values[1];
					bool		modify_nulls[1];

					modify_attnums[0] = embedding_attnum;
					modify_values[0] = val;
					modify_nulls[0] = false;

					rettuple = heap_modify_tuple_by_cols(newtuple, tupdesc, 1,
														 modify_attnums, modify_values, modify_nulls);
					newtuple = rettuple;

					elog(LOG, "embedding_trigger: computed embedding for column '%s'",
						 NameStr(attr->attname));
				}
				else
				{
					elog(LOG, "embedding_trigger: expression returned NULL for column '%s'",
						 NameStr(attr->attname));
				}

				ExecDropSingleTupleTableSlot(slot);
				FreeExecutorState(estate);
			}
			else
			{
				Oid embedding_func_oid = get_embedding_function_oid(rel->rd_id, NameStr(attr->attname));

				if (OidIsValid(embedding_func_oid))
				{
					FmgrInfo embedding_func;
					Datum col_datum;
					bool col_isnull;
					Datum embedding_datum;

					fmgr_info(embedding_func_oid, &embedding_func);

					col_datum = heap_getattr(newtuple, attnum, tupdesc, &col_isnull);

					if (!col_isnull)
					{
						embedding_datum = FunctionCall1(&embedding_func, col_datum);

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
				}
			}
		}
		else
		{
			Oid embedding_func_oid = get_embedding_function_oid(rel->rd_id, NameStr(attr->attname));

			if (OidIsValid(embedding_func_oid))
			{
				FmgrInfo embedding_func;
				Datum col_datum;
				bool col_isnull;
				Datum embedding_datum;

				fmgr_info(embedding_func_oid, &embedding_func);

				col_datum = heap_getattr(newtuple, attnum, tupdesc, &col_isnull);

				if (!col_isnull)
				{
					embedding_datum = FunctionCall1(&embedding_func, col_datum);

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
			}
		}
	}

	return PointerGetDatum(newtuple);
}

Datum
embeddings_trigger(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	TupleDesc	tupdesc;
	HeapTuple	newtuple;
	Relation	rel;
	int			attnum;
	char	  **tg_args;
	int			nargs;

	if (!CALLED_AS_TRIGGER(fcinfo))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"embeddings_trigger\" was not called by trigger manager")));

	if (!TRIGGER_FIRED_BEFORE(trigdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"embeddings_trigger\" must be called as BEFORE trigger")));

	if (!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"embeddings_trigger\" must be a ROW-level trigger")));

	if (!TRIGGER_FIRED_BY_INSERT(trigdata->tg_event) && !TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"embeddings_trigger\" must be called for INSERT or UPDATE")));

	rel = trigdata->tg_relation;
	tupdesc = RelationGetDescr(rel);

	if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		newtuple = trigdata->tg_newtuple;
	else
		newtuple = trigdata->tg_trigtuple;

	tg_args = trigdata->tg_trigger->tgargs;
	nargs = trigdata->tg_trigger->tgnargs;

	for (attnum = 1; attnum <= tupdesc->natts; attnum++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

		if (attr->attisdropped)
			continue;

		if (!attr->attembeddings)
			continue;

		if (nargs >= 3)
		{
			char	   *vecname = tg_args[0];
			char	   *accessMethod = tg_args[1];
			char	   *colnames_str = tg_args[2];
			List	   *colnames_list;
			ListCell   *lc;
			Oid			funcoid = InvalidOid;
			FmgrInfo	funcinfo;
			int			ncolumns = 0;
			Datum	   *col_values;
			bool	   *col_nulls;
			int			i;
			bool		any_null = false;

			if (strcmp(NameStr(attr->attname), vecname) != 0)
				continue;

			if (!SplitIdentifierString(colnames_str, ',', &colnames_list))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid column list in embeddings trigger")));

			ncolumns = list_length(colnames_list);
			col_values = (Datum *) palloc(sizeof(Datum) * ncolumns);
			col_nulls = (bool *) palloc(sizeof(bool) * ncolumns);
			Oid		   *col_types = (Oid *) palloc(sizeof(Oid) * ncolumns);

			i = 0;
			foreach(lc, colnames_list)
			{
				char	   *colname = (char *) lfirst(lc);
				AttrNumber	src_attnum;
				Datum		col_datum;
				bool		col_isnull;
				Form_pg_attribute src_attr;

				src_attnum = get_attnum(RelationGetRelid(rel), colname);
				if (src_attnum == InvalidAttrNumber)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
							 errmsg("column \"%s\" does not exist", colname)));

				src_attr = TupleDescAttr(tupdesc, src_attnum - 1);
				col_types[i] = src_attr->atttypid;

				col_datum = heap_getattr(newtuple, src_attnum, tupdesc, &col_isnull);
				col_values[i] = col_datum;
				col_nulls[i] = col_isnull;
				if (col_isnull)
					any_null = true;
				i++;
			}

			if (!any_null)
			{
				List	   *namelist;
				FuncCandidateList clist;
				int			fgc_flags;

				namelist = stringToQualifiedNameList(accessMethod, NULL);
				clist = FuncnameGetCandidates(namelist, ncolumns, NIL, true, false, false, true, &fgc_flags);

				if (clist != NULL)
				{
					for (; clist != NULL; clist = clist->next)
					{
						if (clist->nargs == ncolumns || (clist->nvargs > 0 && ncolumns >= clist->nargs - clist->nvargs))
						{
							funcoid = clist->oid;
							break;
						}
					}
				}

				if (OidIsValid(funcoid))
				{
					Datum		vec_datum;
					int			modify_attnums[1];
					Datum		modify_values[1];
					bool		modify_nulls[1];
					HeapTuple	rettuple;
					FuncExpr   *funcexpr;
					List	   *args_list = NIL;
					Oid		   *argtypes_arr;
					Oid			rettype;

					fmgr_info(funcoid, &funcinfo);

					rettype = get_func_rettype(funcoid);

					argtypes_arr = (Oid *) palloc(sizeof(Oid) * ncolumns);
					for (i = 0; i < ncolumns; i++)
					{
						argtypes_arr[i] = col_types[i];
						args_list = lappend(args_list, makeVar(1, i + 1, col_types[i], -1, InvalidOid, 0));
					}

					funcexpr = makeFuncExpr(funcoid, rettype, args_list,
										   InvalidOid, InvalidOid,
										   COERCE_EXPLICIT_CALL);

					funcinfo.fn_expr = (Node *) funcexpr;

					{
						FunctionCallInfo fcinfo_local;
						int j;

						fcinfo_local = (FunctionCallInfo) palloc(SizeForFunctionCallInfo(ncolumns));
						InitFunctionCallInfoData(*fcinfo_local, &funcinfo, ncolumns, InvalidOid, NULL, NULL);
						for (j = 0; j < ncolumns; j++)
						{
							fcinfo_local->args[j].value = col_values[j];
							fcinfo_local->args[j].isnull = col_nulls[j];
						}

						vec_datum = FunctionCallInvoke(fcinfo_local);
						if (fcinfo_local->isnull)
						{
							pfree(fcinfo_local);
							pfree(argtypes_arr);
							pfree(col_values);
							pfree(col_nulls);
							pfree(col_types);
							list_free(colnames_list);
							continue;
						}
						pfree(fcinfo_local);
					}

					pfree(argtypes_arr);

					modify_attnums[0] = attnum;
					modify_values[0] = vec_datum;
					modify_nulls[0] = false;

					rettuple = heap_modify_tuple_by_cols(newtuple, tupdesc, 1,
														 modify_attnums, modify_values, modify_nulls);
					newtuple = rettuple;
				}
				else
				{
					elog(LOG, "embeddings_trigger: function '%s' with %d args not found", accessMethod, ncolumns);
				}
			}

			pfree(col_values);
			pfree(col_nulls);
			pfree(col_types);
			list_free(colnames_list);
		}
	}

	return PointerGetDatum(newtuple);
}
