/*
 * browser.c — Frameless GTK window with custom Cairo-drawn chrome
 *
 * Architecture:
 *  - Single GtkWindow with gtk_window_set_decorated(FALSE) → no Windows frame
 *  - One GtkDrawingArea covering the entire window
 *  - All UI drawn in Cairo: titlebar, tabs, toolbar, content, status bar
 *  - Hit-testing determines which zone was clicked
 *  - Manual window drag/resize via GDK
 */
#include "browser.h"
#include "../html/html_parser.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <pthread.h>
#include <ctype.h>

/* Windows native GDK handled by GTK internals — no direct include needed */

/* ------------------------------------------------------------------ */
/*  Default theme                                                      */
/* ------------------------------------------------------------------ */
NbTheme nb_theme_default(void) {
    NbTheme t = {0};
    t.ui_scale    = 1.0f;

    /* Modern dark chrome */
    t.chrome_r = t.chrome_g = t.chrome_b = 0.15f;
    t.chrome_text_r = t.chrome_text_g = t.chrome_text_b = 0.95f;
    t.accent_r = 0.2f; t.accent_g = 0.5f; t.accent_b = 1.0f;
    t.tab_bg_r = t.tab_bg_g = t.tab_bg_b = 0.20f;
    t.page_bg_r = t.page_bg_g = t.page_bg_b = 0.13f;

    strcpy(t.ui_font, "Sans");
    t.ui_font_size = 11.0f;

    t.show_bookmarks_bar = 0;
    t.show_status_bar    = 1;
    t.compact_toolbar    = 0;
    t.window_w = 1200; t.window_h = 800;
    t.maximized = 0;
    t.corner_radius = 8;
    return t;
}

void nb_theme_save(const NbTheme *t, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "ui_scale %.2f\n", t->ui_scale);
    fprintf(f, "chrome_color %.3f %.3f %.3f\n", t->chrome_r, t->chrome_g, t->chrome_b);
    fprintf(f, "chrome_text %.3f %.3f %.3f\n", t->chrome_text_r, t->chrome_text_g, t->chrome_text_b);
    fprintf(f, "accent %.3f %.3f %.3f\n", t->accent_r, t->accent_g, t->accent_b);
    fprintf(f, "ui_font %s\n", t->ui_font);
    fprintf(f, "ui_font_size %.1f\n", t->ui_font_size);
    fprintf(f, "window_size %d %d\n", t->window_w, t->window_h);
    fclose(f);
}

int nb_theme_load(NbTheme *t, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line,"ui_scale",8)==0) sscanf(line+8, "%f", &t->ui_scale);
        else if (strncmp(line,"chrome_color",12)==0)
            sscanf(line+12, "%f %f %f", &t->chrome_r, &t->chrome_g, &t->chrome_b);
        else if (strncmp(line,"chrome_text",11)==0)
            sscanf(line+11, "%f %f %f", &t->chrome_text_r, &t->chrome_text_g, &t->chrome_text_b);
        else if (strncmp(line,"accent",6)==0)
            sscanf(line+6, "%f %f %f", &t->accent_r, &t->accent_g, &t->accent_b);
        else if (strncmp(line,"ui_font ",8)==0) {
            char *nl = strchr(line+8, '\n'); if (nl) *nl=0;
            strncpy(t->ui_font, line+8, sizeof(t->ui_font)-1);
        }
        else if (strncmp(line,"ui_font_size",12)==0)
            sscanf(line+12, "%f", &t->ui_font_size);
        else if (strncmp(line,"window_size",11)==0)
            sscanf(line+11, "%d %d", &t->window_w, &t->window_h);
    }
    fclose(f); return 1;
}

/* ------------------------------------------------------------------ */
/*  Forward decls                                                       */
/* ------------------------------------------------------------------ */
static void nav_cb_js(const char *url, void *userdata);
static void repaint_cb_js(void *userdata);
static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data);
static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, gpointer data);
static gboolean on_button_press(GtkWidget *w, GdkEventButton *ev, gpointer data);
static gboolean on_button_release(GtkWidget *w, GdkEventButton *ev, gpointer data);
static gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer data);
static gboolean on_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer data);
static gboolean timer_pump_cb(gpointer data);
static void     on_window_state(GtkWidget *w, GdkEventWindowState *ev, gpointer data);
static gboolean on_configure(GtkWidget *w, GdkEventConfigure *ev, gpointer data);

/* ------------------------------------------------------------------ */
/*  Tab lifecycle                                                       */
/* ------------------------------------------------------------------ */
static NbTab *tab_new(NbBrowser *b, const char *url) {
    NbTab *t = calloc(1, sizeof(*t));
    if (url) strncpy(t->url, url, sizeof(t->url)-1);
    strncpy(t->title, url && url[0] ? url : "New Tab", sizeof(t->title)-1);
    t->render_state.scale = 1.0f;
    t->index = b->tab_count;

    /* link into list */
    if (b->tabs) {
        NbTab *last = b->tabs;
        while (last->next) last = last->next;
        last->next = t; t->prev = last;
    } else {
        b->tabs = t;
    }
    b->tab_count++;
    return t;
}

static void tab_free(NbTab *t) {
    if (!t) return;
    if (t->js)       nb_js_engine_free(t->js);
    if (t->layout)   nb_layout_free(t->layout);
    if (t->page_css) nb_css_free(t->page_css);
    if (t->doc)      nb_doc_free(t->doc);
    free(t);
}

static void tab_set_title(NbBrowser *b, NbTab *tab, const char *title) {
    strncpy(tab->title, title, sizeof(tab->title)-1);
    if (tab == b->active_tab)
        gtk_window_set_title(GTK_WINDOW(b->window), title);
}

/* ------------------------------------------------------------------ */
/*  Navigation (same threading approach as before)                     */
/* ------------------------------------------------------------------ */
typedef struct {
    NbBrowser *browser;
    NbTab     *tab;
    char       url[2048];
} LoadJob;

typedef struct {
    NbBrowser    *b;
    NbTab        *tab;
    NbDocument   *doc;
    NbStylesheet *css;
    char          title[512];
    char          url[2048];
    char          error[512];
} LoadResult;

static gboolean load_done_cb(gpointer data) {
    LoadResult *r = (LoadResult*)data;
    NbBrowser *b  = r->b;
    NbTab     *tab = r->tab;

    if (tab->js)       { nb_js_engine_free(tab->js);   tab->js       = NULL; }
    if (tab->layout)   { nb_layout_free(tab->layout);  tab->layout   = NULL; }
    if (tab->page_css) { nb_css_free(tab->page_css);   tab->page_css = NULL; }
    if (tab->doc)      { nb_doc_free(tab->doc);        tab->doc      = NULL; }

    tab->doc      = r->doc;
    tab->page_css = r->css;
    strncpy(tab->url,       r->url,   sizeof(tab->url)-1);
    strncpy(tab->error_msg, r->error, sizeof(tab->error_msg)-1);

    if (r->doc && !r->error[0]) {
        nb_css_apply(b->ua_css, tab->page_css, tab->doc);

        /* content area size (will be updated on next draw, but give estimate) */
        float vw = b->content_w > 10 ? b->content_w : 800;
        float vh = b->content_h > 10 ? b->content_h : 600;
        tab->layout = nb_layout_build(tab->doc, vw, vh);

        tab->js = nb_js_engine_new(tab->doc, nav_cb_js, repaint_cb_js, tab);
        nb_js_run_scripts(tab->js);

        NbNode *title_node = nb_doc_by_tag(tab->doc->root, "title");
        if (title_node && title_node->first_child && title_node->first_child->text)
            tab_set_title(b, tab, title_node->first_child->text);
        else
            tab_set_title(b, tab, tab->url);

        if (tab == b->active_tab) {
            strncpy(b->url_edit, tab->url, sizeof(b->url_edit)-1);
            b->url_cursor = (int)strlen(b->url_edit);
        }
    } else {
        tab_set_title(b, tab, "Error");
    }

    tab->loading = 0;
    nb_browser_queue_draw(b);
    free(r);
    return G_SOURCE_REMOVE;
}

static void *load_thread(void *arg) {
    LoadJob *job = (LoadJob*)arg;
    LoadResult *res = calloc(1, sizeof(*res));
    res->b   = job->browser;
    res->tab = job->tab;
    strncpy(res->url, job->url, sizeof(res->url)-1);

    if (strcmp(job->url, "about:blank") == 0 || strcmp(job->url, "about:newtab") == 0) {
        const char *blank = "<html><head><style>body{background:#222;color:#ccc;font-family:sans-serif;margin:40px;}</style></head>"
                            "<body><h2>Nishant Browser</h2><p>Enter a URL to browse.</p></body></html>";
        res->doc = nb_html_parse(blank, strlen(blank));
        strncpy(res->title, "New Tab", sizeof(res->title)-1);
        goto done;
    }

    NbHttpResponse resp = nb_http_get(job->url);

    /* Follow redirects */
    int redirects = 0;
    while ((resp.status_code == 301 || resp.status_code == 302 ||
            resp.status_code == 303 || resp.status_code == 307 ||
            resp.status_code == 308) && resp.location && redirects < 5) {
        char *new_url = nb_url_resolve(job->url, resp.location);
        strncpy(res->url, new_url, sizeof(res->url)-1);
        free(new_url);
        nb_http_response_free(&resp);
        resp = nb_http_get(res->url);
        redirects++;
    }

    if (resp.error) {
        snprintf(res->error, sizeof(res->error), "%s", resp.error_msg);
        nb_http_response_free(&resp);
        goto done;
    }
    if (resp.status_code < 100) {
        snprintf(res->error, sizeof(res->error), "Connection failed (status %d)", resp.status_code);
        nb_http_response_free(&resp);
        goto done;
    }

    if (resp.body.data && resp.body.len > 0)
        res->doc = nb_html_parse(resp.body.data, resp.body.len);

    /* Collect CSS */
    {
        NbStr combined_css = nb_str_new();
        if (res->doc) {
            NbNode *stack[512]; int top = 0;
            stack[top++] = res->doc->root;
            while (top > 0) {
                NbNode *n = stack[--top];
                if (n->type == NB_NODE_ELEMENT) {
                    if (strcmp(n->tag, "style") == 0) {
                        for (NbNode *c = n->first_child; c; c = c->next_sibling)
                            if (c->type == NB_NODE_TEXT && c->text)
                                nb_str_appends(&combined_css, c->text);
                    }
                    if (strcmp(n->tag, "link") == 0) {
                        const char *rel  = nb_attr_val(n, "rel");
                        const char *href = nb_attr_val(n, "href");
                        if (rel && href && strstr(rel, "stylesheet")) {
                            char *css_url = nb_url_resolve(res->url, href);
                            NbHttpResponse cr = nb_http_get(css_url);
                            free(css_url);
                            if (!cr.error && cr.body.data)
                                nb_str_appends(&combined_css, cr.body.data);
                            nb_http_response_free(&cr);
                        }
                    }
                }
                for (NbNode *c = n->first_child; c; c = c->next_sibling)
                    if (top < 511) stack[top++] = c;
            }
        }
        if (combined_css.len > 0)
            res->css = nb_css_parse(combined_css.data, combined_css.len);
        nb_str_free(&combined_css);
    }

    nb_http_response_free(&resp);

done:
    g_idle_add(load_done_cb, res);
    free(job);
    return NULL;
}

void nb_browser_navigate(NbBrowser *b, NbTab *tab, const char *url) {
    if (!b || !tab || !url) return;

    char full_url[2048];
    if (strncmp(url,"http://",7)!=0 && strncmp(url,"https://",8)!=0 &&
        strncmp(url,"about:",6)!=0  && strncmp(url,"file:",5)!=0) {
        snprintf(full_url, sizeof(full_url), "https://%s", url);
    } else {
        strncpy(full_url, url, sizeof(full_url)-1);
    }

    /* History */
    if (b->history_pos < b->history_len - 1) {
        for (int i = b->history_pos + 1; i < b->history_len; i++) {
            free(b->history[i]); b->history[i] = NULL;
        }
        b->history_len = b->history_pos + 1;
    }
    if (b->history_len < 512) {
        free(b->history[b->history_len]);
        b->history[b->history_len++] = strdup(full_url);
        b->history_pos = b->history_len - 1;
    }

    tab->loading = 1;
    strncpy(tab->url, full_url, sizeof(tab->url)-1);
    nb_browser_queue_draw(b);

    LoadJob *job = calloc(1, sizeof(*job));
    job->browser = b; job->tab = tab;
    strncpy(job->url, full_url, sizeof(job->url)-1);
    pthread_t tid;
    pthread_create(&tid, NULL, load_thread, job);
    pthread_detach(tid);
}

void nb_browser_go_back(NbBrowser *b) {
    if (!b || b->history_pos <= 0 || !b->active_tab) return;
    b->history_pos--;
    LoadJob *job = calloc(1,sizeof(*job));
    job->browser=b; job->tab=b->active_tab;
    strncpy(job->url, b->history[b->history_pos], sizeof(job->url)-1);
    pthread_t tid; pthread_create(&tid,NULL,load_thread,job); pthread_detach(tid);
    b->active_tab->loading=1; nb_browser_queue_draw(b);
}

void nb_browser_go_forward(NbBrowser *b) {
    if (!b || b->history_pos >= b->history_len-1 || !b->active_tab) return;
    b->history_pos++;
    LoadJob *job = calloc(1,sizeof(*job));
    job->browser=b; job->tab=b->active_tab;
    strncpy(job->url, b->history[b->history_pos], sizeof(job->url)-1);
    pthread_t tid; pthread_create(&tid,NULL,load_thread,job); pthread_detach(tid);
    b->active_tab->loading=1; nb_browser_queue_draw(b);
}

void nb_browser_reload(NbBrowser *b) {
    if (!b || !b->active_tab) return;
    nb_browser_navigate(b, b->active_tab, b->active_tab->url);
}

NbTab *nb_browser_new_tab(NbBrowser *b, const char *url) {
    NbTab *t = tab_new(b, url);
    b->active_tab = t;
    if (url && url[0]) nb_browser_navigate(b, t, url);
    nb_browser_queue_draw(b);
    return t;
}

void nb_browser_close_tab(NbBrowser *b, NbTab *tab) {
    if (!b || !tab || b->tab_count <= 1) return;
    if (tab->prev) tab->prev->next = tab->next;
    else           b->tabs = tab->next;
    if (tab->next) tab->next->prev = tab->prev;
    b->tab_count--;
    if (b->active_tab == tab) {
        b->active_tab = tab->next ? tab->next : b->tabs;
        if (b->active_tab) {
            strncpy(b->url_edit, b->active_tab->url, sizeof(b->url_edit)-1);
            b->url_cursor = (int)strlen(b->url_edit);
        }
    }
    tab_free(tab);
    nb_browser_queue_draw(b);
}

void nb_browser_queue_draw(NbBrowser *b) {
    if (b && b->canvas) gtk_widget_queue_draw(b->canvas);
}

static void nav_cb_js(const char *url, void *userdata) {
    NbTab *tab = (NbTab*)userdata;
    (void)tab; (void)url;
}
static void repaint_cb_js(void *userdata) {
    NbTab *tab = (NbTab*)userdata;
    if (tab) {
        /* We need the browser pointer — would be better to store it on tab */
        /* For now we'll just skip repaint from JS — full impl would store b* on tab */
    }
}

/* ================================================================== */
/*  CAIRO DRAWING HELPERS                                              */
/* ================================================================== */

static void cr_set_rgb(cairo_t *cr, float r, float g, float b) {
    cairo_set_source_rgb(cr, r, g, b);
}
static void cr_set_rgba(cairo_t *cr, float r, float g, float b, float a) {
    cairo_set_source_rgba(cr, r, g, b, a);
}

/* Rounded rectangle path */
static void cr_round_rect(cairo_t *cr, float x, float y, float w, float h, float r) {
    if (r <= 0) { cairo_rectangle(cr, x, y, w, h); return; }
    cairo_move_to(cr, x+r, y);
    cairo_line_to(cr, x+w-r, y);
    cairo_arc(cr, x+w-r, y+r, r, -G_PI/2, 0);
    cairo_line_to(cr, x+w, y+h-r);
    cairo_arc(cr, x+w-r, y+h-r, r, 0, G_PI/2);
    cairo_line_to(cr, x+r, y+h);
    cairo_arc(cr, x+r, y+h-r, r, G_PI/2, G_PI);
    cairo_line_to(cr, x, y+r);
    cairo_arc(cr, x+r, y+r, r, G_PI, 3*G_PI/2);
    cairo_close_path(cr);
}

/* Draw text centred in a box, return pixel width of text */
static float cr_text_in_box(cairo_t *cr, float x, float y, float w, float h,
                              const char *text, float font_size,
                              const char *font, int bold,
                              float tr, float tg, float tb) {
    if (!text || !text[0]) return 0;
    PangoLayout *pl = pango_cairo_create_layout(cr);
    char desc_str[128];
    snprintf(desc_str, sizeof(desc_str), "%s %s %.0fpx", font, bold?"Bold":"", font_size);
    PangoFontDescription *fd = pango_font_description_from_string(desc_str);
    pango_layout_set_font_description(pl, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(pl, text, -1);
    if (w > 0) { pango_layout_set_width(pl, (int)(w * PANGO_SCALE)); pango_layout_set_ellipsize(pl, PANGO_ELLIPSIZE_END); }
    int pw, ph; pango_layout_get_pixel_size(pl, &pw, &ph);
    float tx = x + (w > 0 ? (w - pw) / 2.0f : 0);
    float ty = y + (h - ph) / 2.0f;
    cr_set_rgb(cr, tr, tg, tb);
    cairo_move_to(cr, tx, ty);
    pango_cairo_show_layout(cr, pl);
    g_object_unref(pl);
    return pw;
}

/* Left-aligned text, returns width */
static float cr_text_left(cairo_t *cr, float x, float y, float h,
                            const char *text, float font_size, const char *font,
                            float tr, float tg, float tb) {
    if (!text || !text[0]) return 0;
    PangoLayout *pl = pango_cairo_create_layout(cr);
    char ds[128]; snprintf(ds, sizeof(ds), "%s %.0fpx", font, font_size);
    PangoFontDescription *fd = pango_font_description_from_string(ds);
    pango_layout_set_font_description(pl, fd); pango_font_description_free(fd);
    pango_layout_set_text(pl, text, -1);
    int pw, ph; pango_layout_get_pixel_size(pl, &pw, &ph);
    cr_set_rgb(cr, tr, tg, tb);
    cairo_move_to(cr, x, y + (h - ph) / 2.0f);
    pango_cairo_show_layout(cr, pl);
    g_object_unref(pl);
    return (float)pw;
}

/* ================================================================== */
/*  HIT RECT REGISTRY                                                  */
/* ================================================================== */
static void hit_clear(NbBrowser *b) { b->hit_count = 0; }
static void hit_add(NbBrowser *b, NbZone zone, int tab_idx,
                    float x, float y, float w, float h) {
    if (b->hit_count >= NB_MAX_HIT_RECTS) return;
    NbHitRect *r    = &b->hit[b->hit_count++];
    r->zone         = zone;
    r->tab_index    = tab_idx;
    r->x = x; r->y = y; r->w = w; r->h = h;
}
static NbHitRect hit_find(NbBrowser *b, float x, float y) {
    /* iterate in reverse so topmost widget wins */
    for (int i = b->hit_count - 1; i >= 0; i--) {
        NbHitRect *r = &b->hit[i];
        if (x >= r->x && x < r->x+r->w && y >= r->y && y < r->y+r->h)
            return *r;
    }
    NbHitRect none = {ZONE_NONE, -1, 0,0,0,0};
    return none;
}

/* ================================================================== */
/*  ICON / SYMBOL DRAWING  (pure Cairo, no image files needed)        */
/* ================================================================== */
static void draw_icon_arrow_left(cairo_t *cr, float cx, float cy, float sz, float r, float g, float b, float a) {
    cr_set_rgba(cr, r,g,b,a);
    cairo_set_line_width(cr, sz*0.15f);
    cairo_move_to(cr, cx+sz*0.4f, cy-sz*0.35f);
    cairo_line_to(cr, cx-sz*0.2f, cy);
    cairo_line_to(cr, cx+sz*0.4f, cy+sz*0.35f);
    cairo_stroke(cr);
}
static void draw_icon_arrow_right(cairo_t *cr, float cx, float cy, float sz, float r, float g, float b, float a) {
    cr_set_rgba(cr, r,g,b,a);
    cairo_set_line_width(cr, sz*0.15f);
    cairo_move_to(cr, cx-sz*0.4f, cy-sz*0.35f);
    cairo_line_to(cr, cx+sz*0.2f, cy);
    cairo_line_to(cr, cx-sz*0.4f, cy+sz*0.35f);
    cairo_stroke(cr);
}
static void draw_icon_reload(cairo_t *cr, float cx, float cy, float sz, float r, float g, float b, float a) {
    cr_set_rgba(cr, r,g,b,a);
    cairo_set_line_width(cr, sz*0.14f);
    cairo_arc(cr, cx, cy, sz*0.38f, -2.2f, 1.2f);
    cairo_stroke(cr);
    /* arrowhead */
    float ax = cx + sz*0.38f*cosf(1.2f), ay = cy + sz*0.38f*sinf(1.2f);
    cairo_move_to(cr, ax-sz*0.18f, ay-sz*0.05f);
    cairo_line_to(cr, ax,           ay);
    cairo_line_to(cr, ax+sz*0.05f, ay-sz*0.2f);
    cairo_stroke(cr);
}
static void draw_icon_stop(cairo_t *cr, float cx, float cy, float sz, float r, float g, float b, float a) {
    cr_set_rgba(cr, r,g,b,a);
    cairo_set_line_width(cr, sz*0.15f);
    float h = sz*0.35f;
    cairo_move_to(cr, cx-h, cy-h); cairo_line_to(cr, cx+h, cy+h); cairo_stroke(cr);
    cairo_move_to(cr, cx+h, cy-h); cairo_line_to(cr, cx-h, cy+h); cairo_stroke(cr);
}
static void draw_icon_home(cairo_t *cr, float cx, float cy, float sz, float r, float g, float b, float a) {
    cr_set_rgba(cr, r,g,b,a);
    cairo_set_line_width(cr, sz*0.13f);
    float h = sz*0.38f;
    cairo_move_to(cr, cx-h, cy+h*0.1f);
    cairo_line_to(cr, cx, cy-h);
    cairo_line_to(cr, cx+h, cy+h*0.1f);
    cairo_stroke(cr);
    cairo_rectangle(cr, cx-h*0.55f, cy+h*0.1f, h*1.1f, h*0.9f);
    cairo_stroke(cr);
}
static void draw_icon_plus(cairo_t *cr, float cx, float cy, float sz, float r, float g, float b, float a) {
    cr_set_rgba(cr, r,g,b,a);
    cairo_set_line_width(cr, sz*0.15f);
    float h = sz*0.35f;
    cairo_move_to(cr, cx, cy-h); cairo_line_to(cr, cx, cy+h); cairo_stroke(cr);
    cairo_move_to(cr, cx-h, cy); cairo_line_to(cr, cx+h, cy); cairo_stroke(cr);
}
static void draw_icon_settings(cairo_t *cr, float cx, float cy, float sz, float r, float g, float b, float a) {
    cr_set_rgba(cr, r,g,b,a);
    cairo_set_line_width(cr, sz*0.12f);
    cairo_arc(cr, cx, cy, sz*0.22f, 0, 2*G_PI); cairo_stroke(cr);
    for (int i=0;i<8;i++) {
        float angle = i * G_PI/4;
        float x1 = cx + sz*0.28f*cosf(angle), y1 = cy + sz*0.28f*sinf(angle);
        float x2 = cx + sz*0.42f*cosf(angle), y2 = cy + sz*0.42f*sinf(angle);
        cairo_move_to(cr,x1,y1); cairo_line_to(cr,x2,y2); cairo_stroke(cr);
    }
}
static void draw_icon_close(cairo_t *cr, float cx, float cy, float sz, float r, float g, float b, float a) {
    cr_set_rgba(cr, r,g,b,a);
    cairo_set_line_width(cr, sz*0.14f);
    float h = sz*0.35f;
    cairo_move_to(cr, cx-h, cy-h); cairo_line_to(cr, cx+h, cy+h); cairo_stroke(cr);
    cairo_move_to(cr, cx+h, cy-h); cairo_line_to(cr, cx-h, cy+h); cairo_stroke(cr);
}
static void draw_icon_minimize(cairo_t *cr, float cx, float cy, float sz, float r, float g, float b, float a) {
    cr_set_rgba(cr, r,g,b,a);
    cairo_set_line_width(cr, sz*0.14f);
    float h = sz*0.3f;
    cairo_move_to(cr, cx-h, cy); cairo_line_to(cr, cx+h, cy); cairo_stroke(cr);
}
static void draw_icon_maximize(cairo_t *cr, float cx, float cy, float sz, float r, float g, float b, float a) {
    cr_set_rgba(cr, r,g,b,a);
    cairo_set_line_width(cr, sz*0.13f);
    float h = sz*0.3f;
    cairo_rectangle(cr, cx-h, cy-h, h*2, h*2); cairo_stroke(cr);
}
static void draw_icon_restore(cairo_t *cr, float cx, float cy, float sz, float r, float g, float b, float a) {
    cr_set_rgba(cr, r,g,b,a);
    cairo_set_line_width(cr, sz*0.12f);
    float h = sz*0.27f;
    cairo_rectangle(cr, cx-h+sz*0.07f, cy-h, h*2, h*2); cairo_stroke(cr);
    cairo_rectangle(cr, cx-h, cy-h+sz*0.07f, h*2, h*2); cairo_stroke(cr);
}
static void draw_icon_bookmark(cairo_t *cr, float cx, float cy, float sz, float r, float g, float b, float a) {
    cr_set_rgba(cr, r,g,b,a);
    cairo_set_line_width(cr, sz*0.12f);
    float hw = sz*0.28f, hh = sz*0.4f;
    cairo_move_to(cr, cx-hw, cy-hh);
    cairo_line_to(cr, cx+hw, cy-hh);
    cairo_line_to(cr, cx+hw, cy+hh);
    cairo_line_to(cr, cx, cy+hh*0.4f);
    cairo_line_to(cr, cx-hw, cy+hh);
    cairo_close_path(cr); cairo_stroke(cr);
}

/* ================================================================== */
/*  TOOLBAR BUTTON HELPER                                              */
/* ================================================================== */
static void draw_toolbar_btn(cairo_t *cr, NbBrowser *b, NbZone zone,
                               float x, float y, float w, float h,
                               int enabled, void (*icon_fn)(cairo_t*, float, float, float, float, float, float, float)) {
    NbTheme *t = &b->theme;
    float s = t->ui_scale;
    int hovered = (b->hover_zone == zone);

    if (hovered && enabled) {
        cr_set_rgba(cr, 1,1,1, 0.10f);
        cr_round_rect(cr, x, y, w, h, 4*s);
        cairo_fill(cr);
    }
    float ic = enabled ? 1.0f : 0.35f;
    float cx = x + w/2, cy = y + h/2, sz = fminf(w,h) * 0.45f;
    icon_fn(cr, cx, cy, sz, t->chrome_text_r*ic, t->chrome_text_g*ic, t->chrome_text_b*ic, 1.0f);
    hit_add(b, zone, -1, x, y, w, h);
}

/* ================================================================== */
/*  GEOMETRY CALCULATION                                               */
/* ================================================================== */
static void calc_geometry(NbBrowser *b) {
    NbTheme *t = &b->theme;
    float s = t->ui_scale;
    b->titlebar_h  = t->compact_toolbar ? 0 : 0;  /* merged into toolbar */
    b->tabbar_h    = 34 * s;
    b->toolbar_h   = t->compact_toolbar ? 36*s : 42*s;
    b->statusbar_h = t->show_status_bar ? 22*s : 0;
    /* titlebar is the row with window controls + tabs */
    b->titlebar_h  = b->tabbar_h;

    b->content_x = 0;
    b->content_y = b->titlebar_h + b->toolbar_h;
    b->content_w = b->win_w;
    b->content_h = b->win_h - b->content_y - b->statusbar_h;
    if (b->content_h < 0) b->content_h = 0;
}

/* ================================================================== */
/*  DRAW TITLEBAR (tabs + window controls merged)                     */
/* ================================================================== */
static void draw_titlebar(cairo_t *cr, NbBrowser *b) {
    NbTheme *t = &b->theme;
    float s = t->ui_scale;
    float W = b->win_w, H = b->titlebar_h;

    /* Background */
    cr_set_rgb(cr, t->chrome_r * 0.85f, t->chrome_g * 0.85f, t->chrome_b * 0.85f);
    cairo_rectangle(cr, 0, 0, W, H);
    cairo_fill(cr);

    /* Window control buttons — top right */
    float btn_sz = H * 0.70f;
    float btn_y  = (H - btn_sz) / 2;
    float margin = 6 * s;

    float close_x  = W - margin - btn_sz;
    float max_x    = close_x - btn_sz - margin * 0.5f;
    float min_x    = max_x  - btn_sz - margin * 0.5f;

    /* Close button */
    int hc = (b->hover_zone == ZONE_CLOSE);
    cr_set_rgba(cr, hc ? 0.9f : 0.55f, hc ? 0.2f : 0.55f, hc ? 0.2f : 0.55f, 1.0f);
    cr_round_rect(cr, close_x, btn_y, btn_sz, btn_sz, btn_sz/2);
    cairo_fill(cr);
    draw_icon_close(cr, close_x+btn_sz/2, btn_y+btn_sz/2, btn_sz, 1,1,1, 1);
    hit_add(b, ZONE_CLOSE, -1, close_x, btn_y, btn_sz, btn_sz);

    /* Maximize button */
    int hm = (b->hover_zone == ZONE_MAXIMIZE);
    cr_set_rgba(cr, hm ? 0.3f : 0.25f, hm ? 0.7f : 0.55f, hm ? 0.3f : 0.25f, 1.0f);
    cr_round_rect(cr, max_x, btn_y, btn_sz, btn_sz, btn_sz/2);
    cairo_fill(cr);
    if (b->maximized) draw_icon_restore(cr, max_x+btn_sz/2, btn_y+btn_sz/2, btn_sz, 1,1,1, 1);
    else              draw_icon_maximize(cr, max_x+btn_sz/2, btn_y+btn_sz/2, btn_sz, 1,1,1, 1);
    hit_add(b, ZONE_MAXIMIZE, -1, max_x, btn_y, btn_sz, btn_sz);

    /* Minimize button */
    int hn = (b->hover_zone == ZONE_MINIMIZE);
    cr_set_rgba(cr, hn ? 0.3f : 0.25f, hn ? 0.55f : 0.45f, hn ? 0.7f : 0.55f, 1.0f);
    cr_round_rect(cr, min_x, btn_y, btn_sz, btn_sz, btn_sz/2);
    cairo_fill(cr);
    draw_icon_minimize(cr, min_x+btn_sz/2, btn_y+btn_sz/2, btn_sz, 1,1,1, 1);
    hit_add(b, ZONE_MINIMIZE, -1, min_x, btn_y, btn_sz, btn_sz);

    /* TABS */
    float tab_area_end = min_x - margin * 2;
    float new_tab_btn_w = H * 0.80f;
    float new_tab_x = 4 * s;
    float tabs_x_start = new_tab_x + new_tab_btn_w + 4*s;
    float tabs_available = tab_area_end - tabs_x_start - 4*s;

    /* New tab button */
    int hnt = (b->hover_zone == ZONE_BTN_NEW_TAB);
    if (hnt) { cr_set_rgba(cr, 1,1,1,0.10f); cr_round_rect(cr, new_tab_x, btn_y, new_tab_btn_w, btn_sz, 4*s); cairo_fill(cr); }
    draw_icon_plus(cr, new_tab_x+new_tab_btn_w/2, btn_y+btn_sz/2, btn_sz,
                   t->chrome_text_r, t->chrome_text_g, t->chrome_text_b, 1.0f);
    hit_add(b, ZONE_BTN_NEW_TAB, -1, new_tab_x, btn_y, new_tab_btn_w, btn_sz);

    /* Draw tabs */
    int n = b->tab_count; if (n < 1) n = 1;
    float tab_w = fminf(200*s, tabs_available / n);
    float tx = tabs_x_start;
    int idx = 0;
    for (NbTab *tab = b->tabs; tab; tab = tab->next, idx++) {
        int is_active = (tab == b->active_tab);
        int is_hov    = (b->hover_zone == ZONE_TAB && b->hover_tab == idx);
        float ty = 3*s;
        float th = H - 3*s;

        /* Tab background */
        if (is_active) {
            cr_set_rgb(cr, t->chrome_r, t->chrome_g, t->chrome_b);
            cr_round_rect(cr, tx, ty, tab_w, th, 6*s);
            cairo_fill(cr);
            /* accent underline */
            cr_set_rgb(cr, t->accent_r, t->accent_g, t->accent_b);
            cairo_rectangle(cr, tx+4, ty+th-2.5f, tab_w-8, 2.5f);
            cairo_fill(cr);
        } else if (is_hov) {
            cr_set_rgba(cr, 1,1,1, 0.07f);
            cr_round_rect(cr, tx, ty, tab_w, th, 6*s);
            cairo_fill(cr);
        }

        /* Loading spinner (dot animation) or tab title */
        float title_x = tx + 8*s;
        float title_w = tab_w - 32*s;
        if (tab->loading) {
            /* simple "..." loading indicator */
            cr_text_in_box(cr, tx, ty, tab_w - 22*s, th, "...",
                           t->ui_font_size*s, t->ui_font, 0,
                           t->chrome_text_r*0.6f, t->chrome_text_g*0.6f, t->chrome_text_b*0.6f);
        } else {
            /* Clip title */
            cairo_save(cr);
            cairo_rectangle(cr, title_x, ty, title_w, th); cairo_clip(cr);
            cr_text_left(cr, title_x, ty, th, tab->title, t->ui_font_size*s, t->ui_font,
                          t->chrome_text_r, t->chrome_text_g, t->chrome_text_b);
            cairo_restore(cr);
        }

        /* Tab close button */
        float close_sz = 14*s;
        float close_bx = tx + tab_w - close_sz - 5*s;
        float close_by = ty + (th - close_sz) / 2;
        if (is_active || is_hov) {
            int hcl = (b->hover_zone == ZONE_TAB_CLOSE && b->hover_tab == idx);
            cr_set_rgba(cr, 1,1,1, hcl ? 0.25f : 0.12f);
            cr_round_rect(cr, close_bx, close_by, close_sz, close_sz, close_sz/2);
            cairo_fill(cr);
            draw_icon_close(cr, close_bx+close_sz/2, close_by+close_sz/2, close_sz,
                            t->chrome_text_r, t->chrome_text_g, t->chrome_text_b, 0.8f);
            hit_add(b, ZONE_TAB_CLOSE, idx, close_bx, close_by, close_sz, close_sz);
        }

        hit_add(b, ZONE_TAB, idx, tx, ty, tab_w, th);
        tab->tab_x = tx; tab->tab_w = tab_w;
        tx += tab_w + 2*s;
    }

    /* Titlebar drag area (everything not covered by buttons/tabs gets registered as draggable) */
    hit_add(b, ZONE_TITLEBAR, -1, 0, 0, W, H);
}

/* ================================================================== */
/*  DRAW TOOLBAR                                                       */
/* ================================================================== */
static void draw_toolbar(cairo_t *cr, NbBrowser *b) {
    NbTheme *t = &b->theme;
    float s = t->ui_scale;
    float y0 = b->titlebar_h;
    float W  = b->win_w;
    float H  = b->toolbar_h;

    /* Background */
    cr_set_rgb(cr, t->chrome_r, t->chrome_g, t->chrome_b);
    cairo_rectangle(cr, 0, y0, W, H);
    cairo_fill(cr);

    /* Separator line at bottom */
    cr_set_rgba(cr, 0,0,0, 0.25f);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, 0, y0+H-0.5f);
    cairo_line_to(cr, W, y0+H-0.5f);
    cairo_stroke(cr);

    float btn_sz  = H * 0.60f;
    float btn_pad = (H - btn_sz) / 2;
    float x = 6*s;
    float by = y0 + btn_pad;
    float bw = btn_sz * 1.35f;

    int can_back    = b->history_pos > 0;
    int can_forward = b->history_pos < b->history_len - 1;
    int is_loading  = b->active_tab && b->active_tab->loading;

    /* Back */
    draw_toolbar_btn(cr, b, ZONE_BTN_BACK,    x, by, bw, btn_sz, can_back, draw_icon_arrow_left); x += bw + 2*s;
    /* Forward */
    draw_toolbar_btn(cr, b, ZONE_BTN_FORWARD, x, by, bw, btn_sz, can_forward, draw_icon_arrow_right); x += bw + 2*s;
    /* Reload/Stop */
    if (is_loading)
        draw_toolbar_btn(cr, b, ZONE_BTN_STOP,   x, by, bw, btn_sz, 1, draw_icon_stop);
    else
        draw_toolbar_btn(cr, b, ZONE_BTN_RELOAD, x, by, bw, btn_sz, 1, draw_icon_reload);
    x += bw + 2*s;
    /* Home */
    draw_toolbar_btn(cr, b, ZONE_BTN_HOME, x, by, bw, btn_sz, 1, draw_icon_home); x += bw + 8*s;

    /* URL BAR */
    float right_btns_w = (bw + 2*s) * 4 + 8*s; /* bookmark+settings+zoom3 */
    float url_w = W - x - right_btns_w - 8*s;
    float url_h = H * 0.68f;
    float url_y = y0 + (H - url_h) / 2;

    /* URL bar background */
    int url_hov = (b->hover_zone == ZONE_URL_BAR);
    float url_bg = url_hov || b->url_focused ? 0.30f : 0.22f;
    cr_set_rgba(cr, url_bg, url_bg, url_bg*1.1f, 1.0f);
    cr_round_rect(cr, x, url_y, url_w, url_h, url_h/2);
    cairo_fill(cr);

    /* URL bar focus ring */
    if (b->url_focused) {
        cr_set_rgba(cr, t->accent_r, t->accent_g, t->accent_b, 0.7f);
        cairo_set_line_width(cr, 1.5f);
        cr_round_rect(cr, x, url_y, url_w, url_h, url_h/2);
        cairo_stroke(cr);
    }

    /* URL text / placeholder */
    const char *url_text = b->url_edit[0] ? b->url_edit :
                           (b->active_tab  ? b->active_tab->url : "");
    cairo_save(cr);
    float pad = url_h * 0.25f;
    cairo_rectangle(cr, x+pad, url_y, url_w-pad*2, url_h); cairo_clip(cr);
    if (!b->url_focused && url_text[0] == '\0') {
        cr_text_left(cr, x+pad, url_y, url_h, "Enter address...",
                     t->ui_font_size*s, t->ui_font, 0.5f,0.5f,0.5f);
    } else {
        cr_text_left(cr, x+pad, url_y, url_h, url_text,
                     t->ui_font_size*s, t->ui_font,
                     t->chrome_text_r, t->chrome_text_g, t->chrome_text_b);
        /* Caret */
        if (b->url_focused) {
            /* measure text up to cursor */
            PangoLayout *pl = pango_cairo_create_layout(cr);
            char ds[128]; snprintf(ds, sizeof(ds), "%s %.0fpx", t->ui_font, t->ui_font_size*s);
            PangoFontDescription *fd = pango_font_description_from_string(ds);
            pango_layout_set_font_description(pl, fd); pango_font_description_free(fd);
            char tmp[2048]; int cc = b->url_cursor; if(cc>(int)strlen(url_text)) cc=strlen(url_text);
            memcpy(tmp, url_text, cc); tmp[cc]=0;
            pango_layout_set_text(pl, tmp, -1);
            int pw,ph; pango_layout_get_pixel_size(pl,&pw,&ph);
            g_object_unref(pl);
            float cx2 = x+pad+pw;
            cr_set_rgba(cr, t->chrome_text_r, t->chrome_text_g, t->chrome_text_b, 0.9f);
            cairo_set_line_width(cr, 1.5f);
            cairo_move_to(cr, cx2, url_y + (url_h-ph)/2 + 1);
            cairo_line_to(cr, cx2, url_y + (url_h-ph)/2 + ph - 1);
            cairo_stroke(cr);
        }
    }
    cairo_restore(cr);
    hit_add(b, ZONE_URL_BAR, -1, x, url_y, url_w, url_h);
    x += url_w + 8*s;

    /* Right-side buttons — zoom out (minus icon) */
    {
        int hzout = (b->hover_zone == ZONE_BTN_ZOOM_OUT);
        if (hzout) {
            cr_set_rgba(cr, 1,1,1,0.10f);
            cr_round_rect(cr, x, by, bw, btn_sz, 4*s);
            cairo_fill(cr);
        }
        float cx2=x+bw/2, cy2=by+btn_sz/2, mz=btn_sz*0.35f;
        cr_set_rgba(cr, t->chrome_text_r, t->chrome_text_g, t->chrome_text_b, 1.0f);
        cairo_set_line_width(cr, btn_sz*0.14f);
        cairo_move_to(cr,cx2-mz,cy2); cairo_line_to(cr,cx2+mz,cy2); cairo_stroke(cr);
        hit_add(b, ZONE_BTN_ZOOM_OUT, -1, x, by, bw, btn_sz);
    }
    x += bw + 2*s;

    /* Zoom label */
    char zoom_str[16];
    float zoom = b->active_tab ? b->active_tab->render_state.scale : 1.0f;
    snprintf(zoom_str, sizeof(zoom_str), "%.0f%%", zoom*100);
    float zlw = 38*s;
    cr_text_in_box(cr, x, by, zlw, btn_sz, zoom_str,
                   t->ui_font_size*0.9f*s, t->ui_font, 0,
                   t->chrome_text_r, t->chrome_text_g, t->chrome_text_b);
    hit_add(b, ZONE_BTN_ZOOM_RESET, -1, x, by, zlw, btn_sz);
    x += zlw + 2*s;

    draw_toolbar_btn(cr, b, ZONE_BTN_ZOOM_IN, x, by, bw, btn_sz, 1, draw_icon_plus); x += bw + 6*s;
    draw_toolbar_btn(cr, b, ZONE_BTN_BOOKMARK, x, by, bw, btn_sz, 1, draw_icon_bookmark); x += bw + 2*s;
    draw_toolbar_btn(cr, b, ZONE_BTN_SETTINGS, x, by, bw, btn_sz, 1, draw_icon_settings);
}

/* ================================================================== */
/*  SETTINGS PANEL                                                     */
/* ================================================================== */
/* A simple slider helper — returns new value if dragged */
static void draw_slider(cairo_t *cr, NbBrowser *b, float x, float y, float w,
                         float val, float vmin, float vmax,
                         const char *label, float s,
                         float tr, float tg, float tb) {
    /* Track */
    cr_set_rgba(cr, tr*0.3f, tg*0.3f, tb*0.3f, 1.0f);
    cr_round_rect(cr, x, y+8*s, w, 4*s, 2*s);
    cairo_fill(cr);
    /* Fill */
    float fill_w = w * (val - vmin) / (vmax - vmin);
    cr_set_rgba(cr, 0.2f, 0.5f, 1.0f, 1.0f);
    cr_round_rect(cr, x, y+8*s, fill_w, 4*s, 2*s);
    cairo_fill(cr);
    /* Knob */
    float kx = x + fill_w;
    cr_set_rgba(cr, 1,1,1, 1.0f);
    cairo_arc(cr, kx, y+10*s, 7*s, 0, 2*G_PI);
    cairo_fill(cr);
    /* Label */
    char lbl[64]; snprintf(lbl, sizeof(lbl), "%s: %.2f", label, val);
    cr_text_left(cr, x, y - 2*s, 16*s, lbl, 11*s, "Sans", tr, tg, tb);
}

static void draw_color_swatch(cairo_t *cr, float x, float y, float w, float h,
                               float r, float g, float b2, float br, float bg, float bb) {
    cr_set_rgba(cr, r, g, b2, 1.0f);
    cr_round_rect(cr, x, y, w, h, 4);
    cairo_fill(cr);
    cr_set_rgba(cr, br, bg, bb, 0.5f);
    cairo_set_line_width(cr, 1);
    cr_round_rect(cr, x, y, w, h, 4);
    cairo_stroke(cr);
}

static void draw_settings_panel(cairo_t *cr, NbBrowser *b) {
    NbTheme *t = &b->theme;
    float s = t->ui_scale;
    float W = b->win_w, H = b->win_h;

    /* Dim overlay */
    cr_set_rgba(cr, 0,0,0, 0.55f);
    cairo_rectangle(cr, 0, 0, W, H);
    cairo_fill(cr);

    /* Panel */
    float pw = fminf(520*s, W - 40*s);
    float ph = fminf(640*s, H - 40*s);
    float px = (W - pw) / 2;
    float py = (H - ph) / 2;

    cr_set_rgb(cr, t->chrome_r + 0.05f, t->chrome_g + 0.05f, t->chrome_b + 0.08f);
    cr_round_rect(cr, px, py, pw, ph, 12*s);
    cairo_fill(cr);

    /* Panel border */
    cr_set_rgba(cr, 1,1,1, 0.10f);
    cairo_set_line_width(cr, 1);
    cr_round_rect(cr, px, py, pw, ph, 12*s);
    cairo_stroke(cr);

    /* Close button */
    float cbx = px + pw - 28*s, cby = py + 8*s, cbsz = 20*s;
    int hc = (b->hover_zone == ZONE_SETTINGS_CLOSE);
    cr_set_rgba(cr, hc?0.9f:0.5f, hc?0.2f:0.5f, hc?0.2f:0.5f, 1.0f);
    cr_round_rect(cr, cbx, cby, cbsz, cbsz, cbsz/2);
    cairo_fill(cr);
    {
        float h2 = cbsz*0.3f;
        cr_set_rgba(cr, 1,1,1,1);
        cairo_set_line_width(cr, 1.5f);
        cairo_move_to(cr, cbx+cbsz/2-h2, cby+cbsz/2-h2);
        cairo_line_to(cr, cbx+cbsz/2+h2, cby+cbsz/2+h2); cairo_stroke(cr);
        cairo_move_to(cr, cbx+cbsz/2+h2, cby+cbsz/2-h2);
        cairo_line_to(cr, cbx+cbsz/2-h2, cby+cbsz/2+h2); cairo_stroke(cr);
    }
    hit_add(b, ZONE_SETTINGS_CLOSE, -1, cbx, cby, cbsz, cbsz);

    /* Title */
    cr_text_in_box(cr, px, py+6*s, pw-30*s, 28*s, "Nishant Browser — Settings",
                   15*s, "Sans", 1,
                   t->chrome_text_r, t->chrome_text_g, t->chrome_text_b);

    /* Sections */
    float lx = px + 20*s, row = py + 44*s, lw = pw - 40*s;
    float tr2 = t->chrome_text_r, tg2 = t->chrome_text_g, tb2 = t->chrome_text_b;

    /* --- Section: Appearance --- */
    cr_set_rgba(cr, t->accent_r, t->accent_g, t->accent_b, 0.8f);
    cairo_rectangle(cr, lx, row, lw, 1.5f); cairo_fill(cr); row += 6*s;
    cr_text_left(cr, lx, row, 18*s, "Appearance", 13*s, "Sans Bold", tr2, tg2, tb2); row += 22*s;

    draw_slider(cr, b, lx, row, lw*0.7f, t->ui_scale, 0.5f, 2.0f, "UI Scale", s, tr2, tg2, tb2);
    row += 28*s;
    draw_slider(cr, b, lx, row, lw*0.7f, t->ui_font_size, 8.0f, 20.0f, "Font Size", s, tr2, tg2, tb2);
    row += 28*s;
    draw_slider(cr, b, lx, row, lw*0.7f, (float)t->corner_radius, 0.0f, 20.0f, "Corner Radius", s, tr2, tg2, tb2);
    row += 32*s;

    /* Colour presets */
    cr_text_left(cr, lx, row, 16*s, "Theme Presets:", 11*s, "Sans", tr2*0.8f, tg2*0.8f, tb2*0.8f);
    row += 18*s;
    struct { const char *name; float r,g,b; } presets[] = {
        {"Dark",      0.13f,0.13f,0.13f},
        {"Blue",      0.10f,0.14f,0.22f},
        {"Purple",    0.15f,0.10f,0.22f},
        {"Carbon",    0.09f,0.09f,0.09f},
        {"Solarized", 0.00f,0.17f,0.21f},
        {NULL,0,0,0}
    };
    float sw_w = 70*s, sw_h = 24*s, sw_gap = 6*s, sw_x = lx;
    for (int i = 0; presets[i].name; i++) {
        draw_color_swatch(cr, sw_x, row, sw_w, sw_h,
                          presets[i].r, presets[i].g, presets[i].b,
                          tr2*0.4f, tg2*0.4f, tb2*0.4f);
        cr_text_in_box(cr, sw_x, row, sw_w, sw_h, presets[i].name,
                       9*s, "Sans", 0, tr2, tg2, tb2);
        sw_x += sw_w + sw_gap;
    }
    row += sw_h + 16*s;

    /* --- Section: Toolbar --- */
    cr_set_rgba(cr, t->accent_r, t->accent_g, t->accent_b, 0.8f);
    cairo_rectangle(cr, lx, row, lw, 1.5f); cairo_fill(cr); row += 6*s;
    cr_text_left(cr, lx, row, 18*s, "Toolbar", 13*s, "Sans Bold", tr2, tg2, tb2); row += 22*s;

    /* Checkboxes */
    struct { const char *label; int *val; } checks[] = {
        {"Compact toolbar (thinner height)", &t->compact_toolbar},
        {"Show status bar",                  &t->show_status_bar},
        {"Show bookmarks bar",               &t->show_bookmarks_bar},
        {NULL, NULL}
    };
    for (int i = 0; checks[i].label; i++) {
        int v = *checks[i].val;
        /* Box */
        cr_set_rgba(cr, v ? t->accent_r : 0.3f, v ? t->accent_g : 0.3f, v ? t->accent_b : 0.3f, 1.0f);
        cr_round_rect(cr, lx, row+1*s, 14*s, 14*s, 3*s); cairo_fill(cr);
        if (v) {
            cr_set_rgba(cr, 1,1,1, 1.0f); cairo_set_line_width(cr, 1.5f);
            cairo_move_to(cr, lx+3*s, row+8*s);
            cairo_line_to(cr, lx+6*s, row+12*s);
            cairo_line_to(cr, lx+12*s, row+3*s); cairo_stroke(cr);
        }
        cr_text_left(cr, lx+20*s, row, 18*s, checks[i].label, 11*s, "Sans", tr2*0.9f, tg2*0.9f, tb2*0.9f);
        row += 20*s;
    }
    row += 8*s;

    /* --- Section: Content --- */
    cr_set_rgba(cr, t->accent_r, t->accent_g, t->accent_b, 0.8f);
    cairo_rectangle(cr, lx, row, lw, 1.5f); cairo_fill(cr); row += 6*s;
    cr_text_left(cr, lx, row, 18*s, "Content", 13*s, "Sans Bold", tr2, tg2, tb2); row += 22*s;
    draw_slider(cr, b, lx, row, lw*0.7f,
                b->active_tab ? b->active_tab->render_state.scale : 1.0f,
                0.25f, 3.0f, "Page Zoom", s, tr2, tg2, tb2);
    row += 32*s;

    /* --- Section: About --- */
    cr_set_rgba(cr, t->chrome_text_r*0.4f, t->chrome_text_g*0.4f, t->chrome_text_b*0.4f, 0.6f);
    cairo_rectangle(cr, lx, row, lw, 1.0f); cairo_fill(cr); row += 8*s;
    cr_text_left(cr, lx, row, 14*s, "Nishant Browser v1.0 — Custom engine, zero Chrome, zero Firefox.",
                 9*s, "Sans", tr2*0.5f, tg2*0.5f, tb2*0.5f);
}

/* ================================================================== */
/*  STATUS BAR                                                         */
/* ================================================================== */
static void draw_status_bar(cairo_t *cr, NbBrowser *b) {
    if (!b->theme.show_status_bar) return;
    NbTheme *t = &b->theme;
    float s = t->ui_scale;
    float y0 = b->win_h - b->statusbar_h;

    cr_set_rgb(cr, t->chrome_r * 0.80f, t->chrome_g * 0.80f, t->chrome_b * 0.80f);
    cairo_rectangle(cr, 0, y0, b->win_w, b->statusbar_h);
    cairo_fill(cr);

    cr_set_rgba(cr, 0,0,0, 0.2f);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, 0, y0+0.5f);
    cairo_line_to(cr, b->win_w, y0+0.5f);
    cairo_stroke(cr);

    const char *status = "Ready";
    if (b->active_tab) {
        if (b->active_tab->loading)   status = "Loading...";
        else if (b->active_tab->error_msg[0]) status = b->active_tab->error_msg;
        else if (b->active_tab->url[0]) status = b->active_tab->url;
    }
    cr_text_left(cr, 8*s, y0, b->statusbar_h, status,
                 9.5f*s, t->ui_font, t->chrome_text_r*0.7f, t->chrome_text_g*0.7f, t->chrome_text_b*0.7f);

    /* Right side: zoom info */
    float zoom = b->active_tab ? b->active_tab->render_state.scale : 1.0f;
    char zs[16]; snprintf(zs, sizeof(zs), "%.0f%%", zoom*100);
    cr_text_in_box(cr, b->win_w - 50*s, y0, 48*s, b->statusbar_h, zs,
                   9.5f*s, t->ui_font, 0,
                   t->chrome_text_r*0.7f, t->chrome_text_g*0.7f, t->chrome_text_b*0.7f);
}

/* ================================================================== */
/*  CONTENT AREA — page viewport                                       */
/* ================================================================== */
static void draw_content(cairo_t *cr, NbBrowser *b) {
    NbTheme *t = &b->theme;
    float cx = b->content_x, cy = b->content_y;
    float cw = b->content_w, ch = b->content_h;
    if (cw <= 0 || ch <= 0) return;

    /* Clip to content area */
    cairo_save(cr);
    cairo_rectangle(cr, cx, cy, cw, ch);
    cairo_clip(cr);
    cairo_translate(cr, cx, cy);

    NbTab *tab = b->active_tab;

    if (!tab) {
        cr_set_rgb(cr, t->page_bg_r, t->page_bg_g, t->page_bg_b);
        cairo_paint(cr);
        cairo_restore(cr);
        return;
    }

    if (tab->loading) {
        cr_set_rgb(cr, t->page_bg_r, t->page_bg_g, t->page_bg_b);
        cairo_paint(cr);
        /* Centered loading message */
        cr_text_in_box(cr, 0, 0, cw, ch, "Loading...", 16, "Sans", 0,
                       t->chrome_text_r*0.5f, t->chrome_text_g*0.5f, t->chrome_text_b*0.5f);
        cairo_restore(cr);
        return;
    }

    if (tab->error_msg[0]) {
        nb_render_error_page(cr, cw, ch, tab->error_msg);
        cairo_restore(cr);
        return;
    }

    if (!tab->layout) {
        cr_set_rgb(cr, 1,1,1);
        cairo_paint(cr);
        cairo_restore(cr);
        return;
    }

    /* Reflow if width changed */
    if (fabsf(tab->layout->viewport_width - cw) > 1.0f)
        nb_layout_reflow(tab->layout, cw, ch);

    tab->render_state.scroll_x = b->scroll_x;
    tab->render_state.scroll_y = b->scroll_y;
    nb_render_paint(cr, tab->layout, &tab->render_state);

    hit_add(b, ZONE_CONTENT, -1, 0, 0, cw, ch);

    cairo_restore(cr);
}

/* ================================================================== */
/*  MAIN DRAW CALLBACK                                                 */
/* ================================================================== */
static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
    NbBrowser *b = (NbBrowser*)data;
    (void)w;

    calc_geometry(b);
    hit_clear(b);

    /* Window rounded corners clip */
    int rad = b->maximized ? 0 : b->theme.corner_radius;
    cr_round_rect(cr, 0, 0, b->win_w, b->win_h, rad);
    cairo_clip(cr);

    /* Content first (bottom layer) */
    draw_content(cr, b);

    /* Chrome on top */
    draw_titlebar(cr, b);
    draw_toolbar(cr, b);
    draw_status_bar(cr, b);

    /* Settings overlay on top of everything */
    if (b->settings_open)
        draw_settings_panel(cr, b);

    /* Thin window border for non-maximized */
    if (!b->maximized) {
        cr_set_rgba(cr, 1,1,1, 0.08f);
        cairo_set_line_width(cr, 1);
        cr_round_rect(cr, 0.5f, 0.5f, b->win_w-1, b->win_h-1, rad);
        cairo_stroke(cr);
    }

    return FALSE;
}

/* ================================================================== */
/*  MOUSE MOTION                                                       */
/* ================================================================== */
static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, gpointer data) {
    NbBrowser *b = (NbBrowser*)data;
    float mx = (float)ev->x, my = (float)ev->y;

    /* Window drag */
    if (b->dragging && !b->maximized) {
        int rx, ry;
        gdk_window_get_root_origin(gtk_widget_get_window(b->window), &rx, &ry);
        int nx = rx + (int)(mx - b->drag_start_x);
        int ny = ry + (int)(my - b->drag_start_y);
        gtk_window_move(GTK_WINDOW(b->window), nx, ny);
        return TRUE;
    }

    /* Update hover zone */
    NbHitRect h = hit_find(b, mx, my);
    NbZone old_zone = b->hover_zone;
    int old_tab = b->hover_tab;
    b->hover_zone = h.zone;
    b->hover_tab  = h.tab_index;

    /* Cursor shape */
    GdkCursor *cur = NULL;
    GdkDisplay *disp = gdk_display_get_default();
    switch (h.zone) {
        case ZONE_URL_BAR:
            cur = gdk_cursor_new_from_name(disp, "text"); break;
        case ZONE_CONTENT:
            cur = gdk_cursor_new_from_name(disp, "default"); break;
        case ZONE_CLOSE: case ZONE_MAXIMIZE: case ZONE_MINIMIZE:
        case ZONE_TAB:   case ZONE_TAB_CLOSE: case ZONE_BTN_BACK:
        case ZONE_BTN_FORWARD: case ZONE_BTN_RELOAD: case ZONE_BTN_STOP:
        case ZONE_BTN_HOME: case ZONE_BTN_NEW_TAB: case ZONE_BTN_ZOOM_IN:
        case ZONE_BTN_ZOOM_OUT: case ZONE_BTN_BOOKMARK: case ZONE_BTN_SETTINGS:
        case ZONE_SETTINGS_CLOSE:
            cur = gdk_cursor_new_from_name(disp, "pointer"); break;
        default:
            cur = gdk_cursor_new_from_name(disp, "default"); break;
    }
    if (cur) {
        gdk_window_set_cursor(gtk_widget_get_window(b->window), cur);
        g_object_unref(cur);
    }

    if (old_zone != b->hover_zone || old_tab != b->hover_tab)
        nb_browser_queue_draw(b);
    return FALSE;
}

/* ================================================================== */
/*  MOUSE BUTTON PRESS                                                 */
/* ================================================================== */
static void navigate_active(NbBrowser *b, const char *url) {
    if (!b->active_tab) nb_browser_new_tab(b, url);
    else                nb_browser_navigate(b, b->active_tab, url);
}

static void apply_theme_preset(NbBrowser *b, float r, float g, float bl) {
    NbTheme *t = &b->theme;
    t->chrome_r = r; t->chrome_g = g; t->chrome_b = bl;
    /* accent stays the same */
    nb_browser_queue_draw(b);
}

static gboolean on_button_press(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    NbBrowser *b = (NbBrowser*)data;
    if (ev->button != 1) return FALSE;
    float mx = (float)ev->x, my = (float)ev->y;

    /* Double-click titlebar to maximize/restore */
    if (ev->type == GDK_2BUTTON_PRESS) {
        NbHitRect h = hit_find(b, mx, my);
        if (h.zone == ZONE_TITLEBAR) {
            if (b->maximized) gtk_window_unmaximize(GTK_WINDOW(b->window));
            else              gtk_window_maximize(GTK_WINDOW(b->window));
            return TRUE;
        }
    }

    NbHitRect h = hit_find(b, mx, my);

    switch (h.zone) {

    case ZONE_CLOSE:
        gtk_main_quit();
        return TRUE;

    case ZONE_MAXIMIZE:
        if (b->maximized) gtk_window_unmaximize(GTK_WINDOW(b->window));
        else              gtk_window_maximize(GTK_WINDOW(b->window));
        return TRUE;

    case ZONE_MINIMIZE:
        gtk_window_iconify(GTK_WINDOW(b->window));
        return TRUE;

    case ZONE_TITLEBAR:
        /* Start window drag */
        b->dragging = 1;
        b->drag_start_x = (int)mx;
        b->drag_start_y = (int)my;
        gtk_widget_grab_focus(b->canvas);
        return TRUE;

    case ZONE_TAB_CLOSE: {
        int idx = 0;
        for (NbTab *t2 = b->tabs; t2; t2 = t2->next, idx++)
            if (idx == h.tab_index) { nb_browser_close_tab(b, t2); break; }
        return TRUE;
    }

    case ZONE_TAB: {
        int idx = 0;
        for (NbTab *t2 = b->tabs; t2; t2 = t2->next, idx++) {
            if (idx == h.tab_index) {
                b->active_tab = t2;
                b->scroll_x = b->scroll_y = 0;
                strncpy(b->url_edit, t2->url, sizeof(b->url_edit)-1);
                b->url_cursor = (int)strlen(b->url_edit);
                nb_browser_queue_draw(b);
                break;
            }
        }
        return TRUE;
    }

    case ZONE_BTN_NEW_TAB:
        nb_browser_new_tab(b, "about:blank");
        return TRUE;

    case ZONE_BTN_BACK:
        nb_browser_go_back(b);
        return TRUE;

    case ZONE_BTN_FORWARD:
        nb_browser_go_forward(b);
        return TRUE;

    case ZONE_BTN_RELOAD:
        nb_browser_reload(b);
        return TRUE;

    case ZONE_BTN_STOP:
        if (b->active_tab) { b->active_tab->loading = 0; nb_browser_queue_draw(b); }
        return TRUE;

    case ZONE_BTN_HOME:
        navigate_active(b, b->home_url);
        return TRUE;

    case ZONE_BTN_ZOOM_IN:
        if (b->active_tab) {
            b->active_tab->render_state.scale += 0.1f;
            nb_browser_queue_draw(b);
        }
        return TRUE;

    case ZONE_BTN_ZOOM_OUT:
        if (b->active_tab && b->active_tab->render_state.scale > 0.2f) {
            b->active_tab->render_state.scale -= 0.1f;
            nb_browser_queue_draw(b);
        }
        return TRUE;

    case ZONE_BTN_ZOOM_RESET:
        if (b->active_tab) {
            b->active_tab->render_state.scale = 1.0f;
            nb_browser_queue_draw(b);
        }
        return TRUE;

    case ZONE_BTN_SETTINGS:
        b->settings_open = !b->settings_open;
        nb_browser_queue_draw(b);
        return TRUE;

    case ZONE_SETTINGS_CLOSE:
        b->settings_open = 0;
        /* Save settings */
        {
            char cfg[512];
            const char *home = g_get_home_dir();
            snprintf(cfg, sizeof(cfg), "%s/.nishant-browser.conf", home);
            nb_theme_save(&b->theme, cfg);
        }
        nb_browser_queue_draw(b);
        return TRUE;

    case ZONE_URL_BAR:
        b->url_focused = 1;
        gtk_widget_grab_focus(b->canvas);
        /* Select all on first click */
        b->url_sel_start = 0;
        b->url_sel_end   = (int)strlen(b->url_edit);
        b->url_cursor    = b->url_sel_end;
        nb_browser_queue_draw(b);
        return TRUE;

    case ZONE_CONTENT:
        b->url_focused = 0;
        /* Click in content — hit test for links */
        if (b->active_tab && b->active_tab->layout) {
            float cx2 = mx - b->content_x + b->scroll_x;
            float cy2 = my - b->content_y + b->scroll_y;
            NbBox *box = nb_layout_box_at(b->active_tab->layout, cx2, cy2);
            if (box && box->node) {
                NbNode *n = box->node;
                while (n && n->type == NB_NODE_ELEMENT) {
                    if (strcmp(n->tag, "a") == 0) {
                        const char *href = nb_attr_val(n, "href");
                        if (href && href[0]) {
                            char *full = nb_url_resolve(b->active_tab->url, href);
                            navigate_active(b, full);
                            free(full);
                        }
                        break;
                    }
                    if (strcmp(n->tag, "button") == 0 || strcmp(n->tag, "input") == 0) {
                        if (b->active_tab->js) nb_js_fire_event(b->active_tab->js, n, "click");
                        break;
                    }
                    n = n->parent;
                }
            }
        }
        nb_browser_queue_draw(b);
        return TRUE;

    default:
        b->url_focused = 0;
        nb_browser_queue_draw(b);
        return FALSE;
    }
}

static gboolean on_button_release(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    NbBrowser *b = (NbBrowser*)data;
    b->dragging = 0;
    return FALSE;
}

/* ================================================================== */
/*  KEYBOARD                                                           */
/* ================================================================== */
static gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer data) {
    NbBrowser *b = (NbBrowser*)data;
    gboolean ctrl = (ev->state & GDK_CONTROL_MASK) != 0;

    /* URL bar editing */
    if (b->url_focused) {
        int len = (int)strlen(b->url_edit);
        switch (ev->keyval) {
        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
            b->url_focused = 0;
            navigate_active(b, b->url_edit);
            return TRUE;
        case GDK_KEY_Escape:
            /* restore current URL */
            if (b->active_tab)
                strncpy(b->url_edit, b->active_tab->url, sizeof(b->url_edit)-1);
            b->url_focused = 0;
            nb_browser_queue_draw(b);
            return TRUE;
        case GDK_KEY_BackSpace:
            if (b->url_cursor > 0) {
                memmove(b->url_edit + b->url_cursor - 1,
                        b->url_edit + b->url_cursor,
                        len - b->url_cursor + 1);
                b->url_cursor--;
            }
            nb_browser_queue_draw(b);
            return TRUE;
        case GDK_KEY_Delete:
            if (b->url_cursor < len) {
                memmove(b->url_edit + b->url_cursor,
                        b->url_edit + b->url_cursor + 1,
                        len - b->url_cursor);
            }
            nb_browser_queue_draw(b);
            return TRUE;
        case GDK_KEY_Left:
            if (b->url_cursor > 0) b->url_cursor--;
            nb_browser_queue_draw(b); return TRUE;
        case GDK_KEY_Right:
            if (b->url_cursor < len) b->url_cursor++;
            nb_browser_queue_draw(b); return TRUE;
        case GDK_KEY_Home:  b->url_cursor = 0;   nb_browser_queue_draw(b); return TRUE;
        case GDK_KEY_End:   b->url_cursor = len; nb_browser_queue_draw(b); return TRUE;
        default:
            if (ctrl && ev->keyval == GDK_KEY_a) {
                b->url_sel_start = 0; b->url_sel_end = len; b->url_cursor = len;
                nb_browser_queue_draw(b); return TRUE;
            }
            if (ctrl && ev->keyval == GDK_KEY_c) return TRUE; /* TODO: clipboard */
            if (ctrl && ev->keyval == GDK_KEY_v) return TRUE; /* TODO: paste */
            /* Printable character */
            if (ev->string && ev->string[0] >= 0x20) {
                int ins_len = (int)strlen(ev->string);
                if (len + ins_len < (int)sizeof(b->url_edit) - 1) {
                    memmove(b->url_edit + b->url_cursor + ins_len,
                            b->url_edit + b->url_cursor,
                            len - b->url_cursor + 1);
                    memcpy(b->url_edit + b->url_cursor, ev->string, ins_len);
                    b->url_cursor += ins_len;
                }
                nb_browser_queue_draw(b);
                return TRUE;
            }
        }
    }

    /* Global shortcuts */
    if (ctrl) {
        switch (ev->keyval) {
        case GDK_KEY_l:
            b->url_focused = 1;
            b->url_sel_start = 0;
            b->url_sel_end = (int)strlen(b->url_edit);
            b->url_cursor  = b->url_sel_end;
            nb_browser_queue_draw(b); return TRUE;
        case GDK_KEY_t:
            nb_browser_new_tab(b, "about:blank"); return TRUE;
        case GDK_KEY_w:
            nb_browser_close_tab(b, b->active_tab); return TRUE;
        case GDK_KEY_r:
            nb_browser_reload(b); return TRUE;
        case GDK_KEY_plus: case GDK_KEY_equal:
            if (b->active_tab) { b->active_tab->render_state.scale += 0.1f; nb_browser_queue_draw(b); } return TRUE;
        case GDK_KEY_minus:
            if (b->active_tab && b->active_tab->render_state.scale > 0.2f) {
                b->active_tab->render_state.scale -= 0.1f; nb_browser_queue_draw(b); } return TRUE;
        case GDK_KEY_0:
            if (b->active_tab) { b->active_tab->render_state.scale = 1.0f; nb_browser_queue_draw(b); } return TRUE;
        case GDK_KEY_comma:
            b->settings_open = !b->settings_open; nb_browser_queue_draw(b); return TRUE;
        }
    }
    if (ev->keyval == GDK_KEY_F5) { nb_browser_reload(b); return TRUE; }
    if (ev->keyval == GDK_KEY_Escape && b->settings_open) {
        b->settings_open = 0; nb_browser_queue_draw(b); return TRUE;
    }
    if ((ev->state & GDK_MOD1_MASK) && ev->keyval == GDK_KEY_Left)  { nb_browser_go_back(b);    return TRUE; }
    if ((ev->state & GDK_MOD1_MASK) && ev->keyval == GDK_KEY_Right) { nb_browser_go_forward(b); return TRUE; }

    return FALSE;
}

/* ================================================================== */
/*  SCROLL                                                             */
/* ================================================================== */
static gboolean on_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer data) {
    NbBrowser *b = (NbBrowser*)data;
    float step = 80.0f * b->theme.ui_scale;
    if (ev->direction == GDK_SCROLL_DOWN)  b->scroll_y += step;
    if (ev->direction == GDK_SCROLL_UP)    b->scroll_y -= step;
    if (ev->direction == GDK_SCROLL_LEFT)  b->scroll_x -= step;
    if (ev->direction == GDK_SCROLL_RIGHT) b->scroll_x += step;
    /* GDK_SCROLL_SMOOTH */
    if (ev->direction == GDK_SCROLL_SMOOTH) {
        b->scroll_x += (float)ev->delta_x * step * 0.5f;
        b->scroll_y += (float)ev->delta_y * step * 0.5f;
    }
    if (b->scroll_y < 0) b->scroll_y = 0;
    if (b->scroll_x < 0) b->scroll_x = 0;
    nb_browser_queue_draw(b);
    return TRUE;
}

/* ================================================================== */
/*  WINDOW STATE / CONFIGURE                                           */
/* ================================================================== */
static void on_window_state(GtkWidget *w, GdkEventWindowState *ev, gpointer data) {
    NbBrowser *b = (NbBrowser*)data;
    b->maximized = (ev->new_window_state & GDK_WINDOW_STATE_MAXIMIZED) != 0;
    nb_browser_queue_draw(b);
}

static gboolean on_configure(GtkWidget *w, GdkEventConfigure *ev, gpointer data) {
    NbBrowser *b = (NbBrowser*)data;
    b->win_w = ev->width;
    b->win_h = ev->height;
    gtk_widget_set_size_request(b->canvas, ev->width, ev->height);
    nb_browser_queue_draw(b);
    return FALSE;
}

static gboolean timer_pump_cb(gpointer data) {
    NbBrowser *b = (NbBrowser*)data;
    if (b->active_tab && b->active_tab->js)
        nb_js_pump_timers(b->active_tab->js);
    return G_SOURCE_CONTINUE;
}

/* ================================================================== */
/*  BUILD UI + RUN                                                     */
/* ================================================================== */
int nb_browser_run(int argc, char **argv) {
    gtk_init(&argc, &argv);

    NbBrowser *b = calloc(1, sizeof(*b));
    b->theme      = nb_theme_default();
    b->ua_css     = nb_css_ua_stylesheet();
    b->history_pos = -1;
    b->history_len =  0;
    strncpy(b->home_url, "about:blank", sizeof(b->home_url)-1);

    /* Load saved config */
    {
        char cfg[512];
        snprintf(cfg, sizeof(cfg), "%s/.nishant-browser.conf", g_get_home_dir());
        nb_theme_load(&b->theme, cfg);
    }

    /* Detect DPI scale from monitor */
    b->dpi_scale = 1.0f;
    {
        GdkDisplay *disp = gdk_display_get_default();
        if (disp) {
            GdkMonitor *mon = gdk_display_get_primary_monitor(disp);
            if (!mon) mon = gdk_display_get_monitor(disp, 0);
            if (mon) {
                GdkRectangle geom; gdk_monitor_get_geometry(mon, &geom);
                int scale = gdk_monitor_get_scale_factor(mon);
                b->dpi_scale = (float)scale;
                /* Auto-set ui_scale from DPI if not already customized */
                if (b->theme.ui_scale == 1.0f && scale > 1)
                    b->theme.ui_scale = (float)scale;
            }
        }
    }

    b->win_w = b->theme.window_w;
    b->win_h = b->theme.window_h;

    /* ---- Main window: no decorations ---- */
    b->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(b->window), "Nishant Browser");
    gtk_window_set_default_size(GTK_WINDOW(b->window), b->win_w, b->win_h);

    /* Remove ALL OS window decorations — this is the key line */
    gtk_window_set_decorated(GTK_WINDOW(b->window), FALSE);

    /* Make background transparent so rounded corners work */
    gtk_widget_set_app_paintable(b->window, TRUE);
    GdkScreen *screen = gtk_widget_get_screen(b->window);
    GdkVisual *visual = gdk_screen_get_rgba_visual(screen);
    if (visual) gtk_widget_set_visual(b->window, visual);

    /* Allow resize by keeping window resizable */
    gtk_window_set_resizable(GTK_WINDOW(b->window), TRUE);

    /* ---- Single full-window drawing area ---- */
    b->canvas = gtk_drawing_area_new();
    gtk_widget_set_size_request(b->canvas, b->win_w, b->win_h);
    gtk_widget_set_can_focus(b->canvas, TRUE);
    gtk_widget_add_events(b->canvas,
        GDK_BUTTON_PRESS_MASK   | GDK_BUTTON_RELEASE_MASK |
        GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK          |
        GDK_KEY_PRESS_MASK      | GDK_SMOOTH_SCROLL_MASK);
    gtk_container_add(GTK_CONTAINER(b->window), b->canvas);

    /* ---- Connect signals ---- */
    g_signal_connect(b->canvas,  "draw",                 G_CALLBACK(on_draw),           b);
    g_signal_connect(b->canvas,  "motion-notify-event",  G_CALLBACK(on_motion),          b);
    g_signal_connect(b->canvas,  "button-press-event",   G_CALLBACK(on_button_press),    b);
    g_signal_connect(b->canvas,  "button-release-event", G_CALLBACK(on_button_release),  b);
    g_signal_connect(b->canvas,  "key-press-event",      G_CALLBACK(on_key_press),       b);
    g_signal_connect(b->canvas,  "scroll-event",         G_CALLBACK(on_scroll),          b);
    g_signal_connect(b->window,  "window-state-event",   G_CALLBACK(on_window_state),    b);
    g_signal_connect(b->window,  "configure-event",      G_CALLBACK(on_configure),       b);
    g_signal_connect(b->window,  "destroy",              G_CALLBACK(gtk_main_quit),      NULL);

    /* JS timer */
    g_timeout_add(100, timer_pump_cb, b);

    /* Show */
    gtk_widget_show_all(b->window);
    gtk_widget_grab_focus(b->canvas);

    /* Open initial tab */
    const char *start_url = (argc > 1) ? argv[1] : "about:blank";
    nb_browser_new_tab(b, start_url);

    gtk_main();

    /* Cleanup */
    {
        char cfg[512];
        snprintf(cfg, sizeof(cfg), "%s/.nishant-browser.conf", g_get_home_dir());
        nb_theme_save(&b->theme, cfg);
    }
    NbTab *t = b->tabs;
    while (t) { NbTab *n = t->next; tab_free(t); t = n; }
    nb_css_free(b->ua_css);
    for (int i = 0; i < b->history_len; i++) free(b->history[i]);
    free(b);
    return 0;
}
