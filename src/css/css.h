#ifndef NB_CSS_H
#define NB_CSS_H

#include "../html/dom.h"
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  CSS value types                                                    */
/* ------------------------------------------------------------------ */

typedef enum {
    CSS_UNIT_PX, CSS_UNIT_EM, CSS_UNIT_REM, CSS_UNIT_PCT,
    CSS_UNIT_PT, CSS_UNIT_VW, CSS_UNIT_VH, CSS_UNIT_AUTO, CSS_UNIT_NONE
} CssUnit;

typedef struct {
    float   value;
    CssUnit unit;
} CssLength;

typedef struct {
    uint8_t r, g, b, a;
    int     is_transparent;
} CssColor;

typedef enum {
    DISPLAY_BLOCK, DISPLAY_INLINE, DISPLAY_INLINE_BLOCK,
    DISPLAY_FLEX,  DISPLAY_NONE,   DISPLAY_LIST_ITEM,
    DISPLAY_TABLE, DISPLAY_TABLE_ROW, DISPLAY_TABLE_CELL,
    DISPLAY_TABLE_ROW_GROUP
} CssDisplay;

typedef enum {
    POSITION_STATIC, POSITION_RELATIVE, POSITION_ABSOLUTE, POSITION_FIXED, POSITION_STICKY
} CssPosition;

typedef enum {
    FLOAT_NONE, FLOAT_LEFT, FLOAT_RIGHT
} CssFloat;

typedef enum {
    FONT_STYLE_NORMAL, FONT_STYLE_ITALIC, FONT_STYLE_OBLIQUE
} CssFontStyle;

typedef enum {
    TEXT_ALIGN_LEFT, TEXT_ALIGN_CENTER, TEXT_ALIGN_RIGHT, TEXT_ALIGN_JUSTIFY
} CssTextAlign;

typedef enum {
    TEXT_DECORATION_NONE = 0,
    TEXT_DECORATION_UNDERLINE   = 1 << 0,
    TEXT_DECORATION_OVERLINE    = 1 << 1,
    TEXT_DECORATION_LINE_THROUGH= 1 << 2
} CssTextDecoration;

typedef enum {
    OVERFLOW_VISIBLE, OVERFLOW_HIDDEN, OVERFLOW_SCROLL, OVERFLOW_AUTO
} CssOverflow;

typedef enum {
    BORDER_STYLE_NONE, BORDER_STYLE_SOLID, BORDER_STYLE_DASHED,
    BORDER_STYLE_DOTTED, BORDER_STYLE_DOUBLE, BORDER_STYLE_GROOVE,
    BORDER_STYLE_RIDGE,  BORDER_STYLE_INSET,  BORDER_STYLE_OUTSET
} CssBorderStyle;

typedef struct {
    CssLength     width;
    CssBorderStyle style;
    CssColor      color;
} CssBorder;

typedef enum {
    FLEX_DIR_ROW, FLEX_DIR_ROW_REVERSE, FLEX_DIR_COLUMN, FLEX_DIR_COLUMN_REVERSE
} CssFlexDirection;

typedef enum {
    FLEX_WRAP_NOWRAP, FLEX_WRAP_WRAP, FLEX_WRAP_WRAP_REVERSE
} CssFlexWrap;

typedef enum {
    ALIGN_AUTO, ALIGN_FLEX_START, ALIGN_FLEX_END, ALIGN_CENTER,
    ALIGN_STRETCH, ALIGN_BASELINE, ALIGN_SPACE_BETWEEN, ALIGN_SPACE_AROUND
} CssAlign;

typedef enum {
    VISIBILITY_VISIBLE, VISIBILITY_HIDDEN, VISIBILITY_COLLAPSE
} CssVisibility;

typedef enum {
    WHITE_SPACE_NORMAL, WHITE_SPACE_NOWRAP, WHITE_SPACE_PRE,
    WHITE_SPACE_PRE_WRAP, WHITE_SPACE_PRE_LINE
} CssWhiteSpace;

/* ------------------------------------------------------------------ */
/*  Computed style — one per node, fully resolved, no inheritance     */
/*  holes (all fields have a value).                                  */
/* ------------------------------------------------------------------ */
typedef struct NbStyle {
    /* Box model */
    CssLength     width, height;
    CssLength     min_width, min_height, max_width, max_height;
    CssLength     margin_top, margin_right, margin_bottom, margin_left;
    CssLength     padding_top, padding_right, padding_bottom, padding_left;
    CssBorder     border_top, border_right, border_bottom, border_left;
    CssLength     top, right, bottom, left;

    /* Visual */
    CssColor      color;
    CssColor      background_color;
    CssDisplay    display;
    CssPosition   position;
    CssFloat      float_;
    CssOverflow   overflow_x, overflow_y;
    CssVisibility visibility;
    float         opacity;
    int           z_index;

    /* Typography */
    char          font_family[64];
    float         font_size;    /* px, already resolved */
    int           font_weight;  /* 100-900 */
    CssFontStyle  font_style;
    float         line_height;  /* px */
    float         letter_spacing; /* px */
    CssTextAlign  text_align;
    int           text_decoration; /* CssTextDecoration flags */
    CssWhiteSpace white_space;

    /* Flexbox */
    CssFlexDirection flex_direction;
    CssFlexWrap      flex_wrap;
    CssAlign         justify_content;
    CssAlign         align_items;
    CssAlign         align_self;
    float            flex_grow;
    float            flex_shrink;
    CssLength        flex_basis;

    /* List */
    int           list_style_none;

    /* Cursor, pointer-events */
    int           cursor_pointer;
} NbStyle;

/* ------------------------------------------------------------------ */
/*  Stylesheet                                                         */
/* ------------------------------------------------------------------ */

/* A single CSS declaration:  property: value */
typedef struct NbDecl {
    char          *property;
    char          *value;
    int            important;
    struct NbDecl *next;
} NbDecl;

/* A CSS rule:  selector { declarations } */
typedef struct NbRule {
    char          *selector;  /* raw selector string */
    NbDecl        *decls;
    struct NbRule *next;
} NbRule;

typedef struct {
    NbRule  *rules;
    NbArena *arena;
} NbStylesheet;

/* ------------------------------------------------------------------ */
/*  API                                                                */
/* ------------------------------------------------------------------ */

/* Parse a CSS string into a stylesheet */
NbStylesheet *nb_css_parse(const char *css, size_t len);
void          nb_css_free(NbStylesheet *ss);

/* Walk the DOM and attach computed styles to every node */
void nb_css_apply(NbStylesheet *ua,   /* user-agent defaults */
                  NbStylesheet *page, /* page stylesheet(s) */
                  NbDocument   *doc);

/* Default browser stylesheet (built-in, never freed) */
NbStylesheet *nb_css_ua_stylesheet(void);

/* Resolve a CssLength to pixels given a reference (parent) size */
float nb_css_resolve_length(CssLength l, float ref, float font_size, float vw, float vh);

/* Color parsing helper */
int nb_css_parse_color(const char *val, CssColor *out);

#endif /* NB_CSS_H */
