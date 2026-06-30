#ifndef POSTGRES_JSONB_HOT_H
#define POSTGRES_JSONB_HOT_H

#include "nodes/execnodes.h"
#include "nodes/pg_list.h"
#include "nodes/bitmapset.h"

typedef struct JsonbKey {
    List* key;
    int attnum;
} JsonbKey;

/*
 * A special object that consists jsonb paths to update.
 * Also includes a flag to prevent extra checks in each update.
 */
typedef struct JsonbUpdateKeysInfo {
    List* args;
    Bitmapset* bitmapset;
    bool checked;
} JsonbUpdateKeysInfo;

JsonbUpdateKeysInfo* jsonb_update_keys_extract(List* plan, Oid oid);
void compare_modified_and_indexed_keys(JsonbUpdateKeysInfo* jbInfo, struct ResultRelInfo *relInfo);

#endif
