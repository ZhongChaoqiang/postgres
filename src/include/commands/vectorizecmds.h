#ifndef VECTORIZECMDS_H
#define VECTORIZECMDS_H

#include "nodes/parsenodes.h"

extern void CreateVectorize(EmbeddingsStmt *stmt);
extern void DropVectorize(DropEmbeddingsStmt *stmt);

#endif
