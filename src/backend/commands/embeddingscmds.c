#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
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
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "commands/trigger.h"
#include "commands/embeddingscmds.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/value.h"
#include "parser/parse_coerce.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "rewrite/rewriteHandler.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

Oid get_vector_type_oid(void);
void create_embeddings_trigger(Relation rel, const char *vecname,
							  const char *accessMethod,
							  List *embeddingsParams, List *options);
static void drop_embeddings_trigger(Relation rel, const char *vecname);

Oid
get_vector_type_oid(void)
{
	return TypenameGetTypid("vector");
}

void
CreateEmbeddings(EmbeddingsStmt *stmt)
{
	Oid			relid;
	Relation	rel;
	Oid			vector_typoid;
	AlterTableStmt *atstmt;
	AlterTableCmd *atcmd;
	ColumnDef  *coldef;
	int16		vector_len = 384;
	ListCell   *lc;

	relid = RangeVarGetRelidExtended(stmt->relation, ShareUpdateExclusiveLock,
									 0, RangeVarCallbackOwnsRelation, NULL);
	rel = table_open(relid, ShareUpdateExclusiveLock);

	if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE,
					   RelationGetRelationName(rel));

	if (get_attnum(relid, stmt->vecname) != InvalidAttrNumber)
	{
		if (stmt->if_not_exists)
		{
			ereport(NOTICE,
					(errmsg("embeddings column \"%s\" already exists on relation \"%s\", skipping",
							stmt->vecname, RelationGetRelationName(rel))));
			table_close(rel, ShareUpdateExclusiveLock);
			return;
		}
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" already exists",
						stmt->vecname, RelationGetRelationName(rel))));
	}

	foreach(lc, stmt->embeddingsParams)
	{
		char	   *colname;
		AttrNumber	attnum;

		if (!IsA(lfirst(lc), String))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("invalid column reference in EMBEDDINGS")));

		colname = strVal(lfirst(lc));
		attnum = get_attnum(relid, colname);

		if (attnum == InvalidAttrNumber)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" does not exist in relation \"%s\"",
							colname, RelationGetRelationName(rel))));
	}

	vector_typoid = get_vector_type_oid();
	if (!OidIsValid(vector_typoid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type \"vector\" is not installed"),
				 errhint("Install the pgvector extension first: CREATE EXTENSION vector;")));

	foreach(lc, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);
		if (strcmp(defel->defname, "vector_len") == 0)
		{
			vector_len = defGetInt64(defel);
		}
	}

	atstmt = makeNode(AlterTableStmt);
	atstmt->relation = stmt->relation;
	atstmt->cmds = NIL;
	atstmt->objtype = OBJECT_TABLE;

	coldef = makeNode(ColumnDef);
	coldef->colname = stmt->vecname;
	coldef->typeName = makeNode(TypeName);
	coldef->typeName->names = list_make1(makeString("vector"));
	{
		A_Const    *typmod_const = makeNode(A_Const);
		typmod_const->val.ival.type = T_Integer;
		typmod_const->val.ival.ival = vector_len;
		typmod_const->location = -1;
		coldef->typeName->typmods = list_make1(typmod_const);
	}
	coldef->typeName->typemod = -1;
	coldef->typeName->location = -1;
	coldef->inhcount = 0;
	coldef->is_local = true;
	coldef->is_not_null = false;
	coldef->is_from_type = false;
	coldef->is_predict = false;
	coldef->is_embedding = false;
	coldef->is_embeddings = true;
	coldef->is_hidden = true;
	coldef->storage = 0;
	coldef->raw_default = NULL;
	coldef->cooked_default = NULL;
	coldef->identity = '\0';
	coldef->generated = ATTRIBUTE_GENERATED_EMBEDDINGS;
	coldef->collClause = NULL;
	coldef->collOid = InvalidOid;
	coldef->constraints = NIL;
	coldef->fdwoptions = NIL;
	coldef->location = -1;

	atcmd = makeNode(AlterTableCmd);
	atcmd->subtype = AT_AddColumn;
	atcmd->def = (Node *) coldef;
	atcmd->missing_ok = false;
	atcmd->behavior = DROP_RESTRICT;

	atstmt->cmds = lappend(atstmt->cmds, atcmd);

	AlterTableInternal(relid, atstmt->cmds, false);

	CommandCounterIncrement();

	create_embeddings_trigger(rel, stmt->vecname, stmt->accessMethod,
							 stmt->embeddingsParams, stmt->options);

	table_close(rel, ShareUpdateExclusiveLock);
}

void
create_embeddings_trigger(Relation rel, const char *vecname,
						 const char *accessMethod,
						 List *embeddingsParams, List *options)
{
	CreateTrigStmt *tgstmt;
	StringInfoData args_buf;
	ListCell   *lc;
	List	   *tg_args = NIL;
	char	   *tgname;
	Oid			embeddings_trigger_oid;
	int			fgc_flags;
	FuncCandidateList clist;
	List	   *namelist;

	namelist = list_make2(makeString("pg_catalog"), makeString("embeddings_trigger"));
	clist = FuncnameGetCandidates(namelist, 0, NIL, false, false, false, true, &fgc_flags);
	if (clist)
		embeddings_trigger_oid = clist->oid;
	else
		embeddings_trigger_oid = InvalidOid;

	if (!OidIsValid(embeddings_trigger_oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function embeddings_trigger() not found")));

	tgname = psprintf("embeddings_%s_trigger", vecname);

	initStringInfo(&args_buf);
	appendStringInfo(&args_buf, "%s", vecname);
	tg_args = lappend(tg_args, makeString(pstrdup(args_buf.data)));

	appendStringInfo(&args_buf, ",%s", accessMethod);
	tg_args = lappend(tg_args, makeString(pstrdup(accessMethod)));

	resetStringInfo(&args_buf);
	foreach(lc, embeddingsParams)
	{
		char	   *colname = strVal(lfirst(lc));
		if (lc != list_head(embeddingsParams))
			appendStringInfoChar(&args_buf, ',');
		appendStringInfo(&args_buf, "%s", colname);
	}
	tg_args = lappend(tg_args, makeString(pstrdup(args_buf.data)));

	resetStringInfo(&args_buf);
	foreach(lc, options)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);
		if (lc != list_head(options))
			appendStringInfoChar(&args_buf, ',');
		appendStringInfo(&args_buf, "%s=%s", defel->defname,
						 defGetString(defel) ? defGetString(defel) : "true");
	}
	if (options != NIL)
		tg_args = lappend(tg_args, makeString(pstrdup(args_buf.data)));

	pfree(args_buf.data);

	tgstmt = makeNode(CreateTrigStmt);
	tgstmt->trigname = tgname;
	tgstmt->relation = makeRangeVar(get_namespace_name(RelationGetNamespace(rel)),
									 pstrdup(RelationGetRelationName(rel)), -1);
	tgstmt->funcname = list_make2(makeString("pg_catalog"), makeString("embeddings_trigger"));
	tgstmt->args = tg_args;
	tgstmt->row = true;
	tgstmt->timing = TRIGGER_TYPE_BEFORE;
	tgstmt->events = TRIGGER_TYPE_INSERT | TRIGGER_TYPE_UPDATE;
	tgstmt->columns = NIL;
	tgstmt->whenClause = NULL;
	tgstmt->isconstraint = false;
	tgstmt->deferrable = false;
	tgstmt->initdeferred = false;
	tgstmt->constrrel = NULL;
	tgstmt->transitionRels = NIL;

	CreateTrigger(tgstmt, NULL, RelationGetRelid(rel), InvalidOid, InvalidOid,
				  InvalidOid, InvalidOid, InvalidOid, NULL, true, false);

	CommandCounterIncrement();
}

void
DropEmbeddings(DropEmbeddingsStmt *stmt)
{
	Oid			relid;
	Relation	rel;
	AttrNumber	attnum;
	AlterTableStmt *atstmt;
	AlterTableCmd *atcmd;

	relid = RangeVarGetRelidExtended(stmt->relation, ShareUpdateExclusiveLock,
									 0, RangeVarCallbackOwnsRelation, NULL);
	rel = table_open(relid, ShareUpdateExclusiveLock);

	if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE,
					   RelationGetRelationName(rel));

	attnum = get_attnum(relid, stmt->vecname);
	if (attnum == InvalidAttrNumber)
	{
		if (stmt->if_exists)
		{
			ereport(NOTICE,
					(errmsg("embeddings column \"%s\" does not exist on relation \"%s\", skipping",
							stmt->vecname, RelationGetRelationName(rel))));
			table_close(rel, ShareUpdateExclusiveLock);
			return;
		}
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						stmt->vecname, RelationGetRelationName(rel))));
	}

	drop_embeddings_trigger(rel, stmt->vecname);

	atstmt = makeNode(AlterTableStmt);
	atcmd = makeNode(AlterTableCmd);

	atstmt->relation = stmt->relation;
	atstmt->objtype = OBJECT_TABLE;
	atstmt->cmds = NIL;

	atcmd->subtype = AT_DropColumn;
	atcmd->name = stmt->vecname;
	atcmd->missing_ok = stmt->if_exists;
	atcmd->behavior = DROP_RESTRICT;

	atstmt->cmds = lappend(atstmt->cmds, atcmd);

	AlterTableInternal(relid, atstmt->cmds, false);

	table_close(rel, ShareUpdateExclusiveLock);
}

static void
drop_embeddings_trigger(Relation rel, const char *vecname)
{
	char	   *tgname_prefix;
	Relation	pg_trigger;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	tuple;
	bool		found = false;

	tgname_prefix = psprintf("embeddings_%s_trigger", vecname);

	pg_trigger = table_open(TriggerRelationId, AccessShareLock);

	ScanKeyInit(&key,
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));

	scan = systable_beginscan(pg_trigger, TriggerRelidNameIndexId,
							  true, NULL, 1, &key);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_trigger trigform = (Form_pg_trigger) GETSTRUCT(tuple);

		if (strncmp(NameStr(trigform->tgname), tgname_prefix,
					strlen(tgname_prefix)) == 0)
		{
			ObjectAddress trigobj;

			trigobj.classId = TriggerRelationId;
			trigobj.objectId = trigform->oid;
			trigobj.objectSubId = 0;

			systable_endscan(scan);
			table_close(pg_trigger, AccessShareLock);

			performDeletion(&trigobj, DROP_RESTRICT, 0);
			CommandCounterIncrement();

			found = true;
			break;
		}
	}

	if (!found)
	{
		systable_endscan(scan);
		table_close(pg_trigger, AccessShareLock);
	}

	pfree(tgname_prefix);
}
