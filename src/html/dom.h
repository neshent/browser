#ifndef NB_DOM_H
#define NB_DOM_H

#include "../util/nb_arena.h"
#include <stdint.h>

/*
 * Minimal DOM tree.  All nodes live in an NbArena; freeing the arena
 * destroys the whole tree without per-node frees.
 */

typedef enum {
    NODE_DOCUMENT,
    NODE_ELEMENT,
    NODE_TEXT,
    NODE_COMMENT,
    NODE_DOCTYPE
} NbNodeType;

typedef struct NbAttr {
    char          *name;
    char          *value;
    struct NbAttr *next;
} NbAttr;

typedef struct NbNode {
    NbNodeType     type;
    char          *tag;          /* element tag name, lower-case */
    char          *text;         /* text content for TEXT nodes */
    NbAttr        *attrs;
    struct NbNode *parent;
    struct NbNode *first_child;
    struct NbNode *last_child;
    struct NbNode *next_sibling;
    struct NbNode *prev_sibling;
    /* Style pointer — filled in by CSS engine */
    void          *computed_style;
} NbNode;

typedef struct {
    NbNode  *root;       /* document node */
    NbArena *arena;
} NbDocument;

/* Create / destroy */
NbDocument *nb_doc_new(void);
void        nb_doc_free(NbDocument *doc);

/* Node construction (all nodes owned by doc->arena) */
NbNode *nb_node_element(NbDocument *doc, const char *tag);
NbNode *nb_node_text(NbDocument *doc, const char *text, size_t len);
NbNode *nb_node_comment(NbDocument *doc, const char *text);

/* Tree manipulation */
void nb_node_append_child(NbNode *parent, NbNode *child);

/* Attribute helpers */
NbAttr *nb_attr_get(NbNode *n, const char *name);
const char *nb_attr_val(NbNode *n, const char *name);
void    nb_attr_set(NbDocument *doc, NbNode *n, const char *name, const char *value);

/* Queries */
NbNode *nb_doc_by_id(NbNode *root, const char *id);
NbNode *nb_doc_by_tag(NbNode *root, const char *tag);   /* first match */

#endif /* NB_DOM_H */
