#ifndef POSTGRES_JSONB_HOT_H
#define POSTGRES_JSONB_HOT_H

#include "nodes/execnodes.h"
#include "nodes/pg_list.h"

typedef struct JsonbUpdatePaths {
    char* paths;
    int colnum;
} JsonbUpdatePaths;

/*
 * A special object that consists jsonb paths to update.
 * Also includes a flag to prevent extra checks in each update.
 */
typedef struct JsonbUpdatePathsInfo {
    List* args;
    bool checked;
} JsonbUpdatePathsInfo;

JsonbUpdatePathsInfo* jsonb_update_paths_checks(List* plan, Oid oid);
void compare_paths_and_indexes(JsonbUpdatePathsInfo* jbInfo, struct ResultRelInfo* relInfo);

#endif
