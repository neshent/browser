#include "dom.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

NbDocument *nb_doc_new(void) {
    NbDocument *doc = calloc(1, sizeof(*doc));
    doc->arena = nb_arena_new(0);
    doc->root  = nb_arena_alloc0(doc->arena, sizeof(NbNode));
    doc->root->type = NB_NODE_DOCUMENT;
    doc->root->tag  = nb_arena_strdup(doc->arena, "#document");
    return doc;
}

void nb_doc_free(NbDocument *doc) {
    if (!doc) return;
    nb_arena_free(doc->arena);
    free(doc);
}

NbNode *nb_node_element(NbDocument *doc, const char *tag) {
    NbNode *n = nb_arena_alloc0(doc->arena, sizeof(NbNode));
    n->type = NB_NODE_ELEMENT;
    size_t tl = strlen(tag);
    char *lt = nb_arena_alloc(doc->arena, tl + 1);
    for (size_t i = 0; i < tl; i++) lt[i] = tolower((unsigned char)tag[i]);
    lt[tl] = '\0';
    n->tag = lt;
    return n;
}

NbNode *nb_node_text(NbDocument *doc, const char *text, size_t len) {
    NbNode *n = nb_arena_alloc0(doc->arena, sizeof(NbNode));
    n->type = NB_NODE_TEXT;
    n->text = nb_arena_strndup(doc->arena, text, len);
    return n;
}

NbNode *nb_node_comment(NbDocument *doc, const char *text) {
    NbNode *n = nb_arena_alloc0(doc->arena, sizeof(NbNode));
    n->type = NB_NODE_COMMENT;
    n->text = nb_arena_strdup(doc->arena, text);
    return n;
}

void nb_node_append_child(NbNode *parent, NbNode *child) {
    child->parent = parent;
    child->prev_sibling = parent->last_child;
    if (parent->last_child) parent->last_child->next_sibling = child;
    else                     parent->first_child = child;
    parent->last_child = child;
    child->next_sibling = NULL;
}

NbAttr *nb_attr_get(NbNode *n, const char *name) {
    for (NbAttr *a = n->attrs; a; a = a->next)
        if (strcmp(a->name, name) == 0) return a;
    return NULL;
}

const char *nb_attr_val(NbNode *n, const char *name) {
    NbAttr *a = nb_attr_get(n, name);
    return a ? a->value : NULL;
}

void nb_attr_set(NbDocument *doc, NbNode *n, const char *name, const char *value) {
    NbAttr *a = nb_attr_get(n, name);
    if (a) { a->value = nb_arena_strdup(doc->arena, value ? value : ""); return; }
    a = nb_arena_alloc0(doc->arena, sizeof(NbAttr));
    a->name  = nb_arena_strdup(doc->arena, name);
    a->value = nb_arena_strdup(doc->arena, value ? value : "");
    a->next  = n->attrs;
    n->attrs = a;
}

NbNode *nb_doc_by_id(NbNode *root, const char *id) {
    if (!root || !id) return NULL;
    if (root->type == NB_NODE_ELEMENT) {
        const char *v = nb_attr_val(root, "id");
        if (v && strcmp(v, id) == 0) return root;
    }
    for (NbNode *c = root->first_child; c; c = c->next_sibling) {
        NbNode *found = nb_doc_by_id(c, id);
        if (found) return found;
    }
    return NULL;
}

NbNode *nb_doc_by_tag(NbNode *root, const char *tag) {
    if (!root || !tag) return NULL;
    if (root->type == NB_NODE_ELEMENT && strcmp(root->tag, tag) == 0) return root;
    for (NbNode *c = root->first_child; c; c = c->next_sibling) {
        NbNode *found = nb_doc_by_tag(c, tag);
        if (found) return found;
    }
    return NULL;
}
