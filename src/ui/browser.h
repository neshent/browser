#ifndef NB_BROWSER_H
#define NB_BROWSER_H

#include "../html/dom.h"
#include "../css/css.h"
#include "../layout/layout.h"
#include "../render/render.h"
#include "../js/js_engine.h"
#include "../net/http.h"
#include <gtk/gtk.h>

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
    /* GTK widgets owned by this tab */
    GtkWidget    *scroll_win;
    GtkWidget    *draw_area;
    GtkWidget    *tab_label;
    struct NbTab *prev;
    struct NbTab *next;
} NbTab;

typedef struct {
    GtkWidget    *window;
    GtkWidget    *vbox_main;
    /* toolbar */
    GtkWidget    *toolbar;
    GtkWidget    *btn_back;
    GtkWidget    *btn_forward;
    GtkWidget    *btn_reload;
    GtkWidget    *btn_stop;
    GtkWidget    *btn_home;
    GtkWidget    *url_entry;
    GtkWidget    *btn_go;
    GtkWidget    *btn_zoom_in;
    GtkWidget    *btn_zoom_out;
    GtkWidget    *btn_zoom_reset;
    GtkWidget    *btn_new_tab;
    GtkWidget    *btn_bookmark;
    /* tab bar */
    GtkWidget    *notebook;
    /* find bar */
    GtkWidget    *find_bar;
    GtkWidget    *find_entry;
    /* status bar */
    GtkWidget    *status_label;
    GtkWidget    *zoom_label;
    /* state */
    char          home_url[512];
    NbStylesheet *ua_css;
    NbTab        *tabs;
    NbTab        *active_tab;
    int           tab_count;
    char         *history[512];
    int           history_pos;
    int           history_len;
} NbBrowser;

int    nb_browser_run(int argc, char **argv);
void   nb_browser_navigate(NbBrowser *b, NbTab *tab, const char *url);
void   nb_browser_go_back(NbBrowser *b);
void   nb_browser_go_forward(NbBrowser *b);
void   nb_browser_reload(NbBrowser *b);
NbTab *nb_browser_new_tab(NbBrowser *b, const char *url);
void   nb_browser_close_tab(NbBrowser *b, NbTab *tab);

#endif
