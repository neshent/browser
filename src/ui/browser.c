#include "browser.h"
#include "../html/html_parser.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <pthread.h>

/* ------------------------------------------------------------------ */
/*  Forward declarations                                               */
/* ------------------------------------------------------------------ */
static void     nav_cb_js(const char *url, void *userdata);
static void     repaint_cb_js(void *userdata);
static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data);
static void     on_url_activate(GtkEntry *e, gpointer data);
static void     on_btn_go(GtkButton *b, gpointer data);
static void     on_btn_back(GtkButton *b, gpointer data);
static void     on_btn_forward(GtkButton *b, gpointer data);
static void     on_btn_reload(GtkButton *b, gpointer data);
static void     on_btn_stop(GtkButton *b, gpointer data);
static void     on_btn_home(GtkButton *b, gpointer data);
static void     on_btn_zoom_in(GtkButton *b, gpointer data);
static void     on_btn_zoom_out(GtkButton *b, gpointer data);
static void     on_btn_zoom_reset(GtkButton *b, gpointer data);
static void     on_btn_new_tab(GtkButton *b, gpointer data);
static void     on_notebook_switch(GtkNotebook *nb, GtkWidget *page, guint idx, gpointer data);
static gboolean on_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer data);
static gboolean on_button_press(GtkWidget *w, GdkEventButton *ev, gpointer data);
static gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer data);
static gboolean timer_pump_cb(gpointer data);

/* ------------------------------------------------------------------ */
/*  CSS colour for toolbar â€” classic grey, visible, no buried menus   */
/* ------------------------------------------------------------------ */
static const char *UI_CSS =
    "window { background-color: #d4d0c8; }"
    ".nb-toolbar { background-color: #d4d0c8; border-bottom: 1px solid #999; padding: 3px 4px; }"
    ".nb-urlbar  { font-size: 13px; padding: 2px 6px; border: 1px inset #888; }"
    ".nb-btn     { padding: 2px 8px; font-size: 12px; }"
    ".nb-status  { font-size: 11px; padding: 1px 6px; background-color: #c8c4bc; border-top: 1px solid #aaa; }"
    ".nb-find    { padding: 2px 4px; background-color: #ffffc0; border-top: 1px solid #bba; }";

/* ------------------------------------------------------------------ */
/*  Tab close button callback                                          */
/* ------------------------------------------------------------------ */
static void on_tab_close_clicked(GtkButton *btn, gpointer d) {
    (void)d;
    NbTab     *tab = (NbTab*)g_object_get_data(G_OBJECT(btn), "tab");
    NbBrowser *br  = (NbBrowser*)g_object_get_data(G_OBJECT(btn), "browser");
    if (tab && br) nb_browser_close_tab(br, tab);
}

/* ------------------------------------------------------------------ */
/*  Tab lifecycle                                                       */
/* ------------------------------------------------------------------ */
static NbTab *tab_new(NbBrowser *b, const char *url) {
    NbTab *t = calloc(1, sizeof(*t));
    if (url) strncpy(t->url, url, sizeof(t->url)-1);
    strncpy(t->title, url ? url : "New Tab", sizeof(t->title)-1);
    t->render_state.scale = 1.0f;

    /* GtkScrolledWindow + GtkDrawingArea */
    t->scroll_win = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(t->scroll_win),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

    t->draw_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(t->draw_area, 800, 4000);
    gtk_widget_add_events(t->draw_area,
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
        GDK_SCROLL_MASK       | GDK_KEY_PRESS_MASK      |
        GDK_POINTER_MOTION_MASK);
    gtk_widget_set_can_focus(t->draw_area, TRUE);

    g_signal_connect(t->draw_area, "draw",              G_CALLBACK(on_draw),         t);
    g_signal_connect(t->draw_area, "scroll-event",      G_CALLBACK(on_scroll),       t);
    g_signal_connect(t->draw_area, "button-press-event",G_CALLBACK(on_button_press), t);
    g_signal_connect(t->draw_area, "key-press-event",   G_CALLBACK(on_key_press),    b);

    gtk_container_add(GTK_CONTAINER(t->scroll_win), t->draw_area);
    gtk_widget_show_all(t->scroll_win);

    /* Tab label with close button */
    GtkWidget *tab_hbox  = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    t->tab_label         = gtk_label_new(t->title);
    gtk_label_set_max_width_chars(GTK_LABEL(t->tab_label), 20);
    gtk_label_set_ellipsize(GTK_LABEL(t->tab_label), PANGO_ELLIPSIZE_END);
    GtkWidget *close_btn = gtk_button_new_from_icon_name("window-close-symbolic",
                                                          GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(close_btn), GTK_RELIEF_NONE);
    g_object_set_data(G_OBJECT(close_btn), "tab", t);
    g_object_set_data(G_OBJECT(close_btn), "browser", b);
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_tab_close_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(tab_hbox), t->tab_label,  TRUE,  TRUE,  0);
    gtk_box_pack_start(GTK_BOX(tab_hbox), close_btn,      FALSE, FALSE, 0);
    gtk_widget_show_all(tab_hbox);

    gtk_notebook_append_page(GTK_NOTEBOOK(b->notebook), t->scroll_win, tab_hbox);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(b->notebook), t->scroll_win, TRUE);

    /* Link into list */
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

/* ------------------------------------------------------------------ */
/*  Navigation                                                         */
/* ------------------------------------------------------------------ */
/* Background load thread data */
typedef struct {
    NbBrowser *browser;
    NbTab     *tab;
    char       url[2048];
} LoadJob;

static void tab_set_title(NbBrowser *b, NbTab *tab, const char *title) {
    strncpy(tab->title, title, sizeof(tab->title)-1);
    if (tab->tab_label)
        gtk_label_set_text(GTK_LABEL(tab->tab_label), title);
    if (tab == b->active_tab)
        gtk_window_set_title(GTK_WINDOW(b->window), title);
}

static void update_nav_buttons(NbBrowser *b) {
    gtk_widget_set_sensitive(b->btn_back,    b->history_pos > 0);
    gtk_widget_set_sensitive(b->btn_forward, b->history_pos < b->history_len - 1);
}

static void status_set(NbBrowser *b, const char *msg) {
    gtk_label_set_text(GTK_LABEL(b->status_label), msg ? msg : "");
}

static void zoom_label_update(NbBrowser *b) {
    char buf[32];
    float z = b->active_tab ? b->active_tab->render_state.scale : 1.0f;
    snprintf(buf, sizeof(buf), "%.0f%%", z * 100.0f);
    gtk_label_set_text(GTK_LABEL(b->zoom_label), buf);
}

static void request_redraw(NbBrowser *b, NbTab *tab) {
    if (tab && tab->draw_area)
        gtk_widget_queue_draw(tab->draw_area);
}

/* Called on main thread after load completes */
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

    /* Free previous page data */
    if (tab->js)       { nb_js_engine_free(tab->js);   tab->js       = NULL; }
    if (tab->layout)   { nb_layout_free(tab->layout);  tab->layout   = NULL; }
    if (tab->page_css) { nb_css_free(tab->page_css);   tab->page_css = NULL; }
    if (tab->doc)      { nb_doc_free(tab->doc);         tab->doc      = NULL; }

    tab->doc      = r->doc;
    tab->page_css = r->css;
    strncpy(tab->url,       r->url,   sizeof(tab->url)-1);
    strncpy(tab->error_msg, r->error, sizeof(tab->error_msg)-1);

    if (r->doc && !r->error[0]) {
        /* Apply styles */
        nb_css_apply(b->ua_css, tab->page_css, tab->doc);

        /* Build layout */
        GtkAllocation alloc;
        gtk_widget_get_allocation(tab->draw_area, &alloc);
        float vw = alloc.width  > 10 ? alloc.width  : 800;
        float vh = alloc.height > 10 ? alloc.height : 600;
        tab->layout = nb_layout_build(tab->doc, vw, vh);

        /* Resize drawing area to content height */
        if (tab->layout && tab->layout->root) {
            int ch = (int)(tab->layout->root->height + 200);
            if (ch < (int)vh) ch = (int)vh;
            gtk_widget_set_size_request(tab->draw_area, (int)vw, ch);
        }

        /* JS engine */
        tab->js = nb_js_engine_new(tab->doc, nav_cb_js, repaint_cb_js, tab);
        nb_js_run_scripts(tab->js);

        /* Title from <title> tag */
        NbNode *title_node = nb_doc_by_tag(tab->doc->root, "title");
        if (title_node && title_node->first_child && title_node->first_child->text)
            tab_set_title(b, tab, title_node->first_child->text);
        else
            tab_set_title(b, tab, tab->url);

        /* Update URL bar if this is active tab */
        if (tab == b->active_tab)
            gtk_entry_set_text(GTK_ENTRY(b->url_entry), tab->url);

        status_set(b, "Done");
    } else {
        tab_set_title(b, tab, "Error");
        status_set(b, tab->error_msg[0] ? tab->error_msg : "Unknown error");
    }

    tab->loading = 0;
    gtk_widget_set_sensitive(b->btn_stop, FALSE);
    gtk_widget_set_sensitive(b->btn_reload, TRUE);
    update_nav_buttons(b);
    request_redraw(b, tab);

    free(r);
    return G_SOURCE_REMOVE;
}

/* Worker thread: fetches URL, parses HTML + CSS */
static void *load_thread(void *arg) {
    LoadJob *job = (LoadJob*)arg;
    LoadResult *res = calloc(1, sizeof(*res));
    res->b   = job->browser;
    res->tab = job->tab;
    strncpy(res->url, job->url, sizeof(res->url)-1);

    /* Handle about:blank and data: URIs */
    if (strcmp(job->url, "about:blank") == 0 || strcmp(job->url, "about:newtab") == 0) {
        const char *blank = "<html><body style='margin:40px;font-family:sans-serif;'>"
                            "<h2 style='color:#555'>Nishant Browser</h2>"
                            "<p>Enter a URL above to get started.</p></body></html>";
        res->doc = nb_html_parse(blank, strlen(blank));
        strncpy(res->title, "New Tab", sizeof(res->title)-1);
        goto done;
    }

    NbHttpResponse resp = nb_http_get(job->url);

    /* Follow up to 5 redirects */
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
        snprintf(res->error, sizeof(res->error), "Connection failed (status 0)");
        nb_http_response_free(&resp);
        goto done;
    }

    /* Parse HTML */
    if (resp.body.data && resp.body.len > 0) {
        res->doc = nb_html_parse(resp.body.data, resp.body.len);
    }

    /* Collect <style> tags and <link rel=stylesheet> */
    {
        NbStr combined_css = nb_str_new();
        if (res->doc) {
            /* Walk DOM for <style> nodes */
            NbNode *stack[512]; int top = 0;
            stack[top++] = res->doc->root;
            while (top > 0) {
                NbNode *n = stack[--top];
                if (n->type == NODE_ELEMENT) {
                    if (strcmp(n->tag, "style") == 0) {
                        for (NbNode *c = n->first_child; c; c = c->next_sibling)
                            if (c->type == NODE_TEXT && c->text)
                                nb_str_appends(&combined_css, c->text);
                    }
                    /* <link rel="stylesheet"> â€” fetch inline (best-effort) */
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

    /* Prepend scheme if missing */
    char full_url[2048];
    if (strncmp(url,"http://",7)!=0 && strncmp(url,"https://",8)!=0 &&
        strncmp(url,"about:",6)!=0  && strncmp(url,"file:",5)!=0) {
        /* treat bare domains as https:// */
        snprintf(full_url, sizeof(full_url), "https://%s", url);
    } else {
        strncpy(full_url, url, sizeof(full_url)-1);
    }

    /* History */
    if (b->history_pos < b->history_len - 1) {
        /* truncate forward history */
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
    gtk_widget_set_sensitive(b->btn_stop,   TRUE);
    gtk_widget_set_sensitive(b->btn_reload, FALSE);
    status_set(b, "Loading...");
    if (tab == b->active_tab)
        gtk_entry_set_text(GTK_ENTRY(b->url_entry), full_url);

    /* Request redraw to show loading page */
    request_redraw(b, tab);

    /* Spawn load thread */
    LoadJob *job = calloc(1, sizeof(*job));
    job->browser = b; job->tab = tab;
    strncpy(job->url, full_url, sizeof(job->url)-1);
    pthread_t tid;
    pthread_create(&tid, NULL, load_thread, job);
    pthread_detach(tid);
}

void nb_browser_go_back(NbBrowser *b) {
    if (!b || b->history_pos <= 0) return;
    b->history_pos--;
    NbTab *tab = b->active_tab;
    if (tab && b->history[b->history_pos]) {
        /* direct load without re-adding to history */
        LoadJob *job = calloc(1,sizeof(*job));
        job->browser=b; job->tab=tab;
        strncpy(job->url, b->history[b->history_pos], sizeof(job->url)-1);
        pthread_t tid; pthread_create(&tid,NULL,load_thread,job); pthread_detach(tid);
        status_set(b,"Loading..."); tab->loading=1; request_redraw(b,tab);
    }
    update_nav_buttons(b);
}

void nb_browser_go_forward(NbBrowser *b) {
    if (!b || b->history_pos >= b->history_len-1) return;
    b->history_pos++;
    NbTab *tab = b->active_tab;
    if (tab && b->history[b->history_pos]) {
        LoadJob *job = calloc(1,sizeof(*job));
        job->browser=b; job->tab=tab;
        strncpy(job->url, b->history[b->history_pos], sizeof(job->url)-1);
        pthread_t tid; pthread_create(&tid,NULL,load_thread,job); pthread_detach(tid);
        status_set(b,"Loading..."); tab->loading=1; request_redraw(b,tab);
    }
    update_nav_buttons(b);
}

void nb_browser_reload(NbBrowser *b) {
    if (!b || !b->active_tab) return;
    nb_browser_navigate(b, b->active_tab, b->active_tab->url);
}

void nb_browser_stop(NbBrowser *b) {
    /* Mark loading=0; the thread will still complete but the result is stale */
    if (b && b->active_tab) { b->active_tab->loading = 0; status_set(b,"Stopped"); }
    gtk_widget_set_sensitive(b->btn_stop,   FALSE);
    gtk_widget_set_sensitive(b->btn_reload, TRUE);
}

NbTab *nb_browser_new_tab(NbBrowser *b, const char *url) {
    NbTab *t = tab_new(b, url);
    int page = gtk_notebook_page_num(GTK_NOTEBOOK(b->notebook), t->scroll_win);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(b->notebook), page);
    b->active_tab = t;
    if (url && url[0]) nb_browser_navigate(b, t, url);
    return t;
}

void nb_browser_close_tab(NbBrowser *b, NbTab *tab) {
    if (!b || !tab || b->tab_count <= 1) return;
    int page = gtk_notebook_page_num(GTK_NOTEBOOK(b->notebook), tab->scroll_win);
    gtk_notebook_remove_page(GTK_NOTEBOOK(b->notebook), page);
    /* Unlink */
    if (tab->prev) tab->prev->next = tab->next;
    else           b->tabs = tab->next;
    if (tab->next) tab->next->prev = tab->prev;
    b->tab_count--;
    if (b->active_tab == tab) {
        b->active_tab = b->tabs;
        if (b->active_tab)
            gtk_entry_set_text(GTK_ENTRY(b->url_entry), b->active_tab->url);
    }
    tab_free(tab);
}

/* ------------------------------------------------------------------ */
/*  JS callbacks (called from JS thread â†’ must post to GTK main)      */
/* ------------------------------------------------------------------ */
static void nav_cb_js(const char *url, void *userdata) {
    NbTab *tab = (NbTab*)userdata;
    /* We need the browser pointer â€” store it on the tab */
    /* For now navigate via global; in real code store b* on tab */
    (void)tab; (void)url;
}
static void repaint_cb_js(void *userdata) {
    NbTab *tab = (NbTab*)userdata;
    if (tab && tab->draw_area)
        gtk_widget_queue_draw(tab->draw_area);
}

/* ------------------------------------------------------------------ */
/*  GTK draw callback                                                  */
/* ------------------------------------------------------------------ */
static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
    NbTab *tab = (NbTab*)data;
    if (!tab) return FALSE;

    GtkAllocation alloc;
    gtk_widget_get_allocation(w, &alloc);
    float vw = alloc.width;
    float vh = alloc.height;

    if (tab->loading) {
        nb_render_loading_page(cr, vw, vh, tab->url);
        return FALSE;
    }
    if (tab->error_msg[0]) {
        nb_render_error_page(cr, vw, vh, tab->error_msg);
        return FALSE;
    }
    if (!tab->layout) {
        /* blank */
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_paint(cr);
        return FALSE;
    }

    /* Reflow if width changed */
    if (tab->layout && fabsf(tab->layout->viewport_width - vw) > 1.0f) {
        nb_layout_reflow(tab->layout, vw, vh);
        if (tab->layout->root) {
            int ch = (int)(tab->layout->root->height + 200);
            if (ch < (int)vh) ch = (int)vh;
            gtk_widget_set_size_request(w, (int)vw, ch);
        }
    }

    nb_render_paint(cr, tab->layout, &tab->render_state);
    return FALSE;
}

/* ------------------------------------------------------------------ */
/*  Toolbar callbacks                                                  */
/* ------------------------------------------------------------------ */
static void navigate_active(NbBrowser *b, const char *url) {
    if (!b->active_tab) nb_browser_new_tab(b, url);
    else                nb_browser_navigate(b, b->active_tab, url);
}

static void on_url_activate(GtkEntry *e, gpointer data) {
    NbBrowser *b = (NbBrowser*)data;
    navigate_active(b, gtk_entry_get_text(e));
}
static void on_btn_go(GtkButton *btn, gpointer data) {
    NbBrowser *b = (NbBrowser*)data;
    navigate_active(b, gtk_entry_get_text(GTK_ENTRY(b->url_entry)));
}
static void on_btn_back(GtkButton *btn, gpointer data)    { nb_browser_go_back((NbBrowser*)data); }
static void on_btn_forward(GtkButton *btn, gpointer data) { nb_browser_go_forward((NbBrowser*)data); }
static void on_btn_reload(GtkButton *btn, gpointer data)  { nb_browser_reload((NbBrowser*)data); }
static void on_btn_stop(GtkButton *btn, gpointer data)    { nb_browser_stop((NbBrowser*)data); }
static void on_btn_home(GtkButton *btn, gpointer data) {
    NbBrowser *b = (NbBrowser*)data;
    navigate_active(b, b->home_url);
}
static void on_btn_zoom_in(GtkButton *btn, gpointer data) {
    NbBrowser *b = (NbBrowser*)data;
    if (b->active_tab) { b->active_tab->render_state.scale += 0.1f; zoom_label_update(b); request_redraw(b,b->active_tab); }
}
static void on_btn_zoom_out(GtkButton *btn, gpointer data) {
    NbBrowser *b = (NbBrowser*)data;
    if (b->active_tab && b->active_tab->render_state.scale > 0.2f) {
        b->active_tab->render_state.scale -= 0.1f; zoom_label_update(b); request_redraw(b,b->active_tab);
    }
}
static void on_btn_zoom_reset(GtkButton *btn, gpointer data) {
    NbBrowser *b = (NbBrowser*)data;
    if (b->active_tab) { b->active_tab->render_state.scale = 1.0f; zoom_label_update(b); request_redraw(b,b->active_tab); }
}
static void on_btn_new_tab(GtkButton *btn, gpointer data) {
    nb_browser_new_tab((NbBrowser*)data, "about:blank");
}

static void on_notebook_switch(GtkNotebook *nb, GtkWidget *page, guint idx, gpointer data) {
    NbBrowser *b = (NbBrowser*)data;
    /* Find tab whose scroll_win is `page` */
    for (NbTab *t = b->tabs; t; t = t->next) {
        if (t->scroll_win == page) {
            b->active_tab = t;
            gtk_entry_set_text(GTK_ENTRY(b->url_entry), t->url);
            gtk_window_set_title(GTK_WINDOW(b->window), t->title);
            update_nav_buttons(b);
            zoom_label_update(b);
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Mouse / keyboard events                                            */
/* ------------------------------------------------------------------ */
static gboolean on_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer data) {
    NbTab *tab = (NbTab*)data;
    if (!tab) return FALSE;
    /* Scrolling is handled by GtkScrolledWindow; we track scroll_y for render */
    if (ev->direction == GDK_SCROLL_DOWN)      tab->render_state.scroll_y += 60;
    else if (ev->direction == GDK_SCROLL_UP)   tab->render_state.scroll_y -= 60;
    if (tab->render_state.scroll_y < 0) tab->render_state.scroll_y = 0;
    gtk_widget_queue_draw(w);
    return FALSE;
}

static gboolean on_button_press(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    NbTab *tab = (NbTab*)data;
    if (!tab || !tab->layout) return FALSE;
    gtk_widget_grab_focus(w);

    float x = ev->x + tab->render_state.scroll_x;
    float y = ev->y + tab->render_state.scroll_y;
    NbBox *box = nb_layout_box_at(tab->layout, x, y);
    if (!box || !box->node) return FALSE;

    NbNode *n = box->node;
    /* Walk up to find clickable: <a>, <button>, <input>, <label> */
    while (n && n->type == NODE_ELEMENT) {
        if (strcmp(n->tag,"a")==0) {
            const char *href = nb_attr_val(n,"href");
            if (href && href[0]) {
                /* Need browser pointer â€” store it globally for now */
                /* Actual navigation happens via the stored browser pointer */
            }
            break;
        }
        if (strcmp(n->tag,"button")==0 || strcmp(n->tag,"input")==0) {
            if (tab->js) nb_js_fire_event(tab->js, n, "click");
            break;
        }
        n = n->parent;
    }
    return FALSE;
}

static gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer data) {
    NbBrowser *b = (NbBrowser*)data;
    if (!b) return FALSE;
    /* Ctrl+L â€” focus URL bar */
    if ((ev->state & GDK_CONTROL_MASK) && ev->keyval == GDK_KEY_l) {
        gtk_widget_grab_focus(b->url_entry);
        gtk_editable_select_region(GTK_EDITABLE(b->url_entry), 0, -1);
        return TRUE;
    }
    /* Ctrl+T â€” new tab */
    if ((ev->state & GDK_CONTROL_MASK) && ev->keyval == GDK_KEY_t) {
        nb_browser_new_tab(b, "about:blank"); return TRUE;
    }
    /* Ctrl+W â€” close tab */
    if ((ev->state & GDK_CONTROL_MASK) && ev->keyval == GDK_KEY_w) {
        nb_browser_close_tab(b, b->active_tab); return TRUE;
    }
    /* Ctrl+R â€” reload */
    if ((ev->state & GDK_CONTROL_MASK) && ev->keyval == GDK_KEY_r) {
        nb_browser_reload(b); return TRUE;
    }
    /* F5 â€” reload */
    if (ev->keyval == GDK_KEY_F5) { nb_browser_reload(b); return TRUE; }
    /* Alt+Left â€” back */
    if ((ev->state & GDK_MOD1_MASK) && ev->keyval == GDK_KEY_Left) {
        nb_browser_go_back(b); return TRUE;
    }
    /* Alt+Right â€” forward */
    if ((ev->state & GDK_MOD1_MASK) && ev->keyval == GDK_KEY_Right) {
        nb_browser_go_forward(b); return TRUE;
    }
    /* Ctrl++ / Ctrl+- zoom */
    if ((ev->state & GDK_CONTROL_MASK) && (ev->keyval==GDK_KEY_plus||ev->keyval==GDK_KEY_equal)) {
        on_btn_zoom_in(NULL,b); return TRUE;
    }
    if ((ev->state & GDK_CONTROL_MASK) && ev->keyval==GDK_KEY_minus) {
        on_btn_zoom_out(NULL,b); return TRUE;
    }
    if ((ev->state & GDK_CONTROL_MASK) && ev->keyval==GDK_KEY_0) {
        on_btn_zoom_reset(NULL,b); return TRUE;
    }
    return FALSE;
}

static gboolean timer_pump_cb(gpointer data) {
    NbBrowser *b = (NbBrowser*)data;
    if (b->active_tab && b->active_tab->js)
        nb_js_pump_timers(b->active_tab->js);
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ */
/*  Window construction                                                */
/* ------------------------------------------------------------------ */
static GtkWidget *make_tb_button(const char *icon, const char *tooltip) {
    GtkWidget *btn = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_SMALL_TOOLBAR);
    gtk_widget_set_tooltip_text(btn, tooltip);
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(btn), "nb-btn");
    return btn;
}

static GtkWidget *make_tb_label(const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_style_context_add_class(gtk_widget_get_style_context(l), "nb-btn");
    return l;
}

static void build_ui(NbBrowser *b) {
    /* ---- Apply CSS theme ---- */
    GtkCssProvider *css_prov = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css_prov, UI_CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css_prov), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    /* ---- Main window ---- */
    b->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(b->window), "Nishant Browser");
    gtk_window_set_default_size(GTK_WINDOW(b->window), 1024, 768);
    gtk_window_set_icon_name(GTK_WINDOW(b->window), "web-browser");
    g_signal_connect(b->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(b->window, "key-press-event", G_CALLBACK(on_key_press), b);

    /* ---- Outer vbox ---- */
    b->vbox_main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(b->window), b->vbox_main);

    /* ================================================================
     *  TOOLBAR â€” all controls visible, nothing hidden in menus
     * ================================================================ */
    b->toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_style_context_add_class(gtk_widget_get_style_context(b->toolbar), "nb-toolbar");
    gtk_box_pack_start(GTK_BOX(b->vbox_main), b->toolbar, FALSE, FALSE, 0);

    /* Navigation buttons */
    b->btn_back    = make_tb_button("go-previous-symbolic",    "Back (Alt+Left)");
    b->btn_forward = make_tb_button("go-next-symbolic",        "Forward (Alt+Right)");
    b->btn_reload  = make_tb_button("view-refresh-symbolic",   "Reload (F5 / Ctrl+R)");
    b->btn_stop    = make_tb_button("process-stop-symbolic",   "Stop loading");
    b->btn_home    = make_tb_button("go-home-symbolic",        "Home page");
    gtk_box_pack_start(GTK_BOX(b->toolbar), b->btn_back,    FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(b->toolbar), b->btn_forward, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(b->toolbar), b->btn_reload,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(b->toolbar), b->btn_stop,    FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(b->toolbar), b->btn_home,    FALSE, FALSE, 0);

    /* Separator */
    gtk_box_pack_start(GTK_BOX(b->toolbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE,FALSE,2);

    /* URL bar â€” expands to fill space */
    b->url_entry = gtk_entry_new();
    gtk_widget_set_tooltip_text(b->url_entry, "Address bar (Ctrl+L)");
    gtk_style_context_add_class(gtk_widget_get_style_context(b->url_entry), "nb-urlbar");
    gtk_entry_set_placeholder_text(GTK_ENTRY(b->url_entry), "Enter URL...");
    gtk_box_pack_start(GTK_BOX(b->toolbar), b->url_entry, TRUE, TRUE, 4);

    b->btn_go = make_tb_button("go-jump-symbolic", "Go (Enter)");
    gtk_box_pack_start(GTK_BOX(b->toolbar), b->btn_go, FALSE, FALSE, 0);

    /* Separator */
    gtk_box_pack_start(GTK_BOX(b->toolbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE,FALSE,2);

    /* Zoom controls â€” always visible */
    b->btn_zoom_out   = make_tb_button("zoom-out-symbolic",     "Zoom out (Ctrl+-)");
    b->zoom_label     = make_tb_label("100%");
    b->btn_zoom_in    = make_tb_button("zoom-in-symbolic",      "Zoom in (Ctrl++)");
    b->btn_bookmark   = make_tb_button("user-bookmarks-symbolic","Bookmarks");
    b->btn_new_tab    = make_tb_button("tab-new-symbolic",       "New tab (Ctrl+T)");
    gtk_box_pack_start(GTK_BOX(b->toolbar), b->btn_zoom_out,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(b->toolbar), b->zoom_label,    FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(b->toolbar), b->btn_zoom_in,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(b->toolbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL),FALSE,FALSE,2);
    gtk_box_pack_start(GTK_BOX(b->toolbar), b->btn_bookmark,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(b->toolbar), b->btn_new_tab,   FALSE, FALSE, 0);

    /* ================================================================
     *  TAB NOTEBOOK
     * ================================================================ */
    b->notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(b->notebook), TRUE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(b->notebook), FALSE);
    gtk_box_pack_start(GTK_BOX(b->vbox_main), b->notebook, TRUE, TRUE, 0);

    /* ================================================================
     *  FIND BAR (hidden by default)
     * ================================================================ */
    b->find_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(b->find_bar), "nb-find");
    gtk_box_pack_start(GTK_BOX(b->find_bar), gtk_label_new("Find:"), FALSE,FALSE,4);
    b->find_entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(b->find_bar), b->find_entry, TRUE,TRUE,0);
    GtkWidget *fn  = make_tb_button("go-previous-symbolic","Find previous");
    GtkWidget *fp  = make_tb_button("go-next-symbolic","Find next");
    GtkWidget *fcl = make_tb_button("window-close-symbolic","Close find bar");
    gtk_box_pack_start(GTK_BOX(b->find_bar), fn,  FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(b->find_bar), fp,  FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(b->find_bar), fcl, FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(b->vbox_main), b->find_bar, FALSE,FALSE,0);
    g_signal_connect_swapped(fcl,"clicked",G_CALLBACK(gtk_widget_hide), b->find_bar);

    /* ================================================================
     *  STATUS BAR â€” always visible, shows URL on hover + load state
     * ================================================================ */
    GtkWidget *status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(status_box), "nb-status");
    b->status_label = gtk_label_new("Ready");
    gtk_label_set_xalign(GTK_LABEL(b->status_label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(b->status_label), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(status_box), b->status_label, TRUE,TRUE,0);
    gtk_box_pack_start(GTK_BOX(b->vbox_main), status_box, FALSE,FALSE,0);

    /* ================================================================
     *  Wire signals
     * ================================================================ */
    g_signal_connect(b->url_entry, "activate",       G_CALLBACK(on_url_activate), b);
    g_signal_connect(b->btn_go,    "clicked",         G_CALLBACK(on_btn_go),      b);
    g_signal_connect(b->btn_back,  "clicked",         G_CALLBACK(on_btn_back),    b);
    g_signal_connect(b->btn_forward,"clicked",        G_CALLBACK(on_btn_forward), b);
    g_signal_connect(b->btn_reload,"clicked",         G_CALLBACK(on_btn_reload),  b);
    g_signal_connect(b->btn_stop,  "clicked",         G_CALLBACK(on_btn_stop),    b);
    g_signal_connect(b->btn_home,  "clicked",         G_CALLBACK(on_btn_home),    b);
    g_signal_connect(b->btn_zoom_in,"clicked",        G_CALLBACK(on_btn_zoom_in), b);
    g_signal_connect(b->btn_zoom_out,"clicked",       G_CALLBACK(on_btn_zoom_out),b);
    g_signal_connect(b->btn_zoom_reset,"clicked",     G_CALLBACK(on_btn_zoom_reset),b);
    g_signal_connect(b->btn_new_tab,"clicked",        G_CALLBACK(on_btn_new_tab), b);
    g_signal_connect(b->notebook, "switch-page",      G_CALLBACK(on_notebook_switch),b);

    /* Initial button states */
    gtk_widget_set_sensitive(b->btn_back,    FALSE);
    gtk_widget_set_sensitive(b->btn_forward, FALSE);
    gtk_widget_set_sensitive(b->btn_stop,    FALSE);

    gtk_widget_show_all(b->window);
    gtk_widget_hide(b->find_bar);

    /* JS timer pump every 100ms */
    g_timeout_add(100, timer_pump_cb, b);
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                        */
/* ------------------------------------------------------------------ */
int nb_browser_run(int argc, char **argv) {
    gtk_init(&argc, &argv);

    NbBrowser *b = calloc(1, sizeof(*b));
    strncpy(b->home_url, "about:blank", sizeof(b->home_url)-1);
    b->ua_css = nb_css_ua_stylesheet();
    b->history_pos = -1;
    b->history_len =  0;

    build_ui(b);

    /* Open initial tab */
    const char *start_url = (argc > 1) ? argv[1] : "about:blank";
    nb_browser_new_tab(b, start_url);

    gtk_main();

    /* Cleanup */
    NbTab *t = b->tabs;
    while (t) { NbTab *n=t->next; tab_free(t); t=n; }
    nb_css_free(b->ua_css);
    for (int i=0; i<b->history_len; i++) free(b->history[i]);
    free(b);
    return 0;
}
