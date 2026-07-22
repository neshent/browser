#ifndef NB_RENDER_H
#define NB_RENDER_H

#include "../layout/layout.h"
#include <cairo/cairo.h>
#include <pango/pangocairo.h>

/*
 * Paint layer: walks the box tree and draws to a Cairo surface.
 * Uses Pango for text rendering (proper Unicode, font fallback, kerning).
 * Called from GTK draw callback with the widget's Cairo context.
 */

typedef struct {
    float scroll_x;
    float scroll_y;
    float scale;        /* device pixel ratio / zoom */
} NbRenderState;

/* Paint the entire layout into a Cairo context */
void nb_render_paint(cairo_t *cr, NbLayout *layout, NbRenderState *state);

/* Render a selection highlight rect */
void nb_render_selection(cairo_t *cr, float x, float y, float w, float h);

/* Draw a simple loading/error page */
void nb_render_error_page(cairo_t *cr, float vw, float vh, const char *msg);
void nb_render_loading_page(cairo_t *cr, float vw, float vh, const char *url);

#endif /* NB_RENDER_H */
