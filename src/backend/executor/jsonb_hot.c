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

// Allowed JSONB Function in update operations
#define JSONB_SET 3305
#define JSONB_SET_LAX 5054
#define JSONB_DELETE_PATH 3304
#define JSONB_DELETE 3302
#define JSONB_CONCAT 3301
#define JSONB_INSERT 3579

// Allowed JSONB Functions in index expressions
#define JSONB_OBJECT_FIELD_TEXT_ID 3214
#define JSONB_ARRAY_ELEMENT 3215
#define JSONB_ARRAY_ELEMENT_TEXT 3216
#define JSONB_EXTRACT_PATH 3217
#define JSONB_OBJECT_FIELD 3478
#define JSONB_EXTRACT_PATH_TEXT 3940

#define jsonb_update_key_init(var, colnum) \
    JsonbKey* var = palloc(sizeof (JsonbKey)); \
    var->key = NIL;                                           \
    var->attnum = colnum;

static bool parse_jsonb_update_key(JsonbUpdateKeysInfo* jbInfo, int attnum, Oid id, List* args);
static bool is_valid_update_function(Oid id);
static List* get_jsonb_update_key(int attnum, Const* const_object);
static List* jsonb_deconstruct_array(int attnum, Const* const_object);
static List* extract_jsonboid_update_keys(Jsonb* jb, int attnum);
static bool jsonb_index_key_walker(Node* node, void* ctx);
static bool is_valid_expression(Oid id);
static bool parse_jsonb_index_key(List** jbKeys, Node* node);
static bool column_is_index(List* jbKeys, Relation relation);
static void set_list_with_null(List** keys);
static void extract_jsonb_index_key(List** jbKeys, Node* node, MemoryContext memoryContext, ResultRelInfo* resultRelInfo);
static bool check_key_in_index(List* modify_columns, List* indexes_keys, Bitmapset** bitmapset);
static bool is_key_in_index(List* indexes_key, List* modify_key, int attnum);

JsonbUpdateKeysInfo* jsonb_update_keys_checks(List* plan, Oid oid) {
    JsonbUpdateKeysInfo* jbInfo = palloc(sizeof(JsonbUpdateKeysInfo));
    jbInfo->args = NIL;
    jbInfo->bitmapset = NULL;
    jbInfo->checked = true;

    ListCell* lc;

    foreach(lc, plan) {
        TargetEntry* targetEntry = lfirst(lc);
        int attnum = get_attnum(oid, targetEntry->resname);

        if (IsA(targetEntry->expr, OpExpr)) {
            OpExpr* opExpr = (OpExpr*) targetEntry->expr;
            parse_jsonb_update_key(jbInfo, attnum, opExpr->opfuncid, opExpr->args);

        } else if (IsA(targetEntry->expr, FuncExpr)) {
            FuncExpr* funcExpr = (FuncExpr*) targetEntry->expr;
            parse_jsonb_update_key(jbInfo, attnum, funcExpr->funcid, funcExpr->args);
        }
    }
    if (jbInfo->args)
        jbInfo->checked = false;

    return jbInfo;
}

static bool is_valid_update_function(Oid id) {
    switch (id) {
        case JSONB_SET:
        case JSONB_SET_LAX:
        case JSONB_DELETE:
        case JSONB_DELETE_PATH:
        case JSONB_CONCAT:
        case JSONB_INSERT:
            return true;

        default:
            return false;
    }
}

static bool parse_jsonb_update_key(JsonbUpdateKeysInfo* jbInfo, int attnum, Oid id, List* args) {
    if (!is_valid_update_function(id))
        return false;

    ListCell* lc;
    int arg = 0;
    bool state = true;

    foreach(lc, args) {
        Node* node = lfirst(lc);
        if (IsA(node, Var)) {
            Var* var = (Var*) node;
            if (var->varattno != attnum) {
                list_free_deep(jbInfo->args);
                jbInfo->args = NIL;
                return false;
            }

        } else if (arg == 1) {
            if (IsA(node, Const)) {
                Const* const_object = (Const*) node;
                jbInfo->args = list_concat(jbInfo->args, get_jsonb_update_key(attnum, const_object));
                break;
            } else {
                list_free_deep(jbInfo->args);
                jbInfo->args = NIL;
                return false;
            }
        } else if (IsA(node, FuncExpr)) {
            FuncExpr* funcExpr = (FuncExpr*) node;
            state = parse_jsonb_update_key(jbInfo, attnum, funcExpr->funcid, funcExpr->args);
        } else if (IsA(node, OpExpr)) {
            OpExpr* opExpr = (OpExpr*) node;
            state = parse_jsonb_update_key(jbInfo, attnum, opExpr->opfuncid, opExpr->args);
        } else {
            return false;
        }

        if (!state)
            return false;

        arg++;
    }

    return true;
}

static List* get_jsonb_update_key(int attnum, Const* const_object) {
    List* keys = NIL;

    if (const_object->consttype == TEXTOID) {
        char* key = text_to_cstring(DatumGetPointer(const_object->constvalue));
        jsonb_update_key_init(new_key, attnum)
        new_key->key = lappend(new_key->key, key);
        keys = lappend(keys, new_key);

    } else if (const_object->consttype == JSONBOID) {
        Jsonb *jb = DatumGetJsonbP(const_object->constvalue);
        keys = extract_jsonboid_update_keys(jb, attnum);

    } else {
        jsonb_update_key_init(new_key, attnum)
        new_key->key = jsonb_deconstruct_array(attnum, const_object);
        keys = lappend(keys, new_key);
    }

    return keys;
}

static List* jsonb_deconstruct_array(int attnum, Const* const_object) {
    List* keys = NIL;

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
            keys = lappend(keys, str);
        }
    }

    return keys;
}

static List* extract_jsonboid_update_keys(Jsonb* jb, int attnum) {
    JsonbIterator* it = JsonbIteratorInit(&jb->root);
    JsonbValue v;
    int r;

    List* jbKeys = NIL;
    List* current_key = NIL;

    while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE) {
        if (r == WJB_KEY) {
            char *key = pnstrdup(v.val.string.val, v.val.string.len);
            current_key = lappend(current_key, key);
        }
        else if (r == WJB_VALUE) {
            jsonb_update_key_init(new_key, attnum)
            new_key->key = current_key;
            jbKeys = lappend(jbKeys, new_key);
            current_key = NIL;
        }
    }

    return jbKeys;
}

void compare_modified_and_indexed_keys(JsonbUpdateKeysInfo* jbInfo, ResultRelInfo* relInfo) {
    if (!relInfo->ri_IndexRelationDescs || !jbInfo)
        return;

    Bitmapset* bitmapset = NULL;
    bool is_key_in_index = true;

    for (int i = 0; i < relInfo->ri_NumIndices; ++i) {
        Relation index_relation_desc = relInfo->ri_IndexRelationDescs[i];
        List* indexprs = index_relation_desc->rd_indexprs;

        if (!indexprs) {
            if (column_is_index(jbInfo->args, relInfo->ri_IndexRelationDescs[i])) {
                is_key_in_index = true;
                break;
            }
            continue;
        }

        ListCell* lc;
        foreach(lc, indexprs) {
            is_key_in_index = false;
            Node* opExpr = lfirst(lc);

            if (index_relation_desc->rd_jsonbIndexKeysInfo == NULL) {
                extract_jsonb_index_key(&index_relation_desc->rd_jsonbIndexKeysInfo,
                                        opExpr,
                                        index_relation_desc->rd_indexcxt, relInfo);
            }

            List* index_list = index_relation_desc->rd_jsonbIndexKeysInfo;
            if (index_list != NULL &&
                linitial(index_list) == NULL || check_key_in_index(jbInfo->args, index_list, &bitmapset)) {
                is_key_in_index = true;
                break;
            }
        }

        if (is_key_in_index)
            break;
    }

    jbInfo->checked = true;
    jbInfo->bitmapset = is_key_in_index ? NULL : bitmapset;
}

static bool column_is_index(List* jbKeys, Relation relation) {
    ListCell* lc;
    foreach(lc, jbKeys) {
        JsonbKey *jbKey = lfirst(lc);
        int2vector* indkey = &relation->rd_index->indkey;
        if (indkey->dim1 == 1 && indkey->values[0] == 0)
            continue;

        for (int j = 0; j < indkey->dim1; ++j) {
            if (jbKey->attnum == indkey->values[j]) {
                return true;
            }
        }
    }

    return false;
}

static void set_list_with_null(List** keys) {
    list_free_deep(*keys);
    *keys = NIL;
    *keys = lappend(*keys, NULL);
}

static void extract_jsonb_index_key(List** jbKeys, Node* node, MemoryContext memoryContext, ResultRelInfo* resultRelInfo) {
    MemoryContext oldctx;

    oldctx = MemoryContextSwitchTo(memoryContext);

    bool is_valid_func;
    if (IsA(node, OpExpr))
        is_valid_func = is_valid_expression(((OpExpr*) node)->opfuncid);
    else if (IsA(node, FuncExpr))
        is_valid_func = is_valid_expression(((FuncExpr*) node)->funcid);
    else
        is_valid_func = false;

    if (is_valid_func)
        parse_jsonb_index_key(jbKeys, node);
    else
        expression_tree_walker(node, jsonb_index_key_walker, jbKeys);

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
        case JSONB_CONCAT:
            return true;
        default:
            return false;
    }
}

static bool jsonb_index_key_walker(Node* node, void* ctx) {
    List** jsonb_keys = (List **) ctx;

    if (IsA(node, Var)) {
        Var* nodeVar = (Var*) node;
        if (nodeVar->vartype == JSONBOID) {
            set_list_with_null(jsonb_keys);
            return true;
        }
    }

    if (IsA(node, FuncExpr) && is_valid_expression(((FuncExpr*) node)->funcid) ||
            IsA(node, OpExpr) && is_valid_expression(((OpExpr*) node)->opfuncid)) {

        return !parse_jsonb_index_key(jsonb_keys, node);
    }

    return expression_tree_walker(node, jsonb_index_key_walker, jsonb_keys);
}

static bool parse_jsonb_index_key(List** jbKeys, Node* node) {
    JsonbKey* jsonbUpdateKeys = palloc(sizeof (JsonbKey));
    jsonbUpdateKeys->key = NIL;
    Node* current = node;

    while (IsA(current, OpExpr) || IsA(current, FuncExpr)) {
        List* args = IsA(current, OpExpr) ? ((OpExpr*) current)->args : ((FuncExpr*) current)->args;
        Oid funcid = IsA(current, OpExpr) ? ((OpExpr*) current)->opfuncid : ((FuncExpr*) current)->funcid;

        if (!is_valid_expression(funcid)) {
            set_list_with_null(jbKeys);
            return false;
        }

        Node* left = linitial(args);
        if (IsA(left, Var)) {
            Var* var = (Var*) left;
            jsonbUpdateKeys->attnum = var->varattno;
        }
        Node* right = lsecond(args);
        if (IsA(right, Const)) {
            Const* c = (Const*) right;
            if (c->consttype == TEXTARRAYOID)
                jsonbUpdateKeys->key = jsonb_deconstruct_array(jsonbUpdateKeys->attnum, c);
            else if (c->consttype == TEXTOID) {
                char* key = TextDatumGetCString(c->constvalue);
                jsonbUpdateKeys->key = lcons(key, jsonbUpdateKeys->key);
            }
        }
        current = linitial(args);
    }

    (*jbKeys) = lappend(*jbKeys, jsonbUpdateKeys);
    return true;
}

static bool check_key_in_index(List* modify_columns, List* indexes_keys, Bitmapset** bitmapset) {
    ListCell* lc;
    foreach(lc, modify_columns) {
        JsonbKey* jbKeys = (JsonbKey*) lfirst(lc);


        if (is_key_in_index(indexes_keys, jbKeys->key, jbKeys->attnum))
            return true;
        else
            *bitmapset = bms_add_member(*bitmapset, jbKeys->attnum + 7);
    }

    return false;
}

static bool is_key_in_index(List* indexes_key, List* modify_key, int attnum) {
    ListCell* lc;
    foreach(lc, indexes_key) {
        JsonbKey* key = lfirst(lc);
        if (key->attnum != attnum)
            continue;

        ListCell* lc1;
        ListCell* lc2;

        bool key_in_index = true;

        forboth(lc1, key->key, lc2, modify_key) {
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