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

    /* Windows 98 / classic silver chrome */
    t.chrome_r = 0.753f; t.chrome_g = 0.753f; t.chrome_b = 0.753f; /* #C0C0C0 */
    t.chrome_text_r = 0.0f; t.chrome_text_g = 0.0f; t.chrome_text_b = 0.0f;
    t.accent_r = 0.0f; t.accent_g = 0.0f; t.accent_b = 0.502f;  /* classic blue title */
    t.tab_bg_r = 0.753f; t.tab_bg_g = 0.753f; t.tab_bg_b = 0.753f;
    t.page_bg_r = 1.0f; t.page_bg_g = 1.0f; t.page_bg_b = 1.0f;

    strcpy(t.ui_font, "Sans");
    t.ui_font_size = 11.0f;

    t.show_bookmarks_bar = 0;
    t.show_status_bar    = 1;
    t.compact_toolbar    = 0;
    t.window_w = 1200; t.window_h = 800;
    t.maximized = 0;
    t.corner_radius = 0;  /* no rounded corners — classic look */
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
/*  WIN98 CLASSIC 3D BEVEL HELPERS                                    */
/* ================================================================== */

/* Classic colours */
#define W98_BG       0.753f, 0.753f, 0.753f   /* #C0C0C0 face */
#define W98_LIGHT    1.000f, 1.000f, 1.000f   /* highlight */
#define W98_HILIGHT  0.871f, 0.871f, 0.871f   /* #DEDEDE */
#define W98_SHADOW   0.502f, 0.502f, 0.502f   /* #808080 */
#define W98_DARK     0.000f, 0.000f, 0.000f   /* #000000 dark shadow */
#define W98_TEXT     0.000f, 0.000f, 0.000f
#define W98_DISABLED 0.502f, 0.502f, 0.502f
#define W98_TITLE_A  0.000f, 0.000f, 0.502f   /* active title blue */
#define W98_TITLE_T  1.000f, 1.000f, 1.000f   /* title text white */
#define W98_FOCUS    0.000f, 0.000f, 0.502f   /* URL focus border */
#define W98_SEL      0.000f, 0.000f, 0.502f   /* selection blue */

static void cr_set_rgb(cairo_t *cr, float r, float g, float b) {
    cairo_set_source_rgb(cr, r, g, b);
}
static void cr_set_rgba(cairo_t *cr, float r, float g, float b, float a) {
    cairo_set_source_rgba(cr, r, g, b, a);
}

/* cr_round_rect kept for API compat but always draws a plain rect */
static void cr_round_rect(cairo_t *cr, float x, float y, float w, float h, float r) {
    (void)r;
    cairo_rectangle(cr, x, y, w, h);
}

/* Raised 3D border (like a Win98 button up) */
static void bevel_raised(cairo_t *cr, float x, float y, float w, float h) {
    /* outer light (top, left) */
    cairo_set_line_width(cr, 1.0f);
    cr_set_rgb(cr, W98_LIGHT);
    cairo_move_to(cr, x, y+h); cairo_line_to(cr, x, y); cairo_line_to(cr, x+w, y);
    cairo_stroke(cr);
    /* inner highlight */
    cr_set_rgb(cr, W98_HILIGHT);
    cairo_move_to(cr, x+1, y+h-1); cairo_line_to(cr, x+1, y+1); cairo_line_to(cr, x+w-1, y+1);
    cairo_stroke(cr);
    /* outer dark (bottom, right) */
    cr_set_rgb(cr, W98_DARK);
    cairo_move_to(cr, x, y+h); cairo_line_to(cr, x+w, y+h); cairo_line_to(cr, x+w, y);
    cairo_stroke(cr);
    /* inner shadow */
    cr_set_rgb(cr, W98_SHADOW);
    cairo_move_to(cr, x+1, y+h-1); cairo_line_to(cr, x+w-1, y+h-1); cairo_line_to(cr, x+w-1, y+1);
    cairo_stroke(cr);
}

/* Sunken 3D border (like a Win98 button pressed) */
static void bevel_sunken(cairo_t *cr, float x, float y, float w, float h) {
    cairo_set_line_width(cr, 1.0f);
    cr_set_rgb(cr, W98_SHADOW);
    cairo_move_to(cr, x, y+h); cairo_line_to(cr, x, y); cairo_line_to(cr, x+w, y);
    cairo_stroke(cr);
    cr_set_rgb(cr, W98_DARK);
    cairo_move_to(cr, x+1, y+h-1); cairo_line_to(cr, x+1, y+1); cairo_line_to(cr, x+w-1, y+1);
    cairo_stroke(cr);
    cr_set_rgb(cr, W98_LIGHT);
    cairo_move_to(cr, x, y+h); cairo_line_to(cr, x+w, y+h); cairo_line_to(cr, x+w, y);
    cairo_stroke(cr);
    cr_set_rgb(cr, W98_HILIGHT);
    cairo_move_to(cr, x+1, y+h-1); cairo_line_to(cr, x+w-1, y+h-1); cairo_line_to(cr, x+w-1, y+1);
    cairo_stroke(cr);
}

/* Inset border (like a sunken field/edit box) */
static void bevel_inset(cairo_t *cr, float x, float y, float w, float h) {
    cairo_set_line_width(cr, 1.0f);
    cr_set_rgb(cr, W98_SHADOW);
    cairo_move_to(cr, x, y+h); cairo_line_to(cr, x, y); cairo_line_to(cr, x+w, y);
    cairo_stroke(cr);
    cr_set_rgb(cr, W98_DARK);
    cairo_move_to(cr, x+1, y+h-1); cairo_line_to(cr, x+1, y+1); cairo_line_to(cr, x+w-1, y+1);
    cairo_stroke(cr);
    cr_set_rgb(cr, W98_LIGHT);
    cairo_move_to(cr, x, y+h); cairo_line_to(cr, x+w, y+h); cairo_line_to(cr, x+w, y-1);
    cairo_stroke(cr);
}

/* Draw a classic Win98 push button */
static void draw_w98_button(cairo_t *cr, float x, float y, float w, float h,
                             int pressed, int hovered, int enabled) {
    /* Face */
    cr_set_rgb(cr, W98_BG);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
    /* Bevel */
    if (pressed) bevel_sunken(cr, x, y, w, h);
    else         bevel_raised(cr, x, y, w, h);
}

/* Draw a classic 3-pixel title bar stripe pattern */
static void draw_title_gradient(cairo_t *cr, float x, float y, float w, float h,
                                  int active) {
    if (active) {
        /* Classic Win98 blue gradient (2-colour approximation) */
        for (int i = 0; i < (int)h; i++) {
            float t = (float)i / h;
            float r = 0.0f + t * 0.12f;
            float g = 0.0f + t * 0.08f;
            float b = 0.502f + t * 0.20f;
            cairo_set_source_rgb(cr, r, g, b);
            cairo_rectangle(cr, x, y+i, w, 1);
            cairo_fill(cr);
        }
    } else {
        /* Inactive: grey gradient */
        for (int i = 0; i < (int)h; i++) {
            float t = (float)i / h;
            float v = 0.502f + t * 0.15f;
            cairo_set_source_rgb(cr, v, v, v);
            cairo_rectangle(cr, x, y+i, w, 1);
            cairo_fill(cr);
        }
    }
}

/* Text rendering helpers (unchanged) */
static float cr_text_in_box(cairo_t *cr, float x, float y, float w, float h,
                              const char *text, float font_size,
                              const char *font, int bold,
                              float tr, float tg, float tb) {
    if (!text || !text[0]) return 0;
    PangoLayout *pl = pango_cairo_create_layout(cr);
    char ds[128];
    snprintf(ds, sizeof(ds), "%s %s %.0fpx", font, bold?"Bold":"", font_size);
    PangoFontDescription *fd = pango_font_description_from_string(ds);
    pango_layout_set_font_description(pl, fd); pango_font_description_free(fd);
    pango_layout_set_text(pl, text, -1);
    if (w > 0) { pango_layout_set_width(pl,(int)(w*PANGO_SCALE)); pango_layout_set_ellipsize(pl,PANGO_ELLIPSIZE_END); }
    int pw, ph; pango_layout_get_pixel_size(pl, &pw, &ph);
    cairo_set_source_rgb(cr, tr, tg, tb);
    cairo_move_to(cr, x+(w>0?(w-pw)/2.0f:0), y+(h-ph)/2.0f);
    pango_cairo_show_layout(cr, pl);
    g_object_unref(pl);
    return (float)pw;
}
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
    cairo_set_source_rgb(cr, tr, tg, tb);
    cairo_move_to(cr, x, y+(h-ph)/2.0f);
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
    NbHitRect *r = &b->hit[b->hit_count++];
    r->zone = zone; r->tab_index = tab_idx;
    r->x = x; r->y = y; r->w = w; r->h = h;
}
/* Iterate forward — last registered wins (so register background first) */
static NbHitRect hit_find(NbBrowser *b, float x, float y) {
    NbHitRect best = {ZONE_NONE, -1, 0,0,0,0};
    for (int i = 0; i < b->hit_count; i++) {
        NbHitRect *r = &b->hit[i];
        if (x >= r->x && x < r->x+r->w && y >= r->y && y < r->y+r->h)
            best = *r;   /* later registrations win */
    }
    return best;
}

/* ================================================================== */
/*  WIN98-STYLE ICON DRAWING                                          */
/* ================================================================== */
/* All icons drawn as classic Win98 pixel-art style */

static void draw_icon_arrow_left(cairo_t *cr, float cx, float cy, float sz,
                                   float r, float g, float b2, float a) {
    cr_set_rgba(cr, r, g, b2, a);
    cairo_set_line_width(cr, 1.5f);
    float h = sz * 0.38f;
    cairo_move_to(cr, cx+h, cy-h); cairo_line_to(cr, cx-h*0.4f, cy); cairo_line_to(cr, cx+h, cy+h);
    cairo_stroke(cr);
}
static void draw_icon_arrow_right(cairo_t *cr, float cx, float cy, float sz,
                                    float r, float g, float b2, float a) {
    cr_set_rgba(cr, r, g, b2, a);
    cairo_set_line_width(cr, 1.5f);
    float h = sz * 0.38f;
    cairo_move_to(cr, cx-h, cy-h); cairo_line_to(cr, cx+h*0.4f, cy); cairo_line_to(cr, cx-h, cy+h);
    cairo_stroke(cr);
}
static void draw_icon_reload(cairo_t *cr, float cx, float cy, float sz,
                               float r, float g, float b2, float a) {
    cr_set_rgba(cr, r, g, b2, a);
    cairo_set_line_width(cr, 1.5f);
    cairo_arc(cr, cx, cy, sz*0.35f, -2.0f, 1.1f); cairo_stroke(cr);
    float ax = cx+sz*0.35f*cosf(1.1f), ay = cy+sz*0.35f*sinf(1.1f);
    cairo_move_to(cr, ax-sz*0.15f, ay-sz*0.05f);
    cairo_line_to(cr, ax, ay); cairo_line_to(cr, ax+sz*0.05f, ay-sz*0.18f);
    cairo_stroke(cr);
}
static void draw_icon_stop(cairo_t *cr, float cx, float cy, float sz,
                             float r, float g, float b2, float a) {
    cr_set_rgba(cr, r, g, b2, a);
    cairo_set_line_width(cr, 2.0f);
    float h = sz*0.32f;
    cairo_move_to(cr, cx-h, cy-h); cairo_line_to(cr, cx+h, cy+h); cairo_stroke(cr);
    cairo_move_to(cr, cx+h, cy-h); cairo_line_to(cr, cx-h, cy+h); cairo_stroke(cr);
}
static void draw_icon_home(cairo_t *cr, float cx, float cy, float sz,
                            float r, float g, float b2, float a) {
    cr_set_rgba(cr, r, g, b2, a);
    cairo_set_line_width(cr, 1.5f);
    float h = sz*0.36f;
    cairo_move_to(cr, cx-h, cy+h*0.15f); cairo_line_to(cr, cx, cy-h);
    cairo_line_to(cr, cx+h, cy+h*0.15f); cairo_stroke(cr);
    cairo_rectangle(cr, cx-h*0.5f, cy+h*0.15f, h, h*0.85f); cairo_stroke(cr);
}
static void draw_icon_plus(cairo_t *cr, float cx, float cy, float sz,
                            float r, float g, float b2, float a) {
    cr_set_rgba(cr, r, g, b2, a);
    cairo_set_line_width(cr, 1.5f);
    float h = sz*0.32f;
    cairo_move_to(cr, cx, cy-h); cairo_line_to(cr, cx, cy+h); cairo_stroke(cr);
    cairo_move_to(cr, cx-h, cy); cairo_line_to(cr, cx+h, cy); cairo_stroke(cr);
}
static void draw_icon_settings(cairo_t *cr, float cx, float cy, float sz,
                                float r, float g, float b2, float a) {
    cr_set_rgba(cr, r, g, b2, a);
    cairo_set_line_width(cr, 1.2f);
    cairo_arc(cr, cx, cy, sz*0.20f, 0, 2*G_PI); cairo_stroke(cr);
    for (int i = 0; i < 8; i++) {
        float ang = i * G_PI / 4.0f;
        cairo_move_to(cr, cx+sz*0.26f*cosf(ang), cy+sz*0.26f*sinf(ang));
        cairo_line_to(cr, cx+sz*0.40f*cosf(ang), cy+sz*0.40f*sinf(ang));
        cairo_stroke(cr);
    }
}
static void draw_icon_close(cairo_t *cr, float cx, float cy, float sz,
                             float r, float g, float b2, float a) {
    cr_set_rgba(cr, r, g, b2, a);
    cairo_set_line_width(cr, 1.8f);
    float h = sz*0.30f;
    cairo_move_to(cr, cx-h, cy-h); cairo_line_to(cr, cx+h, cy+h); cairo_stroke(cr);
    cairo_move_to(cr, cx+h, cy-h); cairo_line_to(cr, cx-h, cy+h); cairo_stroke(cr);
}
static void draw_icon_minimize(cairo_t *cr, float cx, float cy, float sz,
                                float r, float g, float b2, float a) {
    cr_set_rgba(cr, r, g, b2, a);
    cairo_set_line_width(cr, 2.0f);
    float h = sz*0.28f;
    cairo_move_to(cr, cx-h, cy+h); cairo_line_to(cr, cx+h, cy+h); cairo_stroke(cr);
}
static void draw_icon_maximize(cairo_t *cr, float cx, float cy, float sz,
                                float r, float g, float b2, float a) {
    cr_set_rgba(cr, r, g, b2, a);
    cairo_set_line_width(cr, 1.5f);
    float h = sz*0.28f;
    cairo_rectangle(cr, cx-h, cy-h, h*2, h*2); cairo_stroke(cr);
    /* double top bar (Win98 style) */
    cairo_move_to(cr, cx-h, cy-h+2); cairo_line_to(cr, cx+h, cy-h+2); cairo_stroke(cr);
}
static void draw_icon_restore(cairo_t *cr, float cx, float cy, float sz,
                               float r, float g, float b2, float a) {
    cr_set_rgba(cr, r, g, b2, a);
    cairo_set_line_width(cr, 1.5f);
    float h = sz*0.22f, off = sz*0.10f;
    /* back rect */
    cairo_rectangle(cr, cx-h+off, cy-h-off, h*2, h*2); cairo_stroke(cr);
    /* front rect */
    cr_set_rgba(cr, 0.753f, 0.753f, 0.753f, 1.0f); /* fill to hide overlap */
    cairo_rectangle(cr, cx-h, cy-h+off, h*2, h*2); cairo_fill(cr);
    cr_set_rgba(cr, r, g, b2, a);
    cairo_rectangle(cr, cx-h, cy-h+off, h*2, h*2); cairo_stroke(cr);
    cairo_move_to(cr, cx-h, cy-h+off+2); cairo_line_to(cr, cx+h, cy-h+off+2); cairo_stroke(cr);
}
static void draw_icon_bookmark(cairo_t *cr, float cx, float cy, float sz,
                                float r, float g, float b2, float a) {
    cr_set_rgba(cr, r, g, b2, a);
    cairo_set_line_width(cr, 1.5f);
    float hw = sz*0.26f, hh = sz*0.38f;
    cairo_move_to(cr, cx-hw, cy-hh); cairo_line_to(cr, cx+hw, cy-hh);
    cairo_line_to(cr, cx+hw, cy+hh); cairo_line_to(cr, cx, cy+hh*0.3f);
    cairo_line_to(cr, cx-hw, cy+hh); cairo_close_path(cr); cairo_stroke(cr);
}

/* ================================================================== */
/*  WIN98 TOOLBAR BUTTON                                               */
/* ================================================================== */
static void draw_toolbar_btn(cairo_t *cr, NbBrowser *b, NbZone zone,
                               float x, float y, float w, float h,
                               int enabled,
                               void (*icon_fn)(cairo_t*,float,float,float,float,float,float,float)) {
    int pressed = 0;
    int hov     = (b->hover_zone == zone);
    draw_w98_button(cr, x, y, w, h, pressed, hov, enabled);
    float ic = enabled ? 0.0f : 0.502f;
    float cx2 = x + w/2, cy2 = y + h/2, sz = fminf(w,h)*0.44f;
    icon_fn(cr, cx2, cy2, sz, ic, ic, ic, 1.0f);
    hit_add(b, zone, -1, x, y, w, h);
}

/* ================================================================== */
/*  GEOMETRY CALCULATION                                               */
/* ================================================================== */
static void calc_geometry(NbBrowser *b) {
    float s = b->theme.ui_scale;
    b->titlebar_h  = 22 * s;   /* Win98 title bar */
    b->tabbar_h    = 26 * s;   /* tab strip below title */
    b->toolbar_h   = 34 * s;   /* address bar + nav buttons */
    b->statusbar_h = b->theme.show_status_bar ? 20*s : 0;

    b->content_x = 0;
    b->content_y = b->titlebar_h + b->tabbar_h + b->toolbar_h;
    b->content_w = b->win_w;
    b->content_h = b->win_h - b->content_y - b->statusbar_h;
    if (b->content_h < 0) b->content_h = 0;
}

/* ================================================================== */
/*  WIN98 TITLE BAR                                                    */
/* ================================================================== */
static void draw_titlebar(cairo_t *cr, NbBrowser *b) {
    float s  = b->theme.ui_scale;
    float W  = (float)b->win_w;
    float H  = b->titlebar_h;

    /* IMPORTANT: register drag zone FIRST so specific buttons override it */
    hit_add(b, ZONE_TITLEBAR, -1, 0, 0, W, H);

    /* Outer window border */
    bevel_raised(cr, 0, 0, W, b->win_h);

    /* Title bar gradient */
    draw_title_gradient(cr, 2, 2, W-4, H-2, 1);

    /* Window title */
    const char *title = b->active_tab && b->active_tab->title[0]
                        ? b->active_tab->title : "Nishant Browser";
    PangoLayout *pl = pango_cairo_create_layout(cr);
    char ds[64]; snprintf(ds, sizeof(ds), "Sans Bold %.0fpx", 11.0f*s);
    PangoFontDescription *fd = pango_font_description_from_string(ds);
    pango_layout_set_font_description(pl, fd); pango_font_description_free(fd);
    pango_layout_set_text(pl, title, -1);
    pango_layout_set_ellipsize(pl, PANGO_ELLIPSIZE_END);
    int pw2, ph2; pango_layout_get_pixel_size(pl, &pw2, &ph2);
    cairo_set_source_rgb(cr, W98_TITLE_T);
    cairo_move_to(cr, 6*s, 2+(H-2-ph2)/2.0f);
    pango_cairo_show_layout(cr, pl); g_object_unref(pl);

    /* Window control buttons */
    float btn_w = (float)(int)(H * 0.90f);
    float btn_h = H - 4*s, btn_y = 2*s;
    float margin = 3*s;
    float x = W - 2 - margin - btn_w;

    /* Close */
    int hc = (b->hover_zone == ZONE_CLOSE);
    draw_w98_button(cr, x, btn_y, btn_w, btn_h, 0, hc, 1);
    draw_icon_close(cr, x+btn_w/2, btn_y+btn_h/2, btn_h*0.70f, 0,0,0,1);
    hit_add(b, ZONE_CLOSE, -1, x, btn_y, btn_w, btn_h);
    x -= btn_w + margin;

    /* Maximize/Restore */
    int hm = (b->hover_zone == ZONE_MAXIMIZE);
    draw_w98_button(cr, x, btn_y, btn_w, btn_h, 0, hm, 1);
    if (b->maximized) draw_icon_restore(cr, x+btn_w/2, btn_y+btn_h/2, btn_h*0.70f, 0,0,0,1);
    else              draw_icon_maximize(cr, x+btn_w/2, btn_y+btn_h/2, btn_h*0.70f, 0,0,0,1);
    hit_add(b, ZONE_MAXIMIZE, -1, x, btn_y, btn_w, btn_h);
    x -= btn_w + margin;

    /* Minimize */
    int hn = (b->hover_zone == ZONE_MINIMIZE);
    draw_w98_button(cr, x, btn_y, btn_w, btn_h, 0, hn, 1);
    draw_icon_minimize(cr, x+btn_w/2, btn_y+btn_h/2, btn_h*0.70f, 0,0,0,1);
    hit_add(b, ZONE_MINIMIZE, -1, x, btn_y, btn_w, btn_h);
}

/* ================================================================== */
/*  WIN98 TAB STRIP                                                    */
/* ================================================================== */
static void draw_tabstrip(cairo_t *cr, NbBrowser *b) {
    float s   = b->theme.ui_scale;
    float y0  = b->titlebar_h;
    float H   = b->tabbar_h;
    float W   = (float)b->win_w;

    cr_set_rgb(cr, W98_BG);
    cairo_rectangle(cr, 2, y0, W-4, H);
    cairo_fill(cr);

    cr_set_rgb(cr, W98_SHADOW);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, 2, y0+H-1); cairo_line_to(cr, W-2, y0+H-1);
    cairo_stroke(cr);

    /* New tab button */
    float ntw = 22*s, nth = H-4*s, nty = y0+2*s;
    int hn2 = (b->hover_zone == ZONE_BTN_NEW_TAB);
    draw_w98_button(cr, 3*s, nty, ntw, nth, 0, hn2, 1);
    draw_icon_plus(cr, 3*s+ntw/2, nty+nth/2, nth*0.5f, 0,0,0,1);
    hit_add(b, ZONE_BTN_NEW_TAB, -1, 3*s, nty, ntw, nth);

    float tx = 3*s + ntw + 4*s;
    int n = b->tab_count; if (n < 1) n = 1;
    float tabs_avail = W - tx - 6*s;
    float tab_w = fminf(180*s, tabs_avail / (float)n);
    if (tab_w < 60) tab_w = 60;

    int idx = 0;
    for (NbTab *tab = b->tabs; tab; tab = tab->next, idx++) {
        int active = (tab == b->active_tab);
        int is_hov = (b->hover_zone == ZONE_TAB && b->hover_tab == idx);
        float ty   = active ? y0+1*s : y0+3*s;
        float th   = active ? H-1*s  : H-3*s;

        cr_set_rgb(cr, W98_BG);
        cairo_rectangle(cr, tx, ty, tab_w, th); cairo_fill(cr);
        bevel_raised(cr, tx, ty, tab_w, th);

        if (active) {
            /* Erase bottom line to merge with content area */
            cr_set_rgb(cr, W98_BG);
            cairo_rectangle(cr, tx+1, ty+th-1, tab_w-2, 2); cairo_fill(cr);
        }

        /* Favicon stub */
        float fx = tx+5*s, fy = ty+(th-14*s)/2;
        cr_set_rgb(cr, 0.92f,0.92f,1.0f);
        cairo_rectangle(cr, fx, fy, 14*s, 14*s); cairo_fill(cr);
        bevel_sunken(cr, fx, fy, 14*s, 14*s);

        /* Title */
        float title_x = tx + 24*s;
        float close_bw = 16*s;
        float title_max = tab_w - 24*s - close_bw - 6*s;
        cairo_save(cr);
        cairo_rectangle(cr, title_x, ty, title_max, th); cairo_clip(cr);
        const char *ttitle = tab->loading ? "Loading..." : (tab->title[0] ? tab->title : "New Tab");
        cr_text_left(cr, title_x, ty, th, ttitle, 10.5f*s, "Sans", 0.0f, 0.0f, 0.0f);
        cairo_restore(cr);

        /* Tab close button */
        if (active || is_hov) {
            float cbx = tx + tab_w - close_bw - 3*s;
            float cby = ty + (th-14*s)/2;
            int hcl = (b->hover_zone == ZONE_TAB_CLOSE && b->hover_tab == idx);
            draw_w98_button(cr, cbx, cby, 14*s, 14*s, 0, hcl, 1);
            draw_icon_close(cr, cbx+7*s, cby+7*s, 14*s*0.7f, 0,0,0,1);
            hit_add(b, ZONE_TAB_CLOSE, idx, cbx, cby, 14*s, 14*s);
        }

        hit_add(b, ZONE_TAB, idx, tx, ty, tab_w, th);
        tab->tab_x = tx; tab->tab_w = tab_w;
        tx += tab_w + 2;
    }
}

/* ================================================================== */
/*  WIN98 TOOLBAR                                                      */
/* ================================================================== */
static void draw_toolbar(cairo_t *cr, NbBrowser *b) {
    float s   = b->theme.ui_scale;
    float y0  = b->titlebar_h + b->tabbar_h;
    float W   = (float)b->win_w;
    float H   = b->toolbar_h;

    cr_set_rgb(cr, W98_BG);
    cairo_rectangle(cr, 2, y0, W-4, H); cairo_fill(cr);

    cr_set_rgb(cr, W98_SHADOW);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, 2, y0+H-1); cairo_line_to(cr, W-2, y0+H-1);
    cairo_stroke(cr);

    float btn_h = (float)(int)(H * 0.74f);
    float btn_y = y0 + (H - btn_h)/2;
    float btn_w = btn_h + 8*s;
    float x = 6*s;

    int can_back    = b->history_pos > 0;
    int can_forward = b->history_pos < b->history_len - 1;
    int loading     = b->active_tab && b->active_tab->loading;

    draw_toolbar_btn(cr, b, ZONE_BTN_BACK,    x, btn_y, btn_w, btn_h, can_back,    draw_icon_arrow_left);  x += btn_w + 2*s;
    draw_toolbar_btn(cr, b, ZONE_BTN_FORWARD, x, btn_y, btn_w, btn_h, can_forward, draw_icon_arrow_right); x += btn_w + 2*s;
    if (loading)
        draw_toolbar_btn(cr, b, ZONE_BTN_STOP,   x, btn_y, btn_w, btn_h, 1, draw_icon_stop);
    else
        draw_toolbar_btn(cr, b, ZONE_BTN_RELOAD, x, btn_y, btn_w, btn_h, 1, draw_icon_reload);
    x += btn_w + 2*s;
    draw_toolbar_btn(cr, b, ZONE_BTN_HOME, x, btn_y, btn_w, btn_h, 1, draw_icon_home);
    x += btn_w + 6*s;

    /* "Address:" label */
    float lbl_w = 0;
    {
        PangoLayout *pl = pango_cairo_create_layout(cr);
        char dsc[64]; snprintf(dsc, sizeof(dsc), "Sans %.0fpx", 10.5f*s);
        PangoFontDescription *fd = pango_font_description_from_string(dsc);
        pango_layout_set_font_description(pl, fd); pango_font_description_free(fd);
        pango_layout_set_text(pl, "Address:", -1);
        int pw3, ph3; pango_layout_get_pixel_size(pl, &pw3, &ph3);
        cairo_set_source_rgb(cr, 0.0f, 0.0f, 0.0f);
        cairo_move_to(cr, x, y0+(H-ph3)/2.0f);
        pango_cairo_show_layout(cr, pl); g_object_unref(pl);
        lbl_w = (float)pw3 + 4*s;
    }
    x += lbl_w;

    /* Right-side buttons width */
    float right_w = (btn_w + 2*s)*3 + 36*s + 6*s;
    float url_w = W - x - right_w - 8*s;
    float url_h = btn_h;
    float url_y = btn_y;

    /* URL bar — white sunken edit box */
    cr_set_rgb(cr, 1,1,1);
    cairo_rectangle(cr, x, url_y, url_w, url_h); cairo_fill(cr);
    bevel_inset(cr, x, url_y, url_w, url_h);

    if (b->url_focused) {
        cr_set_rgb(cr, W98_FOCUS);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, x-1, url_y-1, url_w+2, url_h+2);
        cairo_stroke(cr);
    }

    const char *url_text = b->url_edit[0] ? b->url_edit :
                           (b->active_tab ? b->active_tab->url : "");
    cairo_save(cr);
    float pad = 4*s;
    cairo_rectangle(cr, x+pad, url_y+1, url_w-pad*2, url_h-2); cairo_clip(cr);
    if (!url_text || !url_text[0]) {
        cr_text_left(cr, x+pad, url_y, url_h, "Type address and press Enter",
                     10.0f*s, "Sans", 0.502f, 0.502f, 0.502f);
    } else {
        cr_text_left(cr, x+pad, url_y, url_h, url_text, 10.5f*s, "Sans", 0.0f, 0.0f, 0.0f);
        if (b->url_focused) {
            PangoLayout *pl = pango_cairo_create_layout(cr);
            char dsc[64]; snprintf(dsc, sizeof(dsc), "Sans %.0fpx", 10.5f*s);
            PangoFontDescription *fd = pango_font_description_from_string(dsc);
            pango_layout_set_font_description(pl, fd); pango_font_description_free(fd);
            int cc = b->url_cursor;
            int tlen = (int)strlen(url_text);
            if (cc > tlen) cc = tlen;
            char tmp[2048]; memcpy(tmp, url_text, cc); tmp[cc] = 0;
            pango_layout_set_text(pl, tmp, -1);
            int pw3, ph3; pango_layout_get_pixel_size(pl, &pw3, &ph3);
            g_object_unref(pl);
            float caret_x = x+pad+pw3;
            cr_set_rgb(cr, 0.0f, 0.0f, 0.0f);
            cairo_set_line_width(cr, 1.0f);
            cairo_move_to(cr, caret_x, url_y+(url_h-ph3)/2+1);
            cairo_line_to(cr, caret_x, url_y+(url_h-ph3)/2+ph3-1);
            cairo_stroke(cr);
        }
    }
    cairo_restore(cr);
    hit_add(b, ZONE_URL_BAR, -1, x, url_y, url_w, url_h);
    x += url_w + 6*s;

    /* Zoom out */
    {
        int hz = (b->hover_zone == ZONE_BTN_ZOOM_OUT);
        draw_w98_button(cr, x, btn_y, btn_w, btn_h, 0, hz, 1);
        float mz = btn_h*0.28f;
        cr_set_rgb(cr, 0.0f, 0.0f, 0.0f);
        cairo_set_line_width(cr, 1.5f);
        cairo_move_to(cr, x+btn_w/2-mz, btn_y+btn_h/2);
        cairo_line_to(cr, x+btn_w/2+mz, btn_y+btn_h/2); cairo_stroke(cr);
        hit_add(b, ZONE_BTN_ZOOM_OUT, -1, x, btn_y, btn_w, btn_h);
        x += btn_w + 2*s;
    }
    /* Zoom % */
    {
        float zoom = b->active_tab ? b->active_tab->render_state.scale : 1.0f;
        char zs[10]; snprintf(zs, sizeof(zs), "%.0f%%", zoom*100);
        float zw = 34*s;
        cr_set_rgb(cr, 1,1,1);
        cairo_rectangle(cr, x, btn_y, zw, btn_h); cairo_fill(cr);
        bevel_inset(cr, x, btn_y, zw, btn_h);
        cr_text_in_box(cr, x, btn_y, zw, btn_h, zs, 10.0f*s, "Sans", 0, 0.0f, 0.0f, 0.0f);
        hit_add(b, ZONE_BTN_ZOOM_RESET, -1, x, btn_y, zw, btn_h);
        x += zw + 2*s;
    }
    draw_toolbar_btn(cr, b, ZONE_BTN_ZOOM_IN,  x, btn_y, btn_w, btn_h, 1, draw_icon_plus);     x += btn_w + 2*s;
    draw_toolbar_btn(cr, b, ZONE_BTN_BOOKMARK, x, btn_y, btn_w, btn_h, 1, draw_icon_bookmark);  x += btn_w + 2*s;
    draw_toolbar_btn(cr, b, ZONE_BTN_SETTINGS, x, btn_y, btn_w, btn_h, 1, draw_icon_settings);
}

/* ================================================================== */
/*  WIN98 SETTINGS PANEL                                               */
/* ================================================================== */
static void draw_slider(cairo_t *cr, NbBrowser *b, float x, float y, float w,
                         float val, float vmin, float vmax,
                         const char *label, float s,
                         float tr, float tg, float tb) {
    (void)b;
    char lbl[64]; snprintf(lbl, sizeof(lbl), "%s: %.2f", label, val);
    cr_text_left(cr, x, y-2*s, 16*s, lbl, 11*s, "Sans", tr, tg, tb);
    /* Track */
    cr_set_rgb(cr, 1,1,1);
    cairo_rectangle(cr, x, y+8*s, w, 6*s); cairo_fill(cr);
    bevel_inset(cr, x, y+8*s, w, 6*s);
    /* Fill */
    float fill_w = w * (val - vmin) / (vmax - vmin);
    cr_set_rgb(cr, W98_TITLE_A);
    cairo_rectangle(cr, x, y+8*s, fill_w, 6*s); cairo_fill(cr);
    /* Knob */
    float kx = x + fill_w - 4*s;
    cr_set_rgb(cr, W98_BG);
    cairo_rectangle(cr, kx, y+5*s, 8*s, 12*s); cairo_fill(cr);
    bevel_raised(cr, kx, y+5*s, 8*s, 12*s);
}

static void draw_color_swatch(cairo_t *cr, float x, float y, float w, float h,
                               float r, float g, float b2, float br, float bg, float bb) {
    (void)br; (void)bg; (void)bb;
    cr_set_rgb(cr, r, g, b2);
    cairo_rectangle(cr, x, y, w, h); cairo_fill(cr);
    bevel_raised(cr, x, y, w, h);
}

static void draw_settings_panel(cairo_t *cr, NbBrowser *b) {
    float s  = b->theme.ui_scale;
    float W  = b->win_w, H = b->win_h;

    /* Dim overlay */
    cr_set_rgba(cr, 0,0,0, 0.35f);
    cairo_rectangle(cr, 0, 0, W, H); cairo_fill(cr);

    float pw = fminf(520*s, W-40*s);
    float ph = fminf(620*s, H-40*s);
    float px = (W-pw)/2, py = (H-ph)/2;

    /* Panel background */
    cr_set_rgb(cr, W98_BG);
    cairo_rectangle(cr, px, py, pw, ph); cairo_fill(cr);
    bevel_raised(cr, px, py, pw, ph);

    /* Title bar */
    draw_title_gradient(cr, px+2, py+2, pw-4, 20*s, 1);
    cairo_set_source_rgb(cr, W98_TITLE_T);
    {
        PangoLayout *pl = pango_cairo_create_layout(cr);
        char ds[64]; snprintf(ds, sizeof(ds), "Sans Bold %.0fpx", 11.0f*s);
        PangoFontDescription *fd = pango_font_description_from_string(ds);
        pango_layout_set_font_description(pl, fd); pango_font_description_free(fd);
        pango_layout_set_text(pl, "Nishant Browser - Options", -1);
        int pw2, ph2; pango_layout_get_pixel_size(pl, &pw2, &ph2);
        cairo_move_to(cr, px+6*s, py+2+(20*s-ph2)/2);
        pango_cairo_show_layout(cr, pl); g_object_unref(pl);
    }

    /* Close button in title */
    float cbx = px+pw-2-18*s, cby = py+2, cbw = 16*s, cbh = 18*s;
    int hc = (b->hover_zone == ZONE_SETTINGS_CLOSE);
    draw_w98_button(cr, cbx, cby, cbw, cbh, 0, hc, 1);
    draw_icon_close(cr, cbx+cbw/2, cby+cbh/2, cbh*0.65f, 0,0,0,1);
    hit_add(b, ZONE_SETTINGS_CLOSE, -1, cbx, cby, cbw, cbh);

    float lx = px+12*s, row = py+22*s+8*s, lw = pw-24*s;

    /* Group box: Appearance */
    cr_set_rgb(cr, 0.0f, 0.0f, 0.0f);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, lx, row+8*s, lw, 110*s); cairo_stroke(cr);
    cr_set_rgb(cr, W98_BG);
    cairo_rectangle(cr, lx+8*s, row+3*s, 80*s, 12*s); cairo_fill(cr);
    cr_text_left(cr, lx+10*s, row+3*s, 12*s, "Appearance", 10.5f*s, "Sans", 0.0f, 0.0f, 0.0f);
    draw_slider(cr, b, lx+8*s, row+20*s, lw*0.65f, b->theme.ui_scale, 0.5f, 2.0f, "UI Scale", s, 0.0f, 0.0f, 0.0f);
    draw_slider(cr, b, lx+8*s, row+50*s, lw*0.65f, b->theme.ui_font_size, 8.0f, 20.0f, "Font Size", s, 0.0f, 0.0f, 0.0f);
    draw_slider(cr, b, lx+8*s, row+80*s, lw*0.65f, (float)b->theme.corner_radius, 0.0f, 16.0f, "Corner Radius", s, 0.0f, 0.0f, 0.0f);
    row += 120*s;

    /* Colour presets */
    cr_set_rgb(cr, 0.0f, 0.0f, 0.0f);
    cairo_rectangle(cr, lx, row+8*s, lw, 50*s); cairo_stroke(cr);
    cr_set_rgb(cr, W98_BG);
    cairo_rectangle(cr, lx+8*s, row+3*s, 80*s, 12*s); cairo_fill(cr);
    cr_text_left(cr, lx+10*s, row+3*s, 12*s, "Theme", 10.5f*s, "Sans", 0.0f, 0.0f, 0.0f);
    struct { const char *name; float r,g,b; } presets[] = {
        {"Classic",  0.753f,0.753f,0.753f},
        {"Navy",     0.0f,  0.0f,  0.502f},
        {"Teal",     0.0f,  0.302f,0.302f},
        {"Maroon",   0.502f,0.0f,  0.0f  },
        {"Olive",    0.502f,0.502f,0.0f  },
        {NULL,0,0,0}
    };
    float sw_x = lx+8*s, sw_y = row+16*s;
    float sw_w = 60*s, sw_h = 22*s;
    for (int i = 0; presets[i].name; i++) {
        draw_color_swatch(cr, sw_x, sw_y, sw_w, sw_h, presets[i].r, presets[i].g, presets[i].b, 0,0,0);
        cr_text_in_box(cr, sw_x, sw_y, sw_w, sw_h, presets[i].name, 9.0f*s, "Sans", 0,
                       1-presets[i].r, 1-presets[i].g, 1-presets[i].b);
        sw_x += sw_w + 4*s;
    }
    row += 60*s;

    /* Group box: Options */
    cr_set_rgb(cr, 0.0f, 0.0f, 0.0f);
    cairo_rectangle(cr, lx, row+8*s, lw, 75*s); cairo_stroke(cr);
    cr_set_rgb(cr, W98_BG);
    cairo_rectangle(cr, lx+8*s, row+3*s, 65*s, 12*s); cairo_fill(cr);
    cr_text_left(cr, lx+10*s, row+3*s, 12*s, "Options", 10.5f*s, "Sans", 0.0f, 0.0f, 0.0f);

    struct { const char *label; int *val; } checks[] = {
        {"Compact toolbar",   &b->theme.compact_toolbar  },
        {"Show status bar",   &b->theme.show_status_bar  },
        {"Show bookmarks bar",&b->theme.show_bookmarks_bar},
        {NULL,NULL}
    };
    float chy = row + 20*s;
    for (int i = 0; checks[i].label; i++, chy += 22*s) {
        int v = *checks[i].val;
        /* Classic checkbox */
        cr_set_rgb(cr, 1,1,1);
        cairo_rectangle(cr, lx+8*s, chy+1*s, 13*s, 13*s); cairo_fill(cr);
        bevel_inset(cr, lx+8*s, chy+1*s, 13*s, 13*s);
        if (v) {
            cr_set_rgb(cr, 0.0f, 0.0f, 0.0f);
            cairo_set_line_width(cr, 1.5f);
            cairo_move_to(cr, lx+10*s, chy+8*s);
            cairo_line_to(cr, lx+13*s, chy+12*s);
            cairo_line_to(cr, lx+20*s, chy+4*s);
            cairo_stroke(cr);
        }
        cr_text_left(cr, lx+26*s, chy, 14*s, checks[i].label, 10.5f*s, "Sans", 0.0f, 0.0f, 0.0f);
    }
    row += 85*s;

    /* OK button */
    float okx = px + pw/2 - 35*s, oky = py+ph-30*s, okw = 70*s, okh = 22*s;
    draw_w98_button(cr, okx, oky, okw, okh, 0, 0, 1);
    cr_text_in_box(cr, okx, oky, okw, okh, "OK", 10.5f*s, "Sans", 0, 0.0f, 0.0f, 0.0f);
    hit_add(b, ZONE_SETTINGS_CLOSE, -1, okx, oky, okw, okh);
}

/* ================================================================== */
/*  WIN98 STATUS BAR                                                   */
/* ================================================================== */
static void draw_status_bar(cairo_t *cr, NbBrowser *b) {
    if (!b->theme.show_status_bar) return;
    float s  = b->theme.ui_scale;
    float W  = (float)b->win_w;
    float H  = b->statusbar_h;
    float y0 = b->win_h - H;

    cr_set_rgb(cr, W98_BG);
    cairo_rectangle(cr, 2, y0, W-4, H); cairo_fill(cr);
    bevel_inset(cr, 2, y0, W-4, H);

    /* Status panels */
    const char *status = "Ready";
    if (b->active_tab) {
        if (b->active_tab->loading)          status = b->active_tab->url;
        else if (b->active_tab->error_msg[0]) status = b->active_tab->error_msg;
        else if (b->active_tab->url[0])       status = b->active_tab->url;
    }

    /* Left panel */
    float panel1_w = W - 4 - 60*s - 8*s;
    cr_set_rgb(cr, 1,1,1);
    cairo_rectangle(cr, 4, y0+2, panel1_w, H-4); cairo_fill(cr);
    bevel_inset(cr, 4, y0+2, panel1_w, H-4);
    cairo_save(cr);
    cairo_rectangle(cr, 6, y0+2, panel1_w-4, H-4); cairo_clip(cr);
    cr_text_left(cr, 6, y0+2, H-4, status, 9.5f*s, "Sans", 0.0f, 0.0f, 0.0f);
    cairo_restore(cr);

    /* Right panel — zoom */
    float panel2_x = 4 + panel1_w + 4;
    float panel2_w = 60*s;
    cr_set_rgb(cr, 1,1,1);
    cairo_rectangle(cr, panel2_x, y0+2, panel2_w, H-4); cairo_fill(cr);
    bevel_inset(cr, panel2_x, y0+2, panel2_w, H-4);
    float zoom = b->active_tab ? b->active_tab->render_state.scale : 1.0f;
    char zs[10]; snprintf(zs, sizeof(zs), "%.0f%%", zoom*100);
    cr_text_in_box(cr, panel2_x, y0+2, panel2_w, H-4, zs, 9.5f*s, "Sans", 0, 0.0f, 0.0f, 0.0f);
}

/* ================================================================== */
/*  CONTENT AREA                                                       */
/* ================================================================== */
static void draw_content(cairo_t *cr, NbBrowser *b) {
    float cx = b->content_x, cy = b->content_y;
    float cw = b->content_w, ch = b->content_h;
    if (cw <= 0 || ch <= 0) return;

    cairo_save(cr);
    cairo_rectangle(cr, cx, cy, cw, ch);
    cairo_clip(cr);
    cairo_translate(cr, cx, cy);

    NbTab *tab = b->active_tab;
    if (!tab) {
        cr_set_rgb(cr, 1,1,1); cairo_paint(cr);
        cairo_restore(cr); return;
    }
    if (tab->loading) {
        cr_set_rgb(cr, 1,1,1); cairo_paint(cr);
        cr_text_in_box(cr, 0, 0, cw, ch, "Loading...", 14, "Sans", 0, 0.4f,0.4f,0.4f);
        cairo_restore(cr); return;
    }
    if (tab->error_msg[0]) {
        nb_render_error_page(cr, cw, ch, tab->error_msg);
        cairo_restore(cr); return;
    }
    if (!tab->layout) {
        cr_set_rgb(cr, 1,1,1); cairo_paint(cr);
        cairo_restore(cr); return;
    }

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

    /* Fill whole window with classic grey */
    cr_set_rgb(cr, W98_BG);
    cairo_rectangle(cr, 0, 0, b->win_w, b->win_h);
    cairo_fill(cr);

    /* Draw layers bottom-up */
    draw_content(cr, b);
    draw_titlebar(cr, b);  /* includes outer bevel */
    draw_tabstrip(cr, b);
    draw_toolbar(cr, b);
    draw_status_bar(cr, b);

    if (b->settings_open)
        draw_settings_panel(cr, b);

    return FALSE;
}

/* ================================================================== */
/*  RESIZE EDGE DETECTION                                              */
/* ================================================================== */
#define RESIZE_EDGE 6

static GdkWindowEdge get_resize_edge(NbBrowser *b, float mx, float my) {
    float W = b->win_w, H = b->win_h;
    float E = RESIZE_EDGE;
    int top = my < E, bot = my > H-E, lft = mx < E, rgt = mx > W-E;
    if (top && lft) return GDK_WINDOW_EDGE_NORTH_WEST;
    if (top && rgt) return GDK_WINDOW_EDGE_NORTH_EAST;
    if (bot && lft) return GDK_WINDOW_EDGE_SOUTH_WEST;
    if (bot && rgt) return GDK_WINDOW_EDGE_SOUTH_EAST;
    if (top)        return GDK_WINDOW_EDGE_NORTH;
    if (bot)        return GDK_WINDOW_EDGE_SOUTH;
    if (lft)        return GDK_WINDOW_EDGE_WEST;
    if (rgt)        return GDK_WINDOW_EDGE_EAST;
    return (GdkWindowEdge)-1;
}

static GdkCursorType cursor_for_edge(GdkWindowEdge edge) {
    switch (edge) {
    case GDK_WINDOW_EDGE_NORTH:      return GDK_TOP_SIDE;
    case GDK_WINDOW_EDGE_SOUTH:      return GDK_BOTTOM_SIDE;
    case GDK_WINDOW_EDGE_WEST:       return GDK_LEFT_SIDE;
    case GDK_WINDOW_EDGE_EAST:       return GDK_RIGHT_SIDE;
    case GDK_WINDOW_EDGE_NORTH_WEST: return GDK_TOP_LEFT_CORNER;
    case GDK_WINDOW_EDGE_NORTH_EAST: return GDK_TOP_RIGHT_CORNER;
    case GDK_WINDOW_EDGE_SOUTH_WEST: return GDK_BOTTOM_LEFT_CORNER;
    case GDK_WINDOW_EDGE_SOUTH_EAST: return GDK_BOTTOM_RIGHT_CORNER;
    default:                          return GDK_LEFT_PTR;
    }
}

/* ================================================================== */
/*  MOUSE MOTION                                                       */
/* ================================================================== */
static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, gpointer data) {
    NbBrowser *b = (NbBrowser*)data;
    float mx = (float)ev->x, my = (float)ev->y;

    /* Update hover zone */
    NbHitRect h = hit_find(b, mx, my);
    NbZone old_zone = b->hover_zone;
    int    old_tab  = b->hover_tab;
    b->hover_zone = h.zone;
    b->hover_tab  = h.tab_index;

    /* Cursor */
    GdkDisplay *disp = gdk_display_get_default();
    GdkCursorType ctype = GDK_LEFT_PTR;
    if (!b->maximized) {
        GdkWindowEdge edge = get_resize_edge(b, mx, my);
        if ((int)edge >= 0) ctype = cursor_for_edge(edge);
    }
    if (ctype == GDK_LEFT_PTR) {
        switch (h.zone) {
        case ZONE_URL_BAR: ctype = GDK_XTERM; break;
        case ZONE_CLOSE: case ZONE_MAXIMIZE: case ZONE_MINIMIZE:
        case ZONE_TAB: case ZONE_TAB_CLOSE: case ZONE_BTN_BACK:
        case ZONE_BTN_FORWARD: case ZONE_BTN_RELOAD: case ZONE_BTN_STOP:
        case ZONE_BTN_HOME: case ZONE_BTN_NEW_TAB: case ZONE_BTN_ZOOM_IN:
        case ZONE_BTN_ZOOM_OUT: case ZONE_BTN_BOOKMARK: case ZONE_BTN_SETTINGS:
        case ZONE_SETTINGS_CLOSE: case ZONE_BTN_ZOOM_RESET:
            ctype = GDK_HAND2; break;
        default: break;
        }
    }
    GdkCursor *cur = gdk_cursor_new_for_display(disp, ctype);
    gdk_window_set_cursor(gtk_widget_get_window(b->window), cur);
    g_object_unref(cur);

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

static gboolean on_button_press(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    NbBrowser *b = (NbBrowser*)data;
    if (ev->button != 1) return FALSE;
    float mx = (float)ev->x, my = (float)ev->y;
    GdkWindow *gwin = gtk_widget_get_window(b->window);

    /* ---- Check resize edges first (before hit-test) ---- */
    if (!b->maximized) {
        GdkWindowEdge edge = get_resize_edge(b, mx, my);
        if ((int)edge >= 0) {
            gdk_window_begin_resize_drag(gwin, edge,
                ev->button, (int)ev->x_root, (int)ev->y_root, ev->time);
            return TRUE;
        }
    }

    /* Double-click titlebar → maximize/restore */
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
        gtk_window_close(GTK_WINDOW(b->window));
        return TRUE;

    case ZONE_MAXIMIZE:
        if (b->maximized) gtk_window_unmaximize(GTK_WINDOW(b->window));
        else              gtk_window_maximize(GTK_WINDOW(b->window));
        return TRUE;

    case ZONE_MINIMIZE:
        gtk_window_iconify(GTK_WINDOW(b->window));
        return TRUE;

    case ZONE_TITLEBAR:
        /* Use GDK move drag — correct way to move a CSD/frameless window */
        gdk_window_begin_move_drag(gwin, ev->button,
            (int)ev->x_root, (int)ev->y_root, ev->time);
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

    case ZONE_BTN_NEW_TAB:   nb_browser_new_tab(b, "about:blank");  return TRUE;
    case ZONE_BTN_BACK:      nb_browser_go_back(b);                  return TRUE;
    case ZONE_BTN_FORWARD:   nb_browser_go_forward(b);               return TRUE;
    case ZONE_BTN_RELOAD:    nb_browser_reload(b);                   return TRUE;
    case ZONE_BTN_STOP:
        if (b->active_tab) { b->active_tab->loading = 0; nb_browser_queue_draw(b); }
        return TRUE;
    case ZONE_BTN_HOME:      navigate_active(b, b->home_url);        return TRUE;

    case ZONE_BTN_ZOOM_IN:
        if (b->active_tab) { b->active_tab->render_state.scale += 0.1f; nb_browser_queue_draw(b); }
        return TRUE;
    case ZONE_BTN_ZOOM_OUT:
        if (b->active_tab && b->active_tab->render_state.scale > 0.2f) {
            b->active_tab->render_state.scale -= 0.1f; nb_browser_queue_draw(b); }
        return TRUE;
    case ZONE_BTN_ZOOM_RESET:
        if (b->active_tab) { b->active_tab->render_state.scale = 1.0f; nb_browser_queue_draw(b); }
        return TRUE;

    case ZONE_BTN_SETTINGS:
        b->settings_open = !b->settings_open;
        nb_browser_queue_draw(b);
        return TRUE;

    case ZONE_SETTINGS_CLOSE:
        b->settings_open = 0;
        {   char cfg[512];
            snprintf(cfg, sizeof(cfg), "%s/.nishant-browser.conf", g_get_home_dir());
            nb_theme_save(&b->theme, cfg); }
        nb_browser_queue_draw(b);
        return TRUE;

    case ZONE_URL_BAR:
        b->url_focused = 1;
        gtk_widget_grab_focus(b->canvas);
        b->url_sel_start = 0;
        b->url_sel_end   = (int)strlen(b->url_edit);
        b->url_cursor    = b->url_sel_end;
        nb_browser_queue_draw(b);
        return TRUE;

    case ZONE_CONTENT:
        b->url_focused = 0;
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
    (void)w; (void)ev; (void)data;
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

    /* Remove ALL OS window decorations */
    gtk_window_set_decorated(GTK_WINDOW(b->window), FALSE);

    /* Opaque background — no transparency needed for classic style */
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

