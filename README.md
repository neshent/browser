# Nishant Browser

A lightweight, fast web browser written from scratch in C.  
No Chrome. No Firefox. No Electron. Just C, GTK3, Cairo, Pango, and optionally QuickJS + OpenSSL.

---

## Features

- **Custom HTTP/HTTPS client** — raw sockets, optional OpenSSL TLS
- **Custom HTML parser** — HTML5-inspired tokenizer + tree builder
- **Custom CSS engine** — cascade, inheritance, selector matching, flexbox, block/inline layout
- **Custom layout engine** — block, inline, flexbox, absolute/fixed positioning
- **Cairo + Pango rendering** — proper Unicode text, font families, decorations, borders
- **QuickJS JavaScript** — `document.getElementById`, `fetch()`, `setTimeout`, `localStorage` stub
- **Classic oldschool UI** — every control is visible on the toolbar, nothing buried in menus:
  - Back / Forward / Reload / Stop / Home
  - Address bar (Ctrl+L to focus)
  - Zoom In / Zoom Out / Zoom Reset (always visible %)
  - New Tab button
  - Bookmarks button
  - Find bar (Ctrl+F)
  - Status bar always visible at the bottom
- **Tabbed browsing** — Ctrl+T / Ctrl+W, unlimited tabs
- **Keyboard shortcuts**: Ctrl+L, Ctrl+T, Ctrl+W, Ctrl+R, F5, Alt+Left/Right, Ctrl+±/0

---

## Building on Linux

### Dependencies

```bash
# Ubuntu / Debian
sudo apt install build-essential libgtk-3-dev libcairo2-dev libpango1.0-dev

# Fedora / RHEL
sudo dnf install gcc gtk3-devel cairo-devel pango-devel

# Arch Linux
sudo pacman -S base-devel gtk3 cairo pango
```

### Optional: HTTPS (OpenSSL)

```bash
sudo apt install libssl-dev
```

Then uncomment the `OPENSSL_CFLAGS` / `OPENSSL_LIBS` lines in the `Makefile`.

### Optional: JavaScript (QuickJS)

```bash
git clone https://github.com/bellard/quickjs third_party/quickjs
cd third_party/quickjs && make libquickjs.a && cd ../..
```

Then uncomment the `QUICKJS_DIR` lines in the `Makefile`.

### Build and Run

```bash
make -j$(nproc)
./nishant-browser                        # opens about:blank
./nishant-browser https://example.com    # opens a URL directly
```

---

## Building on Windows (MSYS2)

1. Install [MSYS2](https://www.msys2.org/)
2. Open **MSYS2 MinGW64** shell:

```bash
pacman -S mingw-w64-x86_64-gcc \
          mingw-w64-x86_64-gtk3 \
          mingw-w64-x86_64-cairo \
          mingw-w64-x86_64-pango \
          mingw-w64-x86_64-pkg-config
make -j4
./nishant-browser.exe
```

---

## Project Structure

```
nishant-browser/
├── src/
│   ├── main.c                  # Entry point
│   ├── util/
│   │   ├── nb_string.{h,c}     # Dynamic strings + string views
│   │   └── nb_arena.{h,c}      # Bump-pointer arena allocator
│   ├── net/
│   │   └── http.{h,c}          # HTTP/HTTPS client (raw sockets)
│   ├── html/
│   │   ├── dom.{h,c}           # DOM tree
│   │   └── html_parser.{h,c}   # HTML tokenizer + tree builder
│   ├── css/
│   │   └── css.{h,c}           # CSS parser + style cascade
│   ├── layout/
│   │   └── layout.{h,c}        # Box model + layout engine
│   ├── render/
│   │   └── render.{h,c}        # Cairo/Pango paint layer
│   ├── js/
│   │   └── js_engine.{h,c}     # QuickJS bridge + DOM API
│   └── ui/
│       └── browser.{h,c}       # GTK3 UI + navigation logic
├── third_party/
│   └── quickjs/                # QuickJS source (git clone separately)
├── assets/                     # Icons, default CSS, etc.
├── Makefile
└── README.md
```

---

## Architecture

```
URL input
    │
    ▼
HTTP client (src/net/http.c)
    │  raw TCP socket + optional TLS
    ▼
HTML parser (src/html/html_parser.c)
    │  tokenizer → DOM tree
    ▼
CSS engine (src/css/css.c)
    │  parse stylesheets → cascade → computed styles on each node
    ▼
Layout engine (src/layout/layout.c)
    │  block/inline/flex box tree → x,y,w,h for every element
    ▼
Renderer (src/render/render.c)
    │  Cairo surfaces, Pango text, borders, backgrounds
    ▼
GTK3 DrawingArea (src/ui/browser.c)
    │  on_draw() → nb_render_paint()
    ▼
Screen
```

JavaScript runs alongside rendering:
- QuickJS executes `<script>` tags after parse
- DOM mutations trigger layout reflow + redraw
- `setTimeout` callbacks pumped every 100ms via GTK timer

---

## Known Limitations

- No image decoding yet (img elements show alt text placeholder — add libpng/libjpeg to render.c)
- No WebAssembly
- No WebGL / Canvas 2D
- No Service Workers / Web Workers
- CSS Grid not yet implemented (flexbox and block work)
- Complex JS-heavy SPAs may not work fully without the full QuickJS DOM API surface

---

## License

MIT — do whatever you want with it.
