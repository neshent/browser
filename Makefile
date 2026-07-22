# ===========================================================================
#  Nishant Browser — Makefile
#  Targets: Linux (primary), cross-compile hints for Windows/MSYS2 below.
# ===========================================================================

CC      ?= gcc
BINARY   = nishant-browser

# ---- pkg-config flags ----
GTK_CFLAGS  := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS    := $(shell pkg-config --libs   gtk+-3.0)
CAIRO_CFLAGS:= $(shell pkg-config --cflags cairo)
CAIRO_LIBS  := $(shell pkg-config --libs   cairo)
PANGO_CFLAGS:= $(shell pkg-config --cflags pangocairo)
PANGO_LIBS  := $(shell pkg-config --libs   pangocairo)

# ---- Optional: QuickJS (set QUICKJS_DIR to your QuickJS source tree) ----
# QUICKJS_DIR = third_party/quickjs
# QUICKJS_CFLAGS = -I$(QUICKJS_DIR) -DNB_USE_QUICKJS
# QUICKJS_LIBS   = $(QUICKJS_DIR)/libquickjs.a

# ---- Optional: OpenSSL for HTTPS ----
# OPENSSL_CFLAGS = $(shell pkg-config --cflags openssl) -DNB_USE_OPENSSL
# OPENSSL_LIBS   = $(shell pkg-config --libs   openssl)

CFLAGS = -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter \
         $(GTK_CFLAGS) $(CAIRO_CFLAGS) $(PANGO_CFLAGS)       \
         $(QUICKJS_CFLAGS) $(OPENSSL_CFLAGS)                  \
         -Isrc

LIBS   = $(GTK_LIBS) $(CAIRO_LIBS) $(PANGO_LIBS)             \
         $(QUICKJS_LIBS) $(OPENSSL_LIBS)                      \
         -lm -lpthread

# ---- Sources ----
SRCS = src/main.c                \
       src/util/nb_string.c      \
       src/util/nb_arena.c       \
       src/net/http.c            \
       src/html/dom.c            \
       src/html/html_parser.c    \
       src/css/css.c             \
       src/layout/layout.c       \
       src/render/render.c       \
       src/js/js_engine.c        \
       src/ui/browser.c

OBJS = $(SRCS:.c=.o)

# ---- Rules ----
.PHONY: all clean install

all: $(BINARY)

$(BINARY): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(BINARY)

install: $(BINARY)
	install -Dm755 $(BINARY) $(DESTDIR)/usr/local/bin/$(BINARY)

# ===========================================================================
#  MSYS2 / Windows (MinGW-w64) — uncomment and run in MSYS2 MinGW64 shell:
# ===========================================================================
# CC = x86_64-w64-mingw32-gcc
# CFLAGS += -mwindows    # suppress console window on Windows
# LIBS   += -lws2_32     # Winsock (already pulled in by http.c #pragma comment)
# Everything else is the same — install GTK3 via:
#   pacman -S mingw-w64-x86_64-gtk3 mingw-w64-x86_64-cairo mingw-w64-x86_64-pango
