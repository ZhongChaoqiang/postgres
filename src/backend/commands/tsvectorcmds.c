/*-------------------------------------------------------------------------
 *
 * tsvectorcmds.c
 *	  Commands for creating timeseries vector tables.
 *
 * A timeseries vector table is a normal PostgreSQL table created through the
 * standard CREATE TABLE ... WITH (...) syntax.  When the WITH clause contains
 * a "timeseries.source" option, the core injects all system-required columns
 * (slice_start, slice_end, the carry columns, the vector column and the
 * quality metadata columns) plus a PRIMARY KEY constraint, and stores the
 * timeseries.* options in pg_class.reloptions under their own namespace.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 * src/backend/commands/tsvectorcmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/attnum.h"
#include "access/relation.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "access/tupdesc.h"
#include "catalog/namespace.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/tsvectorcmds.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/value.h"
#include "parser/parser.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/regproc.h"

/* Prefix used when serializing a timeseries.* reloption into pg_class. */
#define TS_RELOPT_PREFIX "timeseries."

/* Number of dimensions of a vector column when timeseries.vector_len is not
 * given.  Matches the default used across the timeseries vector feature. */
#define TS_DEFAULT_VECTOR_LEN 384

/* Default names for the vector column and vectorize function. */
#define TS_DEFAULT_VECTOR_COLUMN "embedding"
#define TS_DEFAULT_VECTORIZE_FUNCTION "ts2v_moment"


/* ----------------------------------------------------------------------
 * Reloption helpers.
 * ---------------------------------------------------------------------- */

/*
 * Look up a "timeseries.<name>" option in a DefElem list and return its
 * string value (via defGetString), or NULL when absent.
 */
static char *
tsrelopt_get_string(List *options, const char *name)
{
	ListCell   *lc;

	foreach(lc, options)
	{
		DefElem    *def = lfirst_node(DefElem, lc);

		if (def->defnamespace != NULL &&
			strcmp(def->defnamespace, "timeseries") == 0 &&
			strcmp(def->defname, name) == 0)
			return defGetString(def);
	}
	return NULL;
}

/*
 * Look up a "timeseries.<name>" integer option, returning default_val when
 * absent.
 */
static int
tsrelopt_get_int32(List *options, const char *name, int default_val)
{
	ListCell   *lc;

	foreach(lc, options)
	{
		DefElem    *def = lfirst_node(DefElem, lc);

		if (def->defnamespace != NULL &&
			strcmp(def->defnamespace, "timeseries") == 0 &&
			strcmp(def->defname, name) == 0)
			return defGetInt32(def);
	}
	return default_val;
}


/* ----------------------------------------------------------------------
 * Column injection helpers.
 * ---------------------------------------------------------------------- */

/*
 * Does the given table element list already declare a column "name"?
 * Only ColumnDef nodes are considered.
 */
static bool
column_name_exists(List *tableElts, const char *name)
{
	ListCell   *lc;

	foreach(lc, tableElts)
	{
		Node	   *node = lfirst(lc);

		if (IsA(node, ColumnDef) &&
			strcmp(((ColumnDef *) node)->colname, name) == 0)
			return true;
	}
	return false;
}

/*
 * Build a raw (untransformed) boolean A_Const, matching what the grammar
 * produces for a bare TRUE/FALSE literal.  raw_default must hold an A_Const
 * (not a transformed Const), otherwise transformExpr() rejects it.
 */
static Node *
make_raw_bool_const(bool state)
{
	A_Const    *n = makeNode(A_Const);

	n->val.boolval.type = T_Boolean;
	n->val.boolval.boolval = state;
	n->location = -1;

	return (Node *) n;
}

/*
 * Build a simple injected ColumnDef from a built-in type OID.  Optionally
 * marks it NOT NULL and/or supplies a raw (untransformed) default expression.
 */
static ColumnDef *
make_injected_column(const char *colname, Oid typeOid, int32 typmod,
					 bool is_not_null, Node *raw_default)
{
	ColumnDef  *col = makeColumnDef(colname, typeOid, typmod, InvalidOid);

	col->is_not_null = is_not_null;
	col->raw_default = raw_default;
	col->is_hidden = false;

	return col;
}

/*
 * Build the injected vector column of type vector(vector_len).
 */
static ColumnDef *
make_injected_vector_column(const char *colname, int vector_len)
{
	ColumnDef  *col = makeNode(ColumnDef);
	TypeName   *type = makeNode(TypeName);
	A_Const    *typmod;

	typmod = makeNode(A_Const);
	typmod->val.ival.type = T_Integer;
	typmod->val.ival.ival = vector_len;
	typmod->location = -1;

	type->names = list_make1(makeString("vector"));
	type->typmods = list_make1(typmod);
	type->typemod = -1;
	type->location = -1;

	col->colname = pstrdup(colname);
	col->typeName = type;
	col->compression = NULL;
	col->inhcount = 0;
	col->is_local = true;
	col->is_not_null = false;
	col->is_from_type = false;
	col->is_predict = false;
	col->is_embedding = false;
	col->is_embeddings = false;
	col->is_hidden = false;
	col->embeddings_func = NIL;
	col->embeddings_cols = NIL;
	col->storage = 0;
	col->storage_name = NULL;
	col->raw_default = NULL;
	col->cooked_default = NULL;
	col->identity = '\0';
	col->identitySequence = NULL;
	col->generated = '\0';
	col->collClause = NULL;
	col->collOid = InvalidOid;
	col->constraints = NIL;
	col->fdwoptions = NIL;
	col->location = -1;

	return col;
}

/*
 * Split a comma-separated list of column names into a List of freshly
 * allocated, whitespace-trimmed strings.
 */
static List *
parse_carry_columns(const char *carry_str)
{
	List	   *result = NIL;
	char	   *copy;
	char	   *tok;

	if (carry_str == NULL)
		return NIL;

	copy = pstrdup(carry_str);
	for (tok = strtok(copy, ","); tok != NULL; tok = strtok(NULL, ","))
	{
		char	   *s = tok;
		char	   *e;

		/* Trim leading whitespace. */
		while (*s == ' ' || *s == '\t')
			s++;

		/* Trim trailing whitespace. */
		e = s + strlen(s);
		while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
			e--;
		*e = '\0';

		if (*s != '\0')
			result = lappend(result, pstrdup(s));
	}

	pfree(copy);
	return result;
}

/*
 * Build the HNSW vector index statement that is created automatically after
 * the vector table itself is created.
 */
static IndexStmt *
make_timeseries_hnsw_index(RangeVar *relation, const char *vector_column)
{
	IndexStmt  *index = makeNode(IndexStmt);
	IndexElem  *iparam = makeNode(IndexElem);

	index->idxname = psprintf("%s_%s_idx", relation->relname, vector_column);
	index->relation = copyObject(relation);
	index->accessMethod = pstrdup("hnsw");
	index->tableSpace = NULL;
	index->options = NIL;
	index->whereClause = NULL;
	index->excludeOpNames = NIL;
	index->idxcomment = NULL;
	index->indexOid = InvalidOid;
	index->oldNumber = InvalidRelFileNumber;
	index->oldCreateSubid = InvalidSubTransactionId;
	index->oldFirstRelfilelocatorSubid = InvalidSubTransactionId;
	index->unique = false;
	index->nulls_not_distinct = false;
	index->primary = false;
	index->isconstraint = false;
	index->iswithoutoverlaps = false;
	index->deferrable = false;
	index->initdeferred = false;
	index->transformed = false;
	index->concurrent = false;
	index->if_not_exists = true;
	index->reset_default_tblspc = false;

	iparam->name = pstrdup(vector_column);
	iparam->expr = NULL;
	iparam->indexcolname = NULL;
	iparam->collation = NIL;
	iparam->opclass = list_make1(makeString("vector_cosine_ops"));
	iparam->opclassopts = NIL;
	iparam->ordering = SORTBY_DEFAULT;
	iparam->nulls_ordering = SORTBY_NULLS_DEFAULT;

	index->indexParams = list_make1(iparam);
	index->indexIncludingParams = NIL;

	return index;
}


/* ----------------------------------------------------------------------
 * Main column injection entry point.
 * ---------------------------------------------------------------------- */

/*
 * InjectTimeseriesColumns
 *
 * See header comment.  The injection mutates stmt->tableElts in place and
 * returns a List of IndexStmt nodes (the HNSW vector index) to be executed
 * after the CREATE TABLE itself.
 */
List *
InjectTimeseriesColumns(CreateStmt *stmt)
{
	char	   *source_name;
	char	   *carry_str;
	char	   *vector_column;
	int			vector_len;
	Oid			source_relid;
	Relation	source_rel;
	TupleDesc	tupdesc;
	List	   *carry_names = NIL;
	List	   *injected = NIL;
	List	   *pk_keys = NIL;
	Constraint *pk;
	ListCell   *lc;
	List	   *extra_stmts = NIL;

	/* Trigger: only act when timeseries.source is present. */
	source_name = tsrelopt_get_string(stmt->options, "source");
	if (source_name == NULL)
		return NIL;

	/* timeseries.bucket_interval is required and must be a positive integer (seconds). */
	{
		int			bucket_interval;

		bucket_interval = tsrelopt_get_int32(stmt->options, "bucket_interval", 0);
		if (bucket_interval <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("timeseries.bucket_interval must be a positive integer (seconds)")));
	}

	carry_str = tsrelopt_get_string(stmt->options, "carry_columns");
	vector_column = tsrelopt_get_string(stmt->options, "vector_column");
	if (vector_column == NULL)
		vector_column = pstrdup(TS_DEFAULT_VECTOR_COLUMN);
	vector_len = tsrelopt_get_int32(stmt->options, "vector_len",
									TS_DEFAULT_VECTOR_LEN);

	/* The vector type must be available (provided by pgvector). */
	if (!OidIsValid(TypenameGetTypid("vector")))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type \"vector\" is not installed"),
				 errhint("Install the pgvector extension first: CREATE EXTENSION vector;")));

	/* Open the source table to validate it and read carry-column types. */
	{
		List	   *names = stringToQualifiedNameList(source_name, NULL);
		RangeVar   *rv = makeRangeVarFromNameList(names);

		source_relid = RangeVarGetRelid(rv, AccessShareLock, false);
	}
	source_rel = relation_open(source_relid, AccessShareLock);
	tupdesc = RelationGetDescr(source_rel);

	/* Parse and validate the carry columns. */
	carry_names = parse_carry_columns(carry_str);
	foreach(lc, carry_names)
	{
		char	   *colname = (char *) lfirst(lc);

		if (get_attnum(source_relid, colname) == InvalidAttrNumber)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("carry column \"%s\" does not exist in source table \"%s\"",
							colname, source_name)));
	}

	/* Validate vector_len is within pgvector's supported range. */
	if (vector_len <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("timeseries.vector_len must be positive")));


	/*
	 * Inject the fixed time-slice columns.  Skip any column the user already
	 * declared explicitly.
	 */
	if (!column_name_exists(stmt->tableElts, "slice_start"))
		injected = lappend(injected,
						   make_injected_column("slice_start", TIMESTAMPTZOID,
												-1, true, NULL));
	if (!column_name_exists(stmt->tableElts, "slice_end"))
		injected = lappend(injected,
						   make_injected_column("slice_end", TIMESTAMPTZOID,
												-1, true, NULL));

	/* Inject the carry columns with types taken from the source table. */
	foreach(lc, carry_names)
	{
		char	   *colname = (char *) lfirst(lc);
		AttrNumber	attnum;
		Form_pg_attribute attr;
		ColumnDef  *col;

		if (column_name_exists(stmt->tableElts, colname))
			continue;

		attnum = get_attnum(source_relid, colname);
		attr = TupleDescAttr(tupdesc, attnum - 1);

		col = makeColumnDef(colname, attr->atttypid, attr->atttypmod,
							attr->attcollation);
		col->is_not_null = true;
		injected = lappend(injected, col);
	}

	/* Inject the vector column. */
	if (!column_name_exists(stmt->tableElts, vector_column))
		injected = lappend(injected,
						   make_injected_vector_column(vector_column,
													   vector_len));

	/* Inject the quality metadata columns. */
	if (!column_name_exists(stmt->tableElts, "_processed"))
		injected = lappend(injected,
						   make_injected_column("_processed", BOOLOID, -1,
												false, make_raw_bool_const(true)));
	if (!column_name_exists(stmt->tableElts, "_data_watermark"))
		injected = lappend(injected,
						   make_injected_column("_data_watermark", TIMESTAMPTZOID,
												-1, false, NULL));
	if (!column_name_exists(stmt->tableElts, "_coverage"))
		injected = lappend(injected,
						   make_injected_column("_coverage", FLOAT8OID, -1,
												false, NULL));
	if (!column_name_exists(stmt->tableElts, "_row_count"))
		injected = lappend(injected,
						   make_injected_column("_row_count", INT4OID, -1,
												false, NULL));
	if (!column_name_exists(stmt->tableElts, "_gap_count"))
		injected = lappend(injected,
						   make_injected_column("_gap_count", INT4OID, -1,
												false, NULL));
	if (!column_name_exists(stmt->tableElts, "_created_at"))
		injected = lappend(injected,
						   make_injected_column("_created_at", TIMESTAMPTZOID,
												-1, false,
												(Node *) makeFuncCall(SystemFuncName("now"),
																	  NIL,
																	  COERCE_EXPLICIT_CALL,
																	  -1)));

	relation_close(source_rel, AccessShareLock);

	/* Build the PRIMARY KEY (slice_start, carry columns...) constraint. */
	pk_keys = lappend(pk_keys, makeString("slice_start"));
	foreach(lc, carry_names)
		pk_keys = lappend(pk_keys, makeString((char *) lfirst(lc)));

	pk = makeNode(Constraint);
	pk->contype = CONSTR_PRIMARY;
	pk->location = -1;
	pk->keys = pk_keys;
	injected = lappend(injected, pk);

	/* Prepend the injected elements ahead of any user-declared columns. */
	stmt->tableElts = list_concat(injected, stmt->tableElts);

	/* The HNSW vector index runs after the table is created. */
	extra_stmts = lappend(extra_stmts,
						  make_timeseries_hnsw_index(stmt->relation,
													 vector_column));

	return extra_stmts;
}


/* ----------------------------------------------------------------------
 * Reloption storage (bypass) helpers.
 * ---------------------------------------------------------------------- */

/*
 * Does the option element beginning at text_str/text_len carry the
 * "timeseries." prefix?
 */
static bool
is_timeseries_option(const char *text_str, int text_len)
{
	const char *prefix = TS_RELOPT_PREFIX;
	int			prefix_len = (int) strlen(prefix);

	return (text_len > prefix_len &&
			strncmp(text_str, prefix, prefix_len) == 0);
}

/*
 * Does defList contain a "timeseries.<name>" DefElem whose name matches the
 * length-bounded name starting at name_ptr?
 */
static bool
timeseries_def_matches(List *defList, const char *name_ptr, int name_len)
{
	ListCell   *lc;

	foreach(lc, defList)
	{
		DefElem    *def = lfirst_node(DefElem, lc);

		if (def->defnamespace != NULL &&
			strcmp(def->defnamespace, "timeseries") == 0 &&
			(int) strlen(def->defname) == name_len &&
			strncmp(def->defname, name_ptr, name_len) == 0)
			return true;
	}
	return false;
}

/*
 * timeseries_strip_reloptions
 *
 * See header comment.
 */
Datum
timeseries_strip_reloptions(Datum options)
{
	ArrayBuildState *astate = NULL;
	ArrayType  *array;
	Datum	   *elems;
	int			nelems;
	int			i;

	if (options == (Datum) 0 || DatumGetPointer(options) == NULL)
		return (Datum) 0;

	array = DatumGetArrayTypeP(options);
	deconstruct_array_builtin(array, TEXTOID, &elems, NULL, &nelems);

	for (i = 0; i < nelems; i++)
	{
		char	   *text_str = VARDATA(elems[i]);
		int			text_len = VARSIZE(elems[i]) - VARHDRSZ;

		if (!is_timeseries_option(text_str, text_len))
			astate = accumArrayResult(astate, elems[i], false, TEXTOID,
									  CurrentMemoryContext);
	}

	if (astate == NULL)
		return (Datum) 0;

	return makeArrayResult(astate, CurrentMemoryContext);
}

/*
 * timeseries_merge_reloptions
 *
 * See header comment.
 */
Datum
timeseries_merge_reloptions(Datum base, Datum old_options, List *defList,
							bool isReset)
{
	ArrayBuildState *astate = NULL;
	ArrayType  *array;
	Datum	   *elems;
	int			nelems;
	int			i;
	ListCell   *lc;

	/* 1. Carry all non-timeseries "base" entries forward unchanged. */
	if (base != (Datum) 0 && DatumGetPointer(base) != NULL)
	{
		array = DatumGetArrayTypeP(base);
		deconstruct_array_builtin(array, TEXTOID, &elems, NULL, &nelems);
		for (i = 0; i < nelems; i++)
			astate = accumArrayResult(astate, elems[i], false, TEXTOID,
									  CurrentMemoryContext);
	}

	/* 2. Carry old timeseries.* entries that are not replaced/removed. */
	if (old_options != (Datum) 0 && DatumGetPointer(old_options) != NULL)
	{
		array = DatumGetArrayTypeP(old_options);
		deconstruct_array_builtin(array, TEXTOID, &elems, NULL, &nelems);
		for (i = 0; i < nelems; i++)
		{
			char	   *text_str = VARDATA(elems[i]);
			int			text_len = VARSIZE(elems[i]) - VARHDRSZ;

			if (!is_timeseries_option(text_str, text_len))
				continue;

			{
				const char *p = text_str + strlen(TS_RELOPT_PREFIX);
				const char *end = text_str + text_len;
				const char *eq = p;

				while (eq < end && *eq != '=')
					eq++;

				if (!timeseries_def_matches(defList, p, (int) (eq - p)))
					astate = accumArrayResult(astate, elems[i], false, TEXTOID,
											  CurrentMemoryContext);
			}
		}
	}

	/* 3. Append new timeseries.* options from defList (SET only). */
	if (!isReset)
	{
		foreach(lc, defList)
		{
			DefElem    *def = lfirst_node(DefElem, lc);
			const char *value;
			text	   *t;
			Size		len;

			if (def->defnamespace == NULL ||
				strcmp(def->defnamespace, "timeseries") != 0)
				continue;

			if (def->arg != NULL)
				value = defGetString(def);
			else
				value = "true";

			len = VARHDRSZ + strlen(TS_RELOPT_PREFIX) + strlen(def->defname) +
				1 + strlen(value);
			t = (text *) palloc(len + 1);
			SET_VARSIZE(t, len);
			sprintf(VARDATA(t), "%s%s=%s", TS_RELOPT_PREFIX, def->defname, value);

			astate = accumArrayResult(astate, PointerGetDatum(t), false, TEXTOID,
									  CurrentMemoryContext);
		}
	}

	if (astate == NULL)
		return (Datum) 0;

	return makeArrayResult(astate, CurrentMemoryContext);
}