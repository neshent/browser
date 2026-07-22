#ifndef NB_BROWSER_H
#define NB_BROWSER_H

#include "../html/dom.h"
#include "../css/css.h"
#include "../layout/layout.h"
#include "../render/render.h"
#include "../js/js_engine.h"
#include "../net/http.h"
#include <gtk/gtk.h>
#include <gdk/gdk.h>

/* ------------------------------------------------------------------ */
/*  UI Theme / Settings                                                */
/* ------------------------------------------------------------------ */
typedef struct {
    /* Scale — 1.0 = normal, 1.25 = 125%, etc. */
    float ui_scale;

    /* Chrome colours */
    float chrome_r, chrome_g, chrome_b;         /* titlebar + toolbar bg */
    float chrome_text_r, chrome_text_g, chrome_text_b;
    float accent_r, accent_g, accent_b;         /* active tab, focus ring */
    float tab_bg_r, tab_bg_g, tab_bg_b;         /* inactive tab */
    float page_bg_r, page_bg_g, page_bg_b;      /* new-tab page */

    /* Typography */
    char  ui_font[64];
    float ui_font_size;     /* base px, scaled by ui_scale */

    /* Toolbar layout flags */
    int   show_bookmarks_bar;
    int   show_status_bar;
    int   compact_toolbar;  /* thinner toolbar */

    /* Window */
    int   window_w, window_h;
    int   maximized;
    int   corner_radius;    /* px, rounded window corners */
} NbTheme;

NbTheme nb_theme_default(void);
void    nb_theme_save(const NbTheme *t, const char *path);
int     nb_theme_load(NbTheme *t, const char *path);

/* ------------------------------------------------------------------ */
/*  Hit-zones for frameless window mouse handling                      */
/* ------------------------------------------------------------------ */
typedef enum {
    ZONE_NONE, ZONE_TITLEBAR, ZONE_CLOSE, ZONE_MAXIMIZE, ZONE_MINIMIZE,
    ZONE_RESIZE_N, ZONE_RESIZE_NE, ZONE_RESIZE_E, ZONE_RESIZE_SE,
    ZONE_RESIZE_S, ZONE_RESIZE_SW, ZONE_RESIZE_W, ZONE_RESIZE_NW,
    ZONE_URL_BAR, ZONE_TAB, ZONE_TAB_CLOSE, ZONE_BTN_BACK,
    ZONE_BTN_FORWARD, ZONE_BTN_RELOAD, ZONE_BTN_STOP, ZONE_BTN_HOME,
    ZONE_BTN_NEW_TAB, ZONE_BTN_ZOOM_IN, ZONE_BTN_ZOOM_OUT,
    ZONE_BTN_ZOOM_RESET, ZONE_BTN_BOOKMARK, ZONE_BTN_SETTINGS,
    ZONE_SETTINGS_CLOSE, ZONE_CONTENT
} NbZone;

typedef struct {
    NbZone zone;
    int    tab_index;
    float  x, y, w, h;
} NbHitRect;

#define NB_MAX_HIT_RECTS 128
#define NB_MAX_TABS 64

/* ------------------------------------------------------------------ */
/*  Tab                                                                */
/* ------------------------------------------------------------------ */
typedef struct NbTab {
    char          title[512];
    char          url[2048];
    NbDocument   *doc;
    NbStylesheet *page_css;
    NbLayout     *layout;
    NbJsEngine   *js;
    NbRenderState render_state;
    int           loading;
    char          error_msg[512];
    int           index;
    /* tab strip geometry (set during draw) */
    float         tab_x, tab_w;
    struct NbTab *prev;
    struct NbTab *next;
} NbTab;

/* ------------------------------------------------------------------ */
/*  Browser                                                             */
/* ------------------------------------------------------------------ */
typedef struct NbBrowser {
    /* Single GTK window, fully custom drawn */
    GtkWidget    *window;
    GtkWidget    *canvas;      /* one GtkDrawingArea covering the whole window */

    /* Theme & DPI */
    NbTheme       theme;
    float         dpi_scale;   /* from GDK monitor */

    /* Geometry (logical pixels, before dpi_scale) */
    int           win_w, win_h;

    /* Chrome geometry (recomputed each draw) */
    float         titlebar_h;
    float         toolbar_h;
    float         tabbar_h;
    float         statusbar_h;
    float         content_x, content_y, content_w, content_h;

    /* Hit rects */
    NbHitRect     hit[NB_MAX_HIT_RECTS];
    int           hit_count;

    /* URL bar editing state */
    char          url_edit[2048];
    int           url_focused;
    int           url_cursor;    /* caret position (char index) */
    int           url_sel_start, url_sel_end;

    /* Tabs */
    NbTab        *tabs;
    NbTab        *active_tab;
    int           tab_count;

    /* History */
    char         *history[512];
    int           history_pos;
    int           history_len;
    char          home_url[512];

    /* UA stylesheet */
    NbStylesheet *ua_css;

    /* Settings panel open */
    int           settings_open;

    /* Drag state for frameless window move */
    int           dragging;
    int           drag_start_x, drag_start_y;

    /* Resize state */
    NbZone        resize_zone;
    int           resize_start_x, resize_start_y;
    int           resize_start_w, resize_start_h;

    /* Hover state */
    NbZone        hover_zone;
    int           hover_tab;

    /* Scroll per tab (managed via GtkScrolledWindow internally as a hack;
       for frameless we track scroll offset ourselves) */
    float         scroll_x, scroll_y;

    /* Maximized state */
    int           maximized;
    int           pre_max_x, pre_max_y, pre_max_w, pre_max_h;
} NbBrowser;

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */
int    nb_browser_run(int argc, char **argv);
void   nb_browser_navigate(NbBrowser *b, NbTab *tab, const char *url);
void   nb_browser_go_back(NbBrowser *b);
void   nb_browser_go_forward(NbBrowser *b);
void   nb_browser_reload(NbBrowser *b);
NbTab *nb_browser_new_tab(NbBrowser *b, const char *url);
void   nb_browser_close_tab(NbBrowser *b, NbTab *tab);
void   nb_browser_queue_draw(NbBrowser *b);

#endif /* NB_BROWSER_H */
