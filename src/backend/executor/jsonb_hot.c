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
#include "nodes/nodeFuncs.h"

#define JSONB_OBJECT_FIELD_TEXT_ID 3214
#define JSONB_ARRAY_ELEMENT 3215
#define JSONB_ARRAY_ELEMENT_TEXT 3216
#define JSONB_EXTRACT_PATH 3217
#define JSONB_OBJECT_FIELD 3478
#define JSONB_EXTRACT_PATH_TEXT 3940

#define jsonb_update_path_init(var, colnum) \
    JsonbUpdatePaths* var = palloc(sizeof (JsonbUpdatePaths)); \
    new_path->path = NIL;                                           \
    new_path->attnum = colnum;

typedef struct {
    bool is_valid_func;
    List* jsonb_paths;
} JsonbIndexCtx;

static bool parse_jsonb_update_path(JsonbUpdatePathsInfo* jbInfo, int attnum, Oid type, List* args);
static List* get_jsonb_update_path(int attnum, Const* const_object);
static List* jsonb_deconstruct_array(int attnum, Const* const_object);
static List* extract_jsonboid_update_paths(Jsonb* jb, int attnum);
static bool jsonb_index_path_walker(Node* node, void* ctx);
static bool is_valid_expression(Oid id);
static bool parse_jsonb_index_path(List** jbPaths, Node* node);
static void set_list_with_null(List** paths);
static void extract_jsonb_index_path(List** jbPaths, Node* opExpr, MemoryContext memoryContext);
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

static bool parse_jsonb_update_path(JsonbUpdatePathsInfo* jbInfo, int attnum, Oid type, List* args) {
    ListCell* lc;
    int arg = 0;
    bool state = true;

    foreach(lc, args) {
        Node* node = lfirst(lc);
        if (type == JSONBOID) {
            if (arg == 1) {
                if (IsA(node, Const)) {
                    Const* const_object = (Const*) node;
                    jbInfo->args = list_concat(jbInfo->args, get_jsonb_update_path(attnum, const_object));
                } else {
                    list_free_deep(jbInfo->args);
                    jbInfo->args = NIL;
                    return false;
                }
                break;
            }
        }

        if (IsA(node, FuncExpr)) {
            FuncExpr* funcExpr = (FuncExpr*) node;
            state = parse_jsonb_update_path(jbInfo, attnum, funcExpr->funcresulttype, funcExpr->args);

        } else if (IsA(node, OpExpr)) {
            OpExpr* opExpr = (OpExpr*) node;
            state = parse_jsonb_update_path(jbInfo, attnum, opExpr->opresulttype, opExpr->args);
        }

        if (!state)
            return false;

        arg++;
    }

    return true;
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
        jsonb_update_path_init(new_path, attnum)
        new_path->path = jsonb_deconstruct_array(attnum, const_object);
        paths = lappend(paths, new_path);
    }

    return paths;
}

static List* jsonb_deconstruct_array(int attnum, Const* const_object) {
    List* paths = NIL;

    ArrayType *arr = DatumGetArrayTypeP(const_object->constvalue);
    Datum  *elems;
    bool   *nulls;
    int     nelems;

    deconstruct_array(arr,
                      TEXTOID,
                      -1, false, 'i',
                      &elems, &nulls, &nelems);

    for (int i = 0; i < nelems; i++) {
        if (!nulls[i]) {
            char *str = TextDatumGetCString(elems[i]);
            paths = lappend(paths, str);
        }
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
            Node* opExpr = lfirst(lc);

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

static void set_list_with_null(List** paths) {
    list_free_deep(*paths);
    *paths = NIL;
    *paths = lappend(*paths, NULL);
}

static void extract_jsonb_index_path(List** jbPaths, Node* node, MemoryContext memoryContext) {
    MemoryContext oldctx;

    oldctx = MemoryContextSwitchTo(memoryContext);


    JsonbIndexCtx jsonbIndexCtx;
    List* paths = NIL;
    jsonbIndexCtx.jsonb_paths = paths;

    if (IsA(node, OpExpr))
        jsonbIndexCtx.is_valid_func = is_valid_expression(((OpExpr*) node)->opfuncid);
    else if (IsA(node, FuncExpr))
        jsonbIndexCtx.is_valid_func = is_valid_expression(((FuncExpr*) node)->funcid);
    else
        jsonbIndexCtx.is_valid_func = false;

    if (jsonbIndexCtx.is_valid_func)
        parse_jsonb_index_path(&jsonbIndexCtx.jsonb_paths, node);
    else
        expression_tree_walker(node, jsonb_index_path_walker, &jsonbIndexCtx);

    *jbPaths = jsonbIndexCtx.jsonb_paths;

    MemoryContextSwitchTo(oldctx);
}

static bool is_valid_expression(Oid id) {
    switch (id) {
        case JSONB_OBJECT_FIELD_TEXT_ID:
        case JSONB_ARRAY_ELEMENT:
        case JSONB_ARRAY_ELEMENT_TEXT:
        case JSONB_OBJECT_FIELD:
        case JSONB_EXTRACT_PATH:
        case JSONB_EXTRACT_PATH_TEXT:
            return true;
        default:
            return false;
    }
}

static bool jsonb_index_path_walker(Node* node, void* ctx) {
    JsonbIndexCtx* jsonbIndexCtx = (JsonbIndexCtx*) ctx;

    if (IsA(node, Var)) {
        Var* nodeVar = (Var*) node;
        if (nodeVar->vartype == JSONBOID) {
            set_list_with_null(&jsonbIndexCtx->jsonb_paths);
            return true;
        }
    }

    if (IsA(node, FuncExpr) && is_valid_expression(((FuncExpr*) node)->funcid) ||
            IsA(node, OpExpr) && is_valid_expression(((OpExpr*) node)->opfuncid)) {

        return !parse_jsonb_index_path(&jsonbIndexCtx->jsonb_paths, node);
    }

    return expression_tree_walker(node, jsonb_index_path_walker, jsonbIndexCtx);
}

static bool parse_jsonb_index_path(List** jbPaths, Node* node) {
    JsonbUpdatePaths* jsonbUpdatePaths = palloc(sizeof (JsonbUpdatePaths));
    jsonbUpdatePaths->path = NIL;
    Node* current = node;

    while (IsA(current, OpExpr) || IsA(current, FuncExpr)) {
        List* args = IsA(current, OpExpr) ? ((OpExpr*) current)->args : ((FuncExpr*) current)->args;
        Oid funcid = IsA(current, OpExpr) ? ((OpExpr*) current)->opfuncid : ((FuncExpr*) current)->funcid;

        if (!is_valid_expression(funcid)) {
            set_list_with_null(jbPaths);
            return false;
        }

        Node* left = linitial(args);
        if (IsA(left, Var)) {
            Var* var = (Var*) left;
            jsonbUpdatePaths->attnum = var->varattno;
        }
        Node* right = lsecond(args);
        if (IsA(right, Const)) {
            Const* c = (Const*) right;
            if (c->consttype == TEXTARRAYOID)
                jsonbUpdatePaths->path = jsonb_deconstruct_array(jsonbUpdatePaths->attnum, c);
            else if (c->consttype == TEXTOID) {
                char* key = TextDatumGetCString(c->constvalue);
                jsonbUpdatePaths->path = lcons(key, jsonbUpdatePaths->path);
            }
        }
        current = linitial(args);
    }

    (*jbPaths) = lappend(*jbPaths, jsonbUpdatePaths);
    return true;
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
    if (linitial(indexes_path) == NULL)
        return true;

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