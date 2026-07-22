#ifndef NB_JS_ENGINE_H
#define NB_JS_ENGINE_H

#include "../html/dom.h"

/*
 * JavaScript engine bridge — wraps QuickJS.
 * Exposes a minimal DOM/Web API surface to scripts:
 *   document.getElementById / querySelector / querySelectorAll
 *   document.title (get/set)
 *   element.innerHTML / textContent / style (set)
 *   element.setAttribute / getAttribute
 *   element.addEventListener (click, input, change, submit)
 *   window.location.href (get/set → triggers navigation)
 *   window.setTimeout / clearTimeout  (deferred execution)
 *   console.log / warn / error
 *   fetch() — wraps nb_http_get, returns Promise<Response>
 *   XMLHttpRequest (basic, sync+async)
 *   JSON.parse / JSON.stringify  (built into QuickJS)
 *   localStorage (in-memory stub)
 *   history.pushState / back / forward (stub)
 */

typedef struct NbJsEngine NbJsEngine;

/* Callback when JS requests navigation */
typedef void (*NbJsNavigateCb)(const char *url, void *userdata);
/* Callback when JS requests a repaint */
typedef void (*NbJsRepaintCb)(void *userdata);

NbJsEngine *nb_js_engine_new(NbDocument *doc,
                              NbJsNavigateCb nav_cb,
                              NbJsRepaintCb  repaint_cb,
                              void          *userdata);
void        nb_js_engine_free(NbJsEngine *engine);

/* Run all <script> tags found in the document */
void        nb_js_run_scripts(NbJsEngine *engine);

/* Run an arbitrary JS string (e.g. from eval, inline handler) */
void        nb_js_eval(NbJsEngine *engine, const char *code, size_t len);

/* Fire a DOM event on a node (click, input, submit, etc.) */
void        nb_js_fire_event(NbJsEngine *engine, NbNode *node, const char *event_type);

/* Pump pending setTimeout callbacks — call from GTK idle */
int         nb_js_pump_timers(NbJsEngine *engine);

#endif /* NB_JS_ENGINE_H */
