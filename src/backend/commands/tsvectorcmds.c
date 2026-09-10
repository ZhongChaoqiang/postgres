#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/skey.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "commands/tsvectorcmds.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/value.h"
#include "parser/parse_coerce.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/*
 * Get the OID of the vector type.
 */
static Oid
get_vector_type_oid(void)
{
	return TypenameGetTypid("vector");
}

/*
 * Parse WITH options from the TsVectorStmt.
 */
static void
parse_tsvector_options(List *options, int *vector_len, char **scan_interval,
					   char **completion_delay)
{
	ListCell   *lc;

	*vector_len = 384;
	*scan_interval = pstrdup("5 min");
	*completion_delay = pstrdup("0 min");

	foreach(lc, options)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		if (strcmp(defel->defname, "vector_len") == 0)
			*vector_len = defGetInt64(defel);
		else if (strcmp(defel->defname, "scan_interval") == 0)
		{
			pfree(*scan_interval);
			*scan_interval = defGetString(defel);
		}
		else if (strcmp(defel->defname, "completion_delay") == 0)
		{
			pfree(*completion_delay);
			*completion_delay = defGetString(defel);
		}
	}
}

/*
 * Build a CREATE TABLE statement string for the vector table and execute it
 * via SPI.
 */
static void
create_vector_table(const char *vec_table_name, const char *source_table_name,
					List *carry_columns, const char *vector_column,
					int vector_len, Oid source_relid)
{
	StringInfo	query;
	StringInfo	pk_cols;
	ListCell   *lc;
	Oid			vector_typoid;
	char	   *source_schema;
	char	   *source_relname;
	Relation	rel;
	TupleDesc	tupdesc;

	vector_typoid = get_vector_type_oid();
	if (!OidIsValid(vector_typoid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type \"vector\" is not installed"),
				 errhint("Install the pgvector extension first: CREATE EXTENSION vector;")));

	/* Open source table to inspect carry column types */
	rel = relation_open(source_relid, AccessShareLock);
	tupdesc = RelationGetDescr(rel);
	source_schema = get_namespace_name(RelationGetNamespace(rel));
	source_relname = pstrdup(RelationGetRelationName(rel));

	query = makeStringInfo();
	pk_cols = makeStringInfo();

	appendStringInfo(query, "CREATE TABLE %s (", quote_identifier(vec_table_name));
	appendStringInfoString(query, "slice_start TIMESTAMPTZ NOT NULL");
	appendStringInfoString(query, ", slice_end TIMESTAMPTZ NOT NULL");

	appendStringInfoString(pk_cols, "slice_start");

	/* Add carry columns with types from source table */
	foreach(lc, carry_columns)
	{
		char	   *colname = strVal(lfirst(lc));
		AttrNumber	attnum;
		Form_pg_attribute attr;
		char	   *typname;

		attnum = get_attnum(source_relid, colname);
		if (attnum == InvalidAttrNumber)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" does not exist in source table \"%s\"",
							colname, source_relname)));

		attr = TupleDescAttr(tupdesc, attnum - 1);
		typname = format_type_with_typemod(attr->atttypid, attr->atttypmod);

		appendStringInfo(query, ", %s %s NOT NULL",
						 quote_identifier(colname), typname);
		appendStringInfo(pk_cols, ", %s", quote_identifier(colname));

		pfree(typname);
	}

	/* Add vector column */
	appendStringInfo(query, ", %s vector(%d)", quote_identifier(vector_column), vector_len);

	/* Add hidden metadata columns */
	appendStringInfoString(query, ", _processed BOOLEAN DEFAULT true");
	appendStringInfoString(query, ", _created_at TIMESTAMPTZ DEFAULT now()");

	/* Add primary key */
	appendStringInfo(query, ", PRIMARY KEY (%s)", pk_cols->data);

	appendStringInfoChar(query, ')');

	relation_close(rel, AccessShareLock);

	/* Execute CREATE TABLE via SPI */
	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not connect to SPI")));

	if (SPI_execute(query->data, false, 0) != SPI_OK_UTILITY)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("failed to create vector table: %s", vec_table_name)));

	SPI_finish();

	pfree(query->data);
	pfree(pk_cols->data);
	pfree(source_schema);
	pfree(source_relname);
}

/*
 * Create a vector index on the vector column.
 */
static void
create_vector_index(const char *vec_table_name, const char *vector_column)
{
	StringInfo	query;

	query = makeStringInfo();
	appendStringInfo(query,
					 "CREATE INDEX %s_%s_idx ON %s USING hnsw (%s vector_cosine_ops)",
					 quote_identifier(vec_table_name),
					 quote_identifier(vector_column),
					 quote_identifier(vec_table_name),
					 quote_identifier(vector_column));

	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not connect to SPI")));

	if (SPI_execute(query->data, false, 0) != SPI_OK_UTILITY)
		elog(NOTICE, "could not create vector index on %s (non-fatal)", vec_table_name);

	SPI_finish();
	pfree(query->data);
}

/*
 * Register metadata and background job.
 */
static void
register_tsvector_metadata(const char *vec_table_name, Oid source_relid,
						   const char *bucket_interval, const char *vector_column,
						   const char *vectorize_func, List *carry_columns,
						   int vector_len, const char *scan_interval,
						   const char *completion_delay)
{
	StringInfo	query;
	StringInfo	carry_str;
	ListCell   *lc;
	Oid			vec_relid;
	int			job_id = 0;

	/* Resolve vector table OID */
	vec_relid = get_relname_relid(vec_table_name, PG_PUBLIC_NAMESPACE);
	if (!OidIsValid(vec_relid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("vector table \"%s\" was not created", vec_table_name)));

	/* Build carry columns array string */
	carry_str = makeStringInfo();
	appendStringInfoString(carry_str, "{");
	foreach(lc, carry_columns)
	{
		char	   *colname = strVal(lfirst(lc));
		if (lc != list_head(carry_columns))
			appendStringInfoChar(carry_str, ',');
		appendStringInfoString(carry_str, colname);
	}
	appendStringInfoChar(carry_str, '}');

	/* Insert metadata */
	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not connect to SPI")));

	/* Create schema if not exists */
	query = makeStringInfo();
	appendStringInfo(query,
		"CREATE SCHEMA IF NOT EXISTS _timescaledb_internal");

	if (SPI_execute(query->data, false, 0) != SPI_OK_UTILITY)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("failed to create metadata schema")));

	/* Create metadata table if not exists */
	resetStringInfo(query);
	appendStringInfo(query,
		"CREATE TABLE IF NOT EXISTS _timescaledb_internal.timeseries_vector_tables ("
		"  id SERIAL PRIMARY KEY,"
		"  vector_table REGCLASS NOT NULL UNIQUE,"
		"  source_table REGCLASS NOT NULL,"
		"  bucket_interval TEXT NOT NULL,"
		"  vector_column TEXT NOT NULL,"
		"  vectorize_function TEXT NOT NULL,"
		"  carry_columns TEXT[] NOT NULL DEFAULT '{}',"
		"  vector_len INT NOT NULL DEFAULT 384,"
		"  scan_interval TEXT NOT NULL DEFAULT '5 min',"
		"  completion_delay TEXT NOT NULL DEFAULT '0 min',"
		"  job_id INTEGER,"
		"  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),"
		"  enabled BOOLEAN NOT NULL DEFAULT true)");

	if (SPI_execute(query->data, false, 0) != SPI_OK_UTILITY)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("failed to create metadata table")));

	/* Insert metadata record */
	resetStringInfo(query);
	appendStringInfo(query,
		"INSERT INTO _timescaledb_internal.timeseries_vector_tables "
		"(vector_table, source_table, bucket_interval, vector_column, "
		"vectorize_function, carry_columns, vector_len, scan_interval, completion_delay) "
		"VALUES (%s::regclass, %s::regclass, %s, %s, %s, %s, %d, %s, %s)",
		quote_literal_cstr(vec_table_name),
		quote_literal_cstr(get_rel_name(source_relid)),
		quote_literal_cstr(bucket_interval),
		quote_literal_cstr(vector_column),
		quote_literal_cstr(vectorize_func),
		quote_literal_cstr(carry_str->data),
		vector_len,
		quote_literal_cstr(scan_interval),
		quote_literal_cstr(completion_delay));

	if (SPI_execute(query->data, false, 0) != SPI_OK_INSERT)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("failed to insert metadata for vector table")));

	/*
	 * Try to register a TimescaleDB background job.
	 * If TimescaleDB is not available or the scan function doesn't exist,
	 * we skip silently.
	 */
	{
		Oid			scan_func_oid;
		Oid			add_job_oid;
		List	   *scan_func_name;

		/* Check if timeseries_vector_scan function exists */
		scan_func_name = list_make1(makeString((char *) "timeseries_vector_scan"));
		scan_func_oid = LookupFuncName(scan_func_name, 0, NULL, true);
		list_free(scan_func_name);

		/* Check if timescaledb_internal.add_job function exists */
		add_job_oid = LookupFuncName(list_make2(makeString((char *) "timescaledb_internal"),
												makeString((char *) "add_job")),
									 -1, NULL, true);

		if (OidIsValid(scan_func_oid) && OidIsValid(add_job_oid))
		{
			resetStringInfo(query);
			appendStringInfo(query,
				"SELECT timescaledb_internal.add_job("
				"  'timeseries_vector_scan'::regproc, "
				"  '%s'::interval, "
				"  jsonb_build_object("
				"    'vector_table', %s::regclass::text,"
				"    'source_table', %s::regclass::text,"
				"    'bucket_interval', %s,"
				"    'vector_column', %s,"
				"    'vectorize_function', %s,"
				"    'carry_columns', %s::jsonb,"
				"    'vector_len', %d,"
				"    'completion_delay', %s"
				"  ),"
				"  job_name => %s"
				")",
				scan_interval,
				quote_literal_cstr(vec_table_name),
				quote_literal_cstr(get_rel_name(source_relid)),
				quote_literal_cstr(bucket_interval),
				quote_literal_cstr(vector_column),
				quote_literal_cstr(vectorize_func),
				quote_literal_cstr(carry_str->data),
				vector_len,
				quote_literal_cstr(completion_delay),
				quote_literal_cstr(psprintf("tsv_%s", vec_table_name)));

			if (SPI_execute(query->data, true, 1) == SPI_OK_SELECT && SPI_processed > 0)
			{
				bool		isnull;
				Datum		val;

				val = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
				if (!isnull)
					job_id = DatumGetInt32(val);
			}
		}
		else
		{
			elog(NOTICE, "TimescaleDB job scheduling not available; "
						  "use timeseries_vector_run() for manual execution");
		}
	}

	/* Update job_id in metadata if we got one */
	if (job_id > 0)
	{
		resetStringInfo(query);
		appendStringInfo(query,
			"UPDATE _timescaledb_internal.timeseries_vector_tables "
			"SET job_id = %d WHERE vector_table = %s::regclass",
			job_id, quote_literal_cstr(vec_table_name));
		SPI_execute(query->data, false, 0);
	}

	SPI_finish();

	/*
	 * Note: query was allocated in the SPI context (after SPI_connect), so it
	 * has already been freed by SPI_finish().  Do not pfree(query->data).
	 * carry_str, however, was allocated before SPI_connect and must be freed.
	 */
	pfree(carry_str->data);
}

/*
 * Main entry point: CreateTsVectorTable
 * Creates a vector table from a source hypertable.
 */
void
CreateTsVectorTable(TsVectorStmt *stmt)
{
	Oid			source_relid;
	char	   *source_name;
	char	   *vec_table_name = stmt->vec_table_name;
	int			vector_len;
	char	   *scan_interval;
	char	   *completion_delay;
	Oid			vector_typoid;

	/* Resolve source table */
	source_relid = RangeVarGetRelidExtended(stmt->source_table,
											 ShareUpdateExclusiveLock,
											 0, RangeVarCallbackOwnsRelation, NULL);
	source_name = get_rel_name(source_relid);

	/* Check source table exists and user owns it */
	if (!object_ownercheck(RelationRelationId, source_relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE, source_name);

	/* Check if vector table already exists */
	if (get_relname_relid(vec_table_name, PG_PUBLIC_NAMESPACE) != InvalidOid)
	{
		if (stmt->if_not_exists)
		{
			ereport(NOTICE,
					(errmsg("vector table \"%s\" already exists, skipping",
							vec_table_name)));
			return;
		}
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("relation \"%s\" already exists", vec_table_name)));
	}

	/* Check vector type is available */
	vector_typoid = get_vector_type_oid();
	if (!OidIsValid(vector_typoid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type \"vector\" is not installed"),
				 errhint("Install the pgvector extension first: CREATE EXTENSION vector;")));

	/* Check vectorize function exists */
	if ( LookupExplicitNamespace("public", false) != InvalidOid)
	{
		Oid func_oid = LookupFuncName(list_make1(makeString((char *)stmt->vectorize_func)),
									  -1, NULL, true);	/* nargs=-1 表示不检查参数数量 */
		if (!OidIsValid(func_oid))
			elog(NOTICE, "vectorize function \"%s\" not found; "
						  "it must be created before running the scan", stmt->vectorize_func);
	}

	/* Parse options */
	parse_tsvector_options(stmt->options, &vector_len, &scan_interval,
						   &completion_delay);

	/* Validate carry columns exist in source table */
	if (stmt->carry_columns != NIL)
	{
		ListCell   *lc;
		foreach(lc, stmt->carry_columns)
		{
			char	   *colname = strVal(lfirst(lc));
			AttrNumber	attnum = get_attnum(source_relid, colname);
			if (attnum == InvalidAttrNumber)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("carry column \"%s\" does not exist in source table \"%s\"",
								colname, source_name)));
		}
	}

	/* Step 1: Create the vector table */
	create_vector_table(vec_table_name, source_name, stmt->carry_columns,
						stmt->vector_column, vector_len, source_relid);

	CommandCounterIncrement();

	/* Step 2: Create vector index */
	create_vector_index(vec_table_name, stmt->vector_column);

	CommandCounterIncrement();

	/* Step 3: Register metadata and background job */
	register_tsvector_metadata(vec_table_name, source_relid,
							   stmt->bucket_interval, stmt->vector_column,
							   stmt->vectorize_func, stmt->carry_columns,
							   vector_len, scan_interval, completion_delay);

	elog(NOTICE, "Timeseries vector table \"%s\" created successfully from \"%s\"",
		 vec_table_name, source_name);
}
