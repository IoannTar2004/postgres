#include <string.h>
#include "postgres.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "executor/jsonb_hot.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "nodes/execnodes.h"
#include "utils/rel.h"

#define JSONB_SET_FUNC_ID 3305
#define JSONB_OBJECT_FIELD_TEXT_ID 3214

static char* get_jsonb_path_string(Const* const_object);
static void try_to_modify_hotattrs(List* modify_paths, Const* aConst, Bitmapset* hotattrs);

JsonbUpdatePathsInfo* jsonb_update_paths_checks(List* plan, Oid oid) {
    JsonbUpdatePathsInfo* jbInfo = palloc(sizeof(JsonbUpdatePathsInfo));
    jbInfo->args = NIL;
    jbInfo->checked = true;

    ListCell* lc;

    foreach(lc, plan) {
        TargetEntry* targetEntry = lfirst(lc);
        if (targetEntry->expr->type == T_FuncExpr) {
            FuncExpr* funcExpr = (FuncExpr*) targetEntry->expr;

            if (funcExpr->funcid == JSONB_SET_FUNC_ID) {
                Node* optional_const = (Node*) lsecond(funcExpr->args);

                if (optional_const->type == T_Const) {
                    char* val = get_jsonb_path_string((Const*) optional_const);
                    JsonbUpdatePaths *new_jsonb = palloc(sizeof(JsonbUpdatePaths));
                    new_jsonb->paths = val;
                    new_jsonb->colnum = get_attnum(oid, targetEntry->resname);
                    jbInfo->args = lappend(jbInfo->args, new_jsonb);
                }

            }
        }
    }
    if (jbInfo->args)
        jbInfo->checked = false;

    return jbInfo;
}

static char* get_jsonb_path_string(Const* const_object) {
    ArrayType* datum = DatumGetArrayTypeP(const_object->constvalue);
    return text_to_cstring((text*) ARR_DATA_PTR(datum));
}

void compare_paths_and_indexes(JsonbUpdatePathsInfo* jbInfo, ResultRelInfo* relInfo) {
    if (!relInfo->ri_IndexRelationDescs)
        return;

    for (int i = 0; i < relInfo->ri_NumIndices; ++i) {
        List* indexprs = relInfo->ri_IndexRelationDescs[i]->rd_indexprs;
        if (!indexprs)
            continue;

        ListCell* lc;
        foreach(lc, indexprs) {
            OpExpr* opExpr = lfirst(lc);
            if (opExpr->opfuncid != JSONB_OBJECT_FIELD_TEXT_ID)
                continue;

            Node* optional_const = (Node*) lsecond(opExpr->args);
            if (optional_const->type == T_Const) {
                try_to_modify_hotattrs(jbInfo->args, (Const*) optional_const, relInfo->ri_RelationDesc->rd_hotblockingattr);
            }
        }
    }

    jbInfo->checked = true;
}

static void try_to_modify_hotattrs(List* modify_paths, Const* aConst, Bitmapset* hotattrs) {
    if (!hotattrs)
        return;

    char* index_path = text_to_cstring(DatumGetPointer(aConst->constvalue));
    ListCell* lc;
    foreach(lc, modify_paths) {
        JsonbUpdatePaths* path = (JsonbUpdatePaths*) lfirst(lc);
        if (strcmp(index_path, path->paths)) {
            hotattrs = bms_del_member(hotattrs, path->colnum + 7);
        }
    }
}