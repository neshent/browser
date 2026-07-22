#include "js_engine.h"
#include "../net/http.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef NB_USE_QUICKJS
#include "quickjs.h"

/* ------------------------------------------------------------------ */
/*  Engine struct                                                       */
/* ------------------------------------------------------------------ */
typedef struct TimerEntry {
    int              id;
    JSValue          func;
    int              delay_ms;
    struct TimerEntry *next;
} TimerEntry;

struct NbJsEngine {
    JSRuntime       *rt;
    JSContext       *ctx;
    NbDocument      *doc;
    NbJsNavigateCb   nav_cb;
    NbJsRepaintCb    repaint_cb;
    void            *userdata;
    TimerEntry      *timers;
    int              next_timer_id;
};

/* ------------------------------------------------------------------ */
/*  console.log / warn / error                                         */
/* ------------------------------------------------------------------ */
static JSValue js_console_log(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        if (s) { fprintf(stdout, "%s%s", i ? " " : "", s); JS_FreeCString(ctx, s); }
    }
    fprintf(stdout, "\n");
    return JS_UNDEFINED;
}

/* ------------------------------------------------------------------ */
/*  DOM helpers                                                         */
/* ------------------------------------------------------------------ */

/* Convert C NbNode* ↔ JS object via opaque pointer */
static JSClassID node_class_id;

static void node_finalizer(JSRuntime *rt, JSValue val) { /* arena-owned, nothing to free */ }

static JSClassDef node_class = { "NbNode", .finalizer = node_finalizer };

static JSValue node_to_js(JSContext *ctx, NbNode *node);

static NbNode *js_to_node(JSContext *ctx, JSValueConst val) {
    if (!JS_IsObject(val)) return NULL;
    return (NbNode*)JS_GetOpaque(val, node_class_id);
}

static JSValue js_node_get_innerHTML(JSContext *ctx, JSValueConst this_val) {
    NbNode *n = js_to_node(ctx, this_val);
    if (!n) return JS_NULL;
    /* Simple serialisation — just collect text content */
    char buf[8192]; buf[0]='\0'; size_t pos=0;
    for (NbNode *c=n->first_child; c; c=c->next_sibling) {
        if (c->type==NB_NODE_TEXT && c->text) {
            size_t l=strlen(c->text);
            if (pos+l<sizeof(buf)-1) { memcpy(buf+pos,c->text,l); pos+=l; }
        }
    }
    buf[pos]='\0';
    return JS_NewString(ctx, buf);
}

static JSValue js_node_set_innerHTML(JSContext *ctx, JSValueConst this_val, JSValueConst val) {
    NbNode *n = js_to_node(ctx, this_val);
    if (!n) return JS_UNDEFINED;
    const char *html = JS_ToCString(ctx, val);
    if (!html) return JS_UNDEFINED;
    /* Remove existing children (arena-owned so just orphan them) */
    n->first_child = n->last_child = NULL;
    /* Parse and attach fragment */
    NbDocument *frag = nb_html_parse(html, strlen(html));
    if (frag && frag->root) {
        NbNode *fc = frag->root->first_child;
        while (fc) {
            NbNode *next = fc->next_sibling;
            fc->next_sibling = fc->prev_sibling = NULL;
            nb_node_append_child(n, fc);
            fc = next;
        }
    }
    JS_FreeCString(ctx, html);
    return JS_UNDEFINED;
}

static JSValue js_node_get_textContent(JSContext *ctx, JSValueConst this_val) {
    NbNode *n = js_to_node(ctx, this_val);
    if (!n) return JS_NULL;
    char buf[4096]; buf[0]='\0'; size_t pos=0;
    for (NbNode *c=n->first_child; c; c=c->next_sibling)
        if (c->type==NB_NODE_TEXT && c->text) {
            size_t l=strlen(c->text);
            if (pos+l<sizeof(buf)-1) { memcpy(buf+pos,c->text,l); pos+=l; }
        }
    buf[pos]='\0';
    return JS_NewString(ctx, buf);
}

static JSValue js_node_getAttribute(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    NbNode *n = js_to_node(ctx, this_val);
    if (!n || argc<1) return JS_NULL;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_NULL;
    const char *v = nb_attr_val(n, name);
    JS_FreeCString(ctx, name);
    return v ? JS_NewString(ctx,v) : JS_NULL;
}

static JSValue js_node_setAttribute(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    NbNode *n = js_to_node(ctx, this_val);
    NbJsEngine *eng = (NbJsEngine*)JS_GetContextOpaque(ctx);
    if (!n || argc<2 || !eng) return JS_UNDEFINED;
    const char *name = JS_ToCString(ctx, argv[0]);
    const char *val  = JS_ToCString(ctx, argv[1]);
    if (name && val) nb_attr_set(eng->doc, n, name, val);
    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, val);
    return JS_UNDEFINED;
}

/* ---- event listeners ---- */
/* Stored as a JS array on the node object under "__listeners__<event>" */
static JSValue js_node_addEventListener(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
    if (argc < 2) return JS_UNDEFINED;
    const char *type = JS_ToCString(ctx, argv[0]);
    if (!type) return JS_UNDEFINED;
    char key[128]; snprintf(key, sizeof(key), "__ev_%s", type);
    JS_FreeCString(ctx, type);
    JSValue arr = JS_GetPropertyStr(ctx, this_val, key);
    if (!JS_IsArray(ctx, arr)) {
        JS_FreeValue(ctx, arr);
        arr = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, this_val, key, JS_DupValue(ctx, arr));
    }
    JSValue len_val = JS_GetPropertyStr(ctx, arr, "length");
    int32_t len; JS_ToInt32(ctx, &len, len_val);
    JS_FreeValue(ctx, len_val);
    JS_SetPropertyUint32(ctx, arr, len, JS_DupValue(ctx, argv[1]));
    JS_FreeValue(ctx, arr);
    return JS_UNDEFINED;
}

/* ---- Build a JS Node object ---- */
static JSValue node_to_js(JSContext *ctx, NbNode *node) {
    if (!node) return JS_NULL;
    JSValue obj = JS_NewObjectClass(ctx, node_class_id);
    JS_SetOpaque(obj, node);

    /* Properties */
    const char *tag = (node->type==NB_NODE_ELEMENT && node->tag) ? node->tag : "";
    JS_SetPropertyStr(ctx, obj, "tagName",     JS_NewString(ctx, tag));
    JS_SetPropertyStr(ctx, obj, "nodeName",    JS_NewString(ctx, tag));
    JS_SetPropertyStr(ctx, obj, "nodeType",
        JS_NewInt32(ctx, node->type==NB_NODE_ELEMENT ? 1 : node->type==NB_NODE_TEXT ? 3 : 8));
    JS_SetPropertyStr(ctx, obj, "id",
        JS_NewString(ctx, nb_attr_val(node,"id") ? nb_attr_val(node,"id") : ""));
    JS_SetPropertyStr(ctx, obj, "className",
        JS_NewString(ctx, nb_attr_val(node,"class") ? nb_attr_val(node,"class") : ""));

    /* Methods */
    JS_SetPropertyStr(ctx, obj, "getAttribute",
        JS_NewCFunction(ctx, js_node_getAttribute, "getAttribute", 1));
    JS_SetPropertyStr(ctx, obj, "setAttribute",
        JS_NewCFunction(ctx, js_node_setAttribute, "setAttribute", 2));
    JS_SetPropertyStr(ctx, obj, "addEventListener",
        JS_NewCFunction(ctx, js_node_addEventListener, "addEventListener", 2));

    /* innerHTML / textContent as functions (simplification) */
    JS_SetPropertyStr(ctx, obj, "getInnerHTML",
        JS_NewCFunction(ctx, (JSCFunction*)js_node_get_innerHTML, "getInnerHTML", 0));

    return obj;
}

/* ------------------------------------------------------------------ */
/*  document object                                                    */
/* ------------------------------------------------------------------ */
static JSValue js_doc_getElementById(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    NbJsEngine *eng = (NbJsEngine*)JS_GetContextOpaque(ctx);
    if (!eng || argc<1) return JS_NULL;
    const char *id = JS_ToCString(ctx, argv[0]);
    if (!id) return JS_NULL;
    NbNode *n = nb_doc_by_id(eng->doc->root, id);
    JS_FreeCString(ctx, id);
    return n ? node_to_js(ctx, n) : JS_NULL;
}

static JSValue js_doc_querySelector(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    NbJsEngine *eng = (NbJsEngine*)JS_GetContextOpaque(ctx);
    if (!eng || argc<1) return JS_NULL;
    const char *sel = JS_ToCString(ctx, argv[0]);
    if (!sel) return JS_NULL;
    /* simple: if starts with #, getElementById, else getElementsByTagName */
    NbNode *n = NULL;
    if (sel[0]=='#') n = nb_doc_by_id(eng->doc->root, sel+1);
    else             n = nb_doc_by_tag(eng->doc->root, sel);
    JS_FreeCString(ctx, sel);
    return n ? node_to_js(ctx, n) : JS_NULL;
}

static JSValue js_doc_createEl(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    NbJsEngine *eng = (NbJsEngine*)JS_GetContextOpaque(ctx);
    if (!eng || argc<1) return JS_NULL;
    const char *tag = JS_ToCString(ctx, argv[0]);
    if (!tag) return JS_NULL;
    NbNode *n = nb_node_element(eng->doc, tag);
    JS_FreeCString(ctx, tag);
    return node_to_js(ctx, n);
}

static JSValue js_doc_get_title(JSContext *ctx, JSValueConst this_val) {
    NbJsEngine *eng = (NbJsEngine*)JS_GetContextOpaque(ctx);
    if (!eng) return JS_NULL;
    NbNode *title = nb_doc_by_tag(eng->doc->root, "title");
    if (title && title->first_child && title->first_child->text)
        return JS_NewString(ctx, title->first_child->text);
    return JS_NewString(ctx, "");
}

static JSValue js_doc_set_title(JSContext *ctx, JSValueConst this_val, JSValueConst val) {
    NbJsEngine *eng = (NbJsEngine*)JS_GetContextOpaque(ctx);
    if (!eng) return JS_UNDEFINED;
    NbNode *title = nb_doc_by_tag(eng->doc->root, "title");
    if (title && title->first_child && title->first_child->type==NB_NODE_TEXT) {
        const char *s = JS_ToCString(ctx, val);
        if (s) { title->first_child->text = nb_arena_strdup(eng->doc->arena, s); JS_FreeCString(ctx,s); }
    }
    return JS_UNDEFINED;
}

/* ------------------------------------------------------------------ */
/*  window.setTimeout                                                  */
/* ------------------------------------------------------------------ */
static JSValue js_setTimeout(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    NbJsEngine *eng = (NbJsEngine*)JS_GetContextOpaque(ctx);
    if (!eng || argc<1) return JS_NewInt32(ctx,0);
    int delay = (argc>=2) ? ({ int32_t d; JS_ToInt32(ctx,&d,argv[1]); d; }) : 0;
    TimerEntry *t = calloc(1,sizeof(*t));
    t->id       = ++eng->next_timer_id;
    t->func     = JS_DupValue(ctx, argv[0]);
    t->delay_ms = delay;
    t->next     = eng->timers;
    eng->timers = t;
    return JS_NewInt32(ctx, t->id);
}

static JSValue js_clearTimeout(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    NbJsEngine *eng = (NbJsEngine*)JS_GetContextOpaque(ctx);
    if (!eng || argc<1) return JS_UNDEFINED;
    int32_t id; JS_ToInt32(ctx,&id,argv[0]);
    for (TimerEntry **p=&eng->timers; *p; p=&(*p)->next) {
        if ((*p)->id==id) {
            TimerEntry *t=*p; *p=t->next;
            JS_FreeValue(ctx,t->func); free(t); break;
        }
    }
    return JS_UNDEFINED;
}

/* ------------------------------------------------------------------ */
/*  window.location                                                    */
/* ------------------------------------------------------------------ */
static JSValue js_location_get_href(JSContext *ctx, JSValueConst this_val) {
    /* returned as empty — real URL set by page init code */
    JSValue href = JS_GetPropertyStr(ctx, this_val, "__href__");
    if (JS_IsUndefined(href)||JS_IsNull(href)) return JS_NewString(ctx,"");
    return href;
}
static JSValue js_location_set_href(JSContext *ctx, JSValueConst this_val, JSValueConst val) {
    NbJsEngine *eng = (NbJsEngine*)JS_GetContextOpaque(ctx);
    if (!eng) return JS_UNDEFINED;
    const char *url = JS_ToCString(ctx, val);
    if (url && eng->nav_cb) eng->nav_cb(url, eng->userdata);
    JS_FreeCString(ctx, url);
    return JS_UNDEFINED;
}

/* ------------------------------------------------------------------ */
/*  fetch() — basic Promise wrapper around nb_http_get                */
/* ------------------------------------------------------------------ */
static JSValue js_fetch(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv) {
    if (argc<1) return JS_UNDEFINED;
    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_UNDEFINED;

    NbHttpResponse resp = nb_http_get(url);
    JS_FreeCString(ctx, url);

    /* Build a response-like object */
    JSValue res_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, res_obj, "status", JS_NewInt32(ctx, resp.status_code));
    JS_SetPropertyStr(ctx, res_obj, "ok",     JS_NewBool(ctx, resp.status_code>=200&&resp.status_code<300));
    /* .text() method */
    JSValue body_str = JS_NewStringLen(ctx, resp.body.data ? resp.body.data : "", resp.body.len);
    JS_SetPropertyStr(ctx, res_obj, "_body", body_str);

    /* Minimal then-able promise substitute */
    JSValue promise_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, promise_obj, "response", JS_DupValue(ctx, res_obj));

    nb_http_response_free(&resp);
    JS_FreeValue(ctx, res_obj);
    return promise_obj;
}

/* ------------------------------------------------------------------ */
/*  Engine init / teardown                                             */
/* ------------------------------------------------------------------ */
NbJsEngine *nb_js_engine_new(NbDocument *doc,
                              NbJsNavigateCb nav_cb,
                              NbJsRepaintCb  repaint_cb,
                              void          *userdata) {
    NbJsEngine *eng = calloc(1, sizeof(*eng));
    eng->doc        = doc;
    eng->nav_cb     = nav_cb;
    eng->repaint_cb = repaint_cb;
    eng->userdata   = userdata;

    eng->rt  = JS_NewRuntime();
    JS_SetMemoryLimit(eng->rt, 64 * 1024 * 1024); /* 64 MB JS heap */
    JS_SetMaxStackSize(eng->rt, 512 * 1024);       /* 512 KB stack */

    eng->ctx = JS_NewContext(eng->rt);
    JS_SetContextOpaque(eng->ctx, eng);

    /* Register NbNode class */
    JS_NewClassID(&node_class_id);
    JS_NewClass(eng->rt, node_class_id, &node_class);
    JSValue node_proto = JS_NewObject(eng->ctx);
    JS_SetClassProto(eng->ctx, node_class_id, node_proto);

    JSContext *ctx = eng->ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    /* console */
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log",   JS_NewCFunction(ctx, js_console_log,  "log",   1));
    JS_SetPropertyStr(ctx, console, "warn",  JS_NewCFunction(ctx, js_console_log,  "warn",  1));
    JS_SetPropertyStr(ctx, console, "error", JS_NewCFunction(ctx, js_console_log,  "error", 1));
    JS_SetPropertyStr(ctx, global,  "console", console);

    /* document */
    JSValue jsdoc = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, jsdoc, "getElementById",
        JS_NewCFunction(ctx, js_doc_getElementById, "getElementById", 1));
    JS_SetPropertyStr(ctx, jsdoc, "querySelector",
        JS_NewCFunction(ctx, js_doc_querySelector,  "querySelector",  1));
    JS_SetPropertyStr(ctx, jsdoc, "createElement",
        JS_NewCFunction(ctx, js_doc_createEl,        "createElement",  1));
    /* title getter/setter via defineProperty */
    {
        JSValue getter = JS_NewCFunction(ctx, (JSCFunction*)js_doc_get_title, "get", 0);
        JSValue setter = JS_NewCFunction(ctx, (JSCFunction*)js_doc_set_title, "set", 1);
        JSAtom title_atom = JS_NewAtom(ctx, "title");
        JSPropertyDescriptor desc = {
            .flags = JS_PROP_HAS_GET | JS_PROP_HAS_SET | JS_PROP_ENUMERABLE,
            .getter = getter, .setter = setter,
            .value = JS_UNDEFINED, .writable = JS_UNDEFINED
        };
        JS_DefineProperty(ctx, jsdoc, title_atom, JS_UNDEFINED, getter, setter,
                          JS_PROP_HAS_GET|JS_PROP_HAS_SET|JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, title_atom);
        JS_FreeValue(ctx, getter);
        JS_FreeValue(ctx, setter);
    }
    JS_SetPropertyStr(ctx, global, "document", jsdoc);

    /* window.location */
    JSValue location = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, location, "__href__", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, location, "href",     JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, global,   "location", location);

    /* window.setTimeout / clearTimeout */
    JS_SetPropertyStr(ctx, global, "setTimeout",
        JS_NewCFunction(ctx, js_setTimeout,  "setTimeout",  2));
    JS_SetPropertyStr(ctx, global, "clearTimeout",
        JS_NewCFunction(ctx, js_clearTimeout,"clearTimeout",1));
    JS_SetPropertyStr(ctx, global, "setInterval",
        JS_NewCFunction(ctx, js_setTimeout,  "setInterval", 2)); /* stub */
    JS_SetPropertyStr(ctx, global, "clearInterval",
        JS_NewCFunction(ctx, js_clearTimeout,"clearInterval",1));

    /* fetch */
    JS_SetPropertyStr(ctx, global, "fetch",
        JS_NewCFunction(ctx, js_fetch, "fetch", 1));

    /* window = global self-reference */
    JS_SetPropertyStr(ctx, global, "window", JS_DupValue(ctx, global));

    /* localStorage stub */
    JSValue ls = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ls, "getItem",    JS_NewCFunction(ctx, (JSCFunction*)js_console_log,"getItem",1));
    JS_SetPropertyStr(ctx, ls, "setItem",    JS_NewCFunction(ctx, (JSCFunction*)js_console_log,"setItem",1));
    JS_SetPropertyStr(ctx, ls, "removeItem", JS_NewCFunction(ctx, (JSCFunction*)js_console_log,"removeItem",1));
    JS_SetPropertyStr(ctx, global, "localStorage",   ls);
    JS_SetPropertyStr(ctx, global, "sessionStorage",  JS_DupValue(ctx, ls));

    JS_FreeValue(ctx, global);
    return eng;
}

void nb_js_engine_free(NbJsEngine *eng) {
    if (!eng) return;
    TimerEntry *t = eng->timers;
    while (t) {
        TimerEntry *n = t->next;
        JS_FreeValue(eng->ctx, t->func);
        free(t); t = n;
    }
    JS_FreeContext(eng->ctx);
    JS_FreeRuntime(eng->rt);
    free(eng);
}

/* ------------------------------------------------------------------ */
/*  Run scripts / eval                                                 */
/* ------------------------------------------------------------------ */
/* Collect all <script> text recursively */
static void collect_scripts(NbNode *node, char **out, size_t *out_len, size_t *cap) {
    if (!node) return;
    if (node->type==NB_NODE_ELEMENT && strcmp(node->tag,"script")==0) {
        /* skip external scripts */
        if (nb_attr_val(node,"src")) return;
        for (NbNode *c=node->first_child; c; c=c->next_sibling) {
            if (c->type==NB_NODE_TEXT && c->text) {
                size_t l = strlen(c->text);
                if (*out_len+l+2 > *cap) {
                    *cap = (*out_len+l+2)*2+4096;
                    *out = realloc(*out, *cap);
                }
                memcpy(*out+*out_len, c->text, l);
                (*out_len) += l;
                (*out)[*out_len] = '\n';
                (*out_len)++;
            }
        }
    }
    for (NbNode *c=node->first_child; c; c=c->next_sibling)
        collect_scripts(c, out, out_len, cap);
}

void nb_js_run_scripts(NbJsEngine *eng) {
    if (!eng) return;
    char *code = NULL; size_t code_len=0, cap=0;
    collect_scripts(eng->doc->root, &code, &code_len, &cap);
    if (code_len > 0) {
        code[code_len] = '\0';
        JSValue result = JS_Eval(eng->ctx, code, code_len, "<page>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(result)) {
            JSValue exc = JS_GetException(eng->ctx);
            const char *msg = JS_ToCString(eng->ctx, exc);
            fprintf(stderr, "JS error: %s\n", msg ? msg : "?");
            JS_FreeCString(eng->ctx, msg);
            JS_FreeValue(eng->ctx, exc);
        }
        JS_FreeValue(eng->ctx, result);
    }
    free(code);
}

void nb_js_eval(NbJsEngine *eng, const char *code, size_t len) {
    if (!eng || !code || !len) return;
    JSValue result = JS_Eval(eng->ctx, code, len, "<eval>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(eng->ctx);
        const char *msg = JS_ToCString(eng->ctx, exc);
        fprintf(stderr, "JS eval error: %s\n", msg ? msg : "?");
        JS_FreeCString(eng->ctx, msg);
        JS_FreeValue(eng->ctx, exc);
    }
    JS_FreeValue(eng->ctx, result);
}

void nb_js_fire_event(NbJsEngine *eng, NbNode *node, const char *event_type) {
    if (!eng || !node || !event_type) return;
    /* TODO: look up registered listeners on the node's JS wrapper and call them */
    /* For now: look for inline on<event> attribute */
    char attr_name[64]; snprintf(attr_name,sizeof(attr_name),"on%s",event_type);
    const char *handler = nb_attr_val(node, attr_name);
    if (handler) nb_js_eval(eng, handler, strlen(handler));
}

int nb_js_pump_timers(NbJsEngine *eng) {
    if (!eng || !eng->timers) return 0;
    /* Fire all timers (simplified: fire all immediately) */
    TimerEntry *t = eng->timers;
    eng->timers   = NULL;
    int fired = 0;
    while (t) {
        TimerEntry *n = t->next;
        if (JS_IsFunction(eng->ctx, t->func)) {
            JSValue ret = JS_Call(eng->ctx, t->func, JS_UNDEFINED, 0, NULL);
            if (JS_IsException(ret)) {
                JSValue exc = JS_GetException(eng->ctx);
                JS_FreeValue(eng->ctx, exc);
            }
            JS_FreeValue(eng->ctx, ret);
            fired++;
        }
        JS_FreeValue(eng->ctx, t->func);
        free(t); t = n;
    }
    if (fired && eng->repaint_cb) eng->repaint_cb(eng->userdata);
    return fired;
}

#else /* NB_USE_QUICKJS not defined — stub implementation */

struct NbJsEngine { NbDocument *doc; };

NbJsEngine *nb_js_engine_new(NbDocument *doc, NbJsNavigateCb n, NbJsRepaintCb r, void *u) {
    NbJsEngine *e = calloc(1, sizeof(*e)); e->doc=doc; return e;
}
void nb_js_engine_free(NbJsEngine *e)               { free(e); }
void nb_js_run_scripts(NbJsEngine *e)               { (void)e; }
void nb_js_eval(NbJsEngine *e, const char *c, size_t l) { (void)e;(void)c;(void)l; }
void nb_js_fire_event(NbJsEngine *e, NbNode *n, const char *t) { (void)e;(void)n;(void)t; }
int  nb_js_pump_timers(NbJsEngine *e)               { (void)e; return 0; }

#endif /* NB_USE_QUICKJS */
