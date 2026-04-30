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
#include "utils/jsonb.h"
#include "catalog/pg_type_d.h"
#include "lib/stringinfo.h"
#include "nodes/nodes.h"

#define JSONB_OBJECT_FIELD_TEXT_ID 3214
#define jsonb_update_path_init(var, colnum) \
    JsonbUpdatePaths* var = palloc(sizeof (JsonbUpdatePaths)); \
    new_path->path = NIL;                                           \
    new_path->attnum = colnum;

static void parse_jsonb_update_path(JsonbUpdatePathsInfo* jbInfo, int attnum, Oid type, List* args);
static List* get_jsonb_update_path(int attnum, Const* const_object);
static List* extract_jsonboid_update_paths(Jsonb* jb, int attnum);
static void parse_jsonb_index_path(List** jbPaths, Node* node);
static void extract_jsonb_index_path(List** jbPaths, OpExpr* opExpr, MemoryContext memoryContext);
static bool check_key_in_index(List* modify_columns, List* indexes_paths, Bitmapset** bitmapset, int* attnum);
static bool is_key_in_index(List* indexes_path, List* modify_path, int attnum);

JsonbUpdatePathsInfo* jsonb_update_paths_checks(List* plan, Oid oid) {
    JsonbUpdatePathsInfo* jbInfo = palloc(sizeof(JsonbUpdatePathsInfo));
    jbInfo->args = NIL;
    jbInfo->bitmapset = NULL;
    jbInfo->checked = true;

    ListCell* lc;

    foreach(lc, plan) {
        TargetEntry* targetEntry = lfirst(lc);
        int attnum = get_attnum(oid, targetEntry->resname);

        if (IsA(targetEntry->expr, OpExpr)) {
            OpExpr* opExpr = (OpExpr*) targetEntry->expr;
            parse_jsonb_update_path(jbInfo, attnum, opExpr->opresulttype, opExpr->args);

        } else if (IsA(targetEntry->expr, FuncExpr)) {
            FuncExpr* funcExpr = (FuncExpr*) targetEntry->expr;
            parse_jsonb_update_path(jbInfo, attnum, funcExpr->funcresulttype, funcExpr->args);
        }
    }
    if (jbInfo->args)
        jbInfo->checked = false;

    return jbInfo;
}

static void parse_jsonb_update_path(JsonbUpdatePathsInfo* jbInfo, int attnum, Oid type, List* args) {
    ListCell* lc;
    foreach(lc, args) {
        Node* node = lfirst(lc);
        if (type == JSONBOID && IsA(node, Const)) {
            Const* const_object = (Const*) node;
            jbInfo->args = list_concat(jbInfo->args, get_jsonb_update_path(attnum, const_object));
            break;
        }
        if (IsA(node, FuncExpr)) {
            FuncExpr* funcExpr = (FuncExpr*) node;
            parse_jsonb_update_path(jbInfo, attnum, funcExpr->funcresulttype, funcExpr->args);

        } else if (IsA(node, OpExpr)) {
            OpExpr* opExpr = (OpExpr*) node;
            parse_jsonb_update_path(jbInfo, attnum, opExpr->opresulttype, opExpr->args);
        }
    }
}

static List* get_jsonb_update_path(int attnum, Const* const_object) {
    List* paths = NIL;

    if (const_object->consttype == TEXTOID) {
        char* key = text_to_cstring(DatumGetPointer(const_object->constvalue));
        jsonb_update_path_init(new_path, attnum)
        new_path->path = lappend(new_path->path, key);
        paths = lappend(paths, new_path);

    } else if (const_object->consttype == JSONBOID) {
        Jsonb *jb = DatumGetJsonbP(const_object->constvalue);
        paths = extract_jsonboid_update_paths(jb, attnum);

    } else {
        ArrayType *arr = DatumGetArrayTypeP(const_object->constvalue);
        Datum  *elems;
        bool   *nulls;
        int     nelems;

        deconstruct_array(arr,
                          TEXTOID,
                          -1, false, 'i',
                          &elems, &nulls, &nelems);

        jsonb_update_path_init(new_path, attnum)

        for (int i = 0; i < nelems; i++) {
            if (!nulls[i]) {
                char *str = TextDatumGetCString(elems[i]);
                new_path->path = lappend(new_path->path, str);
            }
        }

        paths = lappend(paths, new_path);
    }

    return paths;
}

static List* extract_jsonboid_update_paths(Jsonb* jb, int attnum) {
    JsonbIterator* it = JsonbIteratorInit(&jb->root);
    JsonbValue v;
    int r;

    List* jbPaths = NIL;
    List* current_path = NIL;

    while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE) {
        if (r == WJB_KEY) {
            char *key = pnstrdup(v.val.string.val, v.val.string.len);
            current_path = lappend(current_path, key);
        }
        else if (r == WJB_VALUE) {
            jsonb_update_path_init(new_path, attnum)
            new_path->path = current_path;
            jbPaths = lappend(jbPaths, new_path);
            list_free_deep(current_path);
            current_path = NIL;
        }
    }

    return jbPaths;
}

void compare_paths_and_indexes(JsonbUpdatePathsInfo* jbInfo, ResultRelInfo* relInfo) {
    if (!relInfo->ri_IndexRelationDescs || !jbInfo)
        return;

    Bitmapset* bitmapset = NULL;
    bool is_key_in_index = false;
    int attnum;

    for (int i = 0; i < relInfo->ri_NumIndices; ++i) {
        Relation index_relation_desc = relInfo->ri_IndexRelationDescs[i];
        List* indexprs = index_relation_desc->rd_indexprs;
        if (!indexprs)
            continue;

        ListCell* lc;
        foreach(lc, indexprs) {
            OpExpr* opExpr = lfirst(lc);

            if (index_relation_desc->rd_jsonbIndexPathsInfo == NULL) {
                extract_jsonb_index_path(&index_relation_desc->rd_jsonbIndexPathsInfo,
                                         opExpr,
                                         index_relation_desc->rd_indexcxt);
            }
            if (check_key_in_index(jbInfo->args, index_relation_desc->rd_jsonbIndexPathsInfo, &bitmapset, &attnum)) {
                is_key_in_index = true;
                break;
            }
        }

        if (is_key_in_index)
            break;
    }

    jbInfo->checked = true;
    jbInfo->bitmapset = is_key_in_index ? NULL : bms_add_member(bitmapset, attnum + 7);
}

static void extract_jsonb_index_path(List** jbPaths, OpExpr* opExpr, MemoryContext memoryContext) {
    MemoryContext oldctx;

    oldctx = MemoryContextSwitchTo(memoryContext);

    List* index_jsonb_paths = NIL;
    parse_jsonb_index_path(&index_jsonb_paths, opExpr);
    *jbPaths = index_jsonb_paths;

    MemoryContextSwitchTo(oldctx);
}

static void parse_jsonb_index_path(List** jbPaths, Node* node) {
    if (!IsA(node, OpExpr))
        return;

    OpExpr* op = (OpExpr*) node;
    ListCell* lc;
    if (op->opfuncid != JSONB_OBJECT_FIELD_TEXT_ID) {
        foreach(lc, op->args) {
            Node* op_node = lfirst(lc);
            parse_jsonb_index_path(jbPaths, op_node);
        }
        return;
    }

    StringInfoData buf;
    initStringInfo(&buf);
    List* buf_list = NIL;
    int attnum;

    while (IsA(op, OpExpr)) {
        Node* left = linitial(op->args);
        if (IsA(left, Var)) {
            Var* var = (Var*) left;
            attnum = var->varattno;
        }
        Node* right = lsecond(op->args);
        if (IsA(right, Const)) {
            Const* c = (Const*) right;
            char* key = TextDatumGetCString(c->constvalue);
            buf_list = lcons(key, buf_list);
        }
        op = linitial(op->args);
    }
    JsonbUpdatePaths* jsonbUpdatePaths = palloc(sizeof (JsonbUpdatePaths));
    jsonbUpdatePaths->path = buf_list;
    jsonbUpdatePaths->attnum = attnum;

    (*jbPaths) = lappend(*jbPaths, jsonbUpdatePaths);
}

static bool check_key_in_index(List* modify_columns, List* indexes_paths, Bitmapset** bitmapset, int* attnum) {
    ListCell* lc;
    foreach(lc, modify_columns) {
        JsonbUpdatePaths* jbPaths = (JsonbUpdatePaths*) lfirst(lc);


        if (is_key_in_index(indexes_paths, jbPaths->path, jbPaths->attnum))
            return true;

        *attnum = jbPaths->attnum;
    }

    return false;
}

static bool is_key_in_index(List* indexes_path, List* modify_path, int attnum) {
    ListCell* lc;
    foreach(lc, indexes_path) {
        JsonbUpdatePaths* path = lfirst(lc);
        if (path->attnum != attnum)
            continue;

        ListCell* lc1;
        ListCell* lc2;

        bool key_in_index = true;

        forboth(lc1, path->path, lc2, modify_path) {
            char* index_key = (char*) lfirst(lc1);
            char* modified_key = (char*) lfirst(lc2);
            if (strcmp(index_key, modified_key)) {
                key_in_index = false;
                break;
            }
        }

        if (key_in_index)
            return true;
    }

    return false;
}