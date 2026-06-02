#ifndef EMBEDDINGSCMDS_H
#define EMBEDDINGSCMDS_H

#include "nodes/parsenodes.h"
#include "utils/relcache.h"

extern void CreateEmbeddings(EmbeddingsStmt *stmt);
extern void DropEmbeddings(DropEmbeddingsStmt *stmt);
extern void create_embeddings_trigger(Relation rel, const char *vecname,
									 const char *accessMethod,
									 List *embeddingsParams, List *options);
extern Oid get_vector_type_oid(void);

#endif
