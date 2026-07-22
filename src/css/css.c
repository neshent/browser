#include "css.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

/* Windows / MinGW compatibility */
#ifdef _WIN32
#  define strcasecmp  _stricmp
#  define strncasecmp _strnicmp
#endif

/* ------------------------------------------------------------------ */
/*  Color parsing                                                      */
/* ------------------------------------------------------------------ */
typedef struct { const char *name; uint8_t r,g,b; } NamedColor;
static const NamedColor NAMED_COLORS[] = {
    {"black",0,0,0},{"white",255,255,255},{"red",255,0,0},
    {"green",0,128,0},{"lime",0,255,0},{"blue",0,0,255},
    {"yellow",255,255,0},{"cyan",0,255,255},{"magenta",255,0,255},
    {"orange",255,165,0},{"pink",255,192,203},{"purple",128,0,128},
    {"gray",128,128,128},{"grey",128,128,128},{"silver",192,192,192},
    {"maroon",128,0,0},{"navy",0,0,128},{"teal",0,128,128},
    {"olive",128,128,0},{"brown",165,42,42},{"coral",255,127,80},
    {"salmon",250,128,114},{"gold",255,215,0},{"tan",210,180,140},
    {"violet",238,130,238},{"indigo",75,0,130},{"beige",245,245,220},
    {"ivory",255,255,240},{"lavender",230,230,250},{"turquoise",64,224,208},
    {"khaki",240,230,140},{"crimson",220,20,60},{"darkblue",0,0,139},
    {"darkgreen",0,100,0},{"darkred",139,0,0},{"darkgray",169,169,169},
    {"lightblue",173,216,230},{"lightgray",211,211,211},{"lightgreen",144,238,144},
    {"lightyellow",255,255,224},{"transparent",0,0,0},
    {NULL,0,0,0}
};

int nb_css_parse_color(const char *val, CssColor *out) {
    if (!val || !*val) return 0;
    while (isspace((unsigned char)*val)) val++;
    out->a = 255; out->is_transparent = 0;

    if (val[0] == '#') {
        val++;
        size_t l = strlen(val);
        unsigned int rv=0,gv=0,bv=0,av=255;
        if (l==3||l==4) {
            sscanf(val,"%1x%1x%1x",&rv,&gv,&bv);
            rv|=rv<<4; gv|=gv<<4; bv|=bv<<4;
            if (l==4) { unsigned int a; sscanf(val+3,"%1x",&a); av=a|(a<<4); }
        } else if (l==6||l==8) {
            sscanf(val,"%2x%2x%2x",&rv,&gv,&bv);
            if (l==8) sscanf(val+6,"%2x",&av);
        } else return 0;
        out->r=(uint8_t)rv; out->g=(uint8_t)gv; out->b=(uint8_t)bv; out->a=(uint8_t)av;
        return 1;
    }
    if (strncmp(val,"rgb",3)==0) {
        int r,g,b; float a=1.f;
        int has_alpha = (val[3]=='a');
        const char *p = strchr(val,'('); if(!p) return 0; p++;
        if (has_alpha) sscanf(p,"%d,%d,%d,%f",&r,&g,&b,&a);
        else           sscanf(p,"%d,%d,%d",&r,&g,&b);
        out->r=(uint8_t)(r<0?0:r>255?255:r);
        out->g=(uint8_t)(g<0?0:g>255?255:g);
        out->b=(uint8_t)(b<0?0:b>255?255:b);
        out->a=(uint8_t)(int)(a<=0?0:a>=1?255:a*255);
        return 1;
    }
    if (strcmp(val,"transparent")==0) { out->r=out->g=out->b=out->a=0; out->is_transparent=1; return 1; }
    if (strcmp(val,"currentColor")==0||strcmp(val,"inherit")==0) return 0;
    for (int i=0; NAMED_COLORS[i].name; i++) {
        if (strcasecmp(NAMED_COLORS[i].name, val)==0) {
            out->r=NAMED_COLORS[i].r; out->g=NAMED_COLORS[i].g; out->b=NAMED_COLORS[i].b;
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Length parsing                                                     */
/* ------------------------------------------------------------------ */
static CssLength parse_length(const char *val) {
    CssLength l = {0, CSS_UNIT_PX};
    if (!val||!*val) { l.unit=CSS_UNIT_AUTO; return l; }
    if (strcmp(val,"auto")==0)  { l.unit=CSS_UNIT_AUTO; return l; }
    if (strcmp(val,"none")==0)  { l.unit=CSS_UNIT_NONE; return l; }
    if (strcmp(val,"0")==0)     { return l; }
    char *end;
    l.value = strtof(val, &end);
    if      (strncmp(end,"px",2)==0)  l.unit = CSS_UNIT_PX;
    else if (strncmp(end,"em",2)==0)  l.unit = CSS_UNIT_EM;
    else if (strncmp(end,"rem",3)==0) l.unit = CSS_UNIT_REM;
    else if (strncmp(end,"%",1)==0)   l.unit = CSS_UNIT_PCT;
    else if (strncmp(end,"pt",2)==0)  { l.unit = CSS_UNIT_PT; }
    else if (strncmp(end,"vw",2)==0)  l.unit = CSS_UNIT_VW;
    else if (strncmp(end,"vh",2)==0)  l.unit = CSS_UNIT_VH;
    return l;
}

float nb_css_resolve_length(CssLength l, float ref, float font_size, float vw, float vh) {
    switch (l.unit) {
        case CSS_UNIT_PX:   return l.value;
        case CSS_UNIT_EM:   return l.value * font_size;
        case CSS_UNIT_REM:  return l.value * 16.0f;
        case CSS_UNIT_PCT:  return l.value * ref / 100.0f;
        case CSS_UNIT_PT:   return l.value * 1.333333f;
        case CSS_UNIT_VW:   return l.value * vw / 100.0f;
        case CSS_UNIT_VH:   return l.value * vh / 100.0f;
        case CSS_UNIT_AUTO: return -1.0f; /* caller handles */
        default:            return 0.0f;
    }
}

/* ------------------------------------------------------------------ */
/*  CSS Tokenizer / Parser                                             */
/* ------------------------------------------------------------------ */
static void skip_ws(const char **p) { while (**p && isspace((unsigned char)**p)) (*p)++; }
static void skip_comment(const char **p) {
    if ((*p)[0]=='/'&&(*p)[1]=='*') {
        *p += 2;
        while (**p && !((*p)[0]=='*'&&(*p)[1]=='/')) (*p)++;
        if (**p) *p += 2;
    }
}
static void skip_ws_and_comments(const char **p) {
    int changed;
    do { changed=0; skip_ws(p); if ((*p)[0]=='/'&&(*p)[1]=='*') { skip_comment(p); changed=1; } } while(changed);
}

static char *read_until(NbArena *a, const char **p, const char *stop_chars) {
    const char *start = *p;
    int depth = 0;
    while (**p) {
        if (**p == '(') depth++;
        else if (**p == ')') { if(depth>0) depth--; else if(strchr(stop_chars,')')&&depth==0) break; }
        else if (depth==0 && strchr(stop_chars, **p)) break;
        (*p)++;
    }
    return nb_arena_strndup(a, start, *p - start);
}

NbStylesheet *nb_css_parse(const char *css, size_t len) {
    NbStylesheet *ss = calloc(1, sizeof(*ss));
    ss->arena = nb_arena_new(0);
    const char *p = css;
    const char *end = css + len;

    while (p < end) {
        skip_ws_and_comments(&p);
        if (p >= end) break;

        /* @-rules: skip @charset, @import inline; skip @media blocks */
        if (*p == '@') {
            p++;
            /* read keyword */
            char kw[32]; int ki=0;
            while (*p && isalpha((unsigned char)*p) && ki<31) kw[ki++]=*p++;
            kw[ki]='\0';
            if (strcmp(kw,"charset")==0||strcmp(kw,"import")==0||strcmp(kw,"namespace")==0) {
                while (*p && *p!=';' && *p!='\n') p++;
                if (*p==';') p++;
            } else {
                /* block rule — find matching {} */
                while (*p && *p != '{') p++;
                if (*p=='{') {
                    int d=1; p++;
                    while (*p && d>0) { if(*p=='{')d++; else if(*p=='}')d--; p++; }
                }
            }
            continue;
        }

        /* read selector */
        const char *sel_start = p;
        while (p < end && *p != '{') p++;
        if (p >= end) break;

        /* trim selector */
        const char *sel_end = p;
        while (sel_end > sel_start && isspace((unsigned char)*(sel_end-1))) sel_end--;
        char *selector = nb_arena_strndup(ss->arena, sel_start, sel_end - sel_start);
        p++; /* '{' */

        /* parse declarations */
        NbDecl *decls = NULL;
        while (*p && *p != '}') {
            skip_ws_and_comments(&p);
            if (!*p || *p=='}') break;
            /* property name */
            const char *pname = p;
            while (*p && *p!=':' && *p!='}') p++;
            if (*p != ':') { if(*p=='}') break; p++; continue; }
            size_t plen = p - pname;
            while (plen>0 && isspace((unsigned char)pname[plen-1])) plen--;
            p++; /* ':' */
            skip_ws_and_comments(&p);
            /* property value */
            const char *pval = p;
            while (*p && *p!=';' && *p!='}') p++;
            size_t vlen = p - pval;
            while (vlen>0 && isspace((unsigned char)pval[vlen-1])) vlen--;
            if (*p==';') p++;

            if (plen && vlen) {
                NbDecl *d = nb_arena_alloc0(ss->arena, sizeof(NbDecl));
                d->property = nb_arena_strndup(ss->arena, pname, plen);
                char *raw_val = nb_arena_strndup(ss->arena, pval, vlen);
                /* strip !important */
                char *imp = strstr(raw_val, "!important");
                if (imp) { *imp='\0'; d->important=1; }
                d->value = raw_val;
                d->next = decls;
                decls = d;
            }
        }
        if (*p=='}') p++;

        if (selector && decls) {
            /* split comma-separated selectors into individual rules */
            char *sel_copy = strdup(selector);
            char *tok = strtok(sel_copy, ",");
            while (tok) {
                while (*tok && isspace((unsigned char)*tok)) tok++;
                char *te = tok + strlen(tok);
                while (te>tok && isspace((unsigned char)*(te-1))) te--;
                NbRule *rule = nb_arena_alloc0(ss->arena, sizeof(NbRule));
                rule->selector = nb_arena_strndup(ss->arena, tok, te-tok);
                rule->decls    = decls;
                rule->next     = ss->rules;
                ss->rules      = rule;
                tok = strtok(NULL, ",");
            }
            free(sel_copy);
        }
    }
    return ss;
}

void nb_css_free(NbStylesheet *ss) {
    if (!ss) return;
    nb_arena_free(ss->arena);
    free(ss);
}

/* ------------------------------------------------------------------ */
/*  Selector matching                                                  */
/* ------------------------------------------------------------------ */
/* Supports: tag, .class, #id, [attr], tag.class, combinations, > */

static int match_simple(NbNode *node, const char *sel) {
    if (!sel || !*sel) return 0;
    if (strcmp(sel,"*")==0) return (node->type==NB_NODE_ELEMENT);
    if (sel[0]=='#') {
        const char *id = nb_attr_val(node,"id");
        return id && strcmp(id, sel+1)==0;
    }
    if (sel[0]=='.') {
        const char *cls = nb_attr_val(node,"class");
        if (!cls) return 0;
        /* check each space-separated class token */
        const char *needle = sel+1;
        size_t nl = strlen(needle);
        const char *c = cls;
        while (*c) {
            while (*c==' ') c++;
            const char *end = c;
            while (*end && *end!=' ') end++;
            if ((size_t)(end-c)==nl && memcmp(c,needle,nl)==0) return 1;
            c = end;
        }
        return 0;
    }
    if (sel[0]=='[') {
        /* attribute selector [attr] or [attr=val] */
        char name[64]={0}; char val[256]={0};
        const char *eq = strchr(sel+1,'=');
        if (eq) {
            size_t nl = eq-(sel+1); if(nl>63) nl=63;
            memcpy(name,sel+1,nl);
            const char *vs = eq+1;
            size_t vl = strlen(vs); if(vl>0&&vs[vl-1]==']') vl--;
            if(vl>0&&(vs[0]=='"'||vs[0]=='\'')) { vs++; vl-=2; }
            if(vl>255) vl=255;
            memcpy(val,vs,vl);
            const char *av = nb_attr_val(node,name);
            return av && strcmp(av,val)==0;
        } else {
            size_t nl = strlen(sel+1); if(nl>0&&sel[nl]==']') nl--;
            if(nl>63) nl=63;
            memcpy(name,sel+1,nl);
            return nb_attr_val(node,name) != NULL;
        }
    }
    /* tag[.class][#id] compound */
    if (node->type != NB_NODE_ELEMENT) return 0;
    char tag[64]={0}; size_t ti=0;
    const char *s = sel;
    while (*s && *s!='.' && *s!='#' && *s!='[' && *s!=':' && ti<63) tag[ti++]=*s++;
    tag[ti]='\0';
    if (ti && strcmp(tag,"*")!=0 && strcmp(tag,node->tag)!=0) return 0;
    /* remaining compound parts */
    while (*s) {
        if (*s=='.'||*s=='#'||*s=='[') {
            const char *end = s+1;
            if (*s=='[') { while(*end&&*end!=']') end++; if(*end==']') end++; }
            else         { while(*end&&*end!='.'&&*end!='#'&&*end!='[') end++; }
            char part[256]; size_t pl=end-s; if(pl>255) pl=255;
            memcpy(part,s,pl); part[pl]='\0';
            if (!match_simple(node, part)) return 0;
            s = end;
        } else if (*s==':') {
            /* pseudo-class — skip for now, treat as match */
            s++; while(*s&&*s!='.'&&*s!='#'&&*s!='['&&*s!=':') s++;
        } else s++;
    }
    return 1;
}

static int selector_matches(NbNode *node, const char *selector) {
    if (!node || node->type!=NB_NODE_ELEMENT || !selector) return 0;
    /* split by combinator ' ' or '>' right-to-left */
    char *sel = strdup(selector);
    /* walk tokens right to left */
    /* For simplicity: last token must match node, ancestor tokens match ancestors */
    /* Tokenize preserving combinators */
    char *parts[32]; char combinators[32]; int np=0;
    char *p = sel;
    while (*p && np<31) {
        while(*p==' ') p++;
        if (!*p) break;
        char comb = ' ';
        if (*p=='>') { comb='>'; p++; while(*p==' ')p++; }
        else if (*p=='+') { comb='+'; p++; while(*p==' ')p++; }
        else if (*p=='~') { comb='~'; p++; while(*p==' ')p++; }
        if (!*p) break;
        char *start=p;
        /* find end of simple selector */
        int depth2=0;
        while(*p) {
            if(*p=='('||*p=='[') depth2++;
            else if(*p==')'||*p==']') { if(depth2>0) depth2--; }
            else if(depth2==0 && (*p==' '||*p=='>'||*p=='+'||*p=='~')) break;
            p++;
        }
        *p='\0'; /* temporary null */
        parts[np]=start; combinators[np]=comb; np++;
        if (*(p+0)=='\0' && *(p+1)) p++;
        else p++;
    }
    if (!np) { free(sel); return 0; }
    /* match right-most part against node */
    if (!match_simple(node, parts[np-1])) { free(sel); return 0; }
    /* match remaining parts against ancestors */
    NbNode *cur = node->parent;
    for (int i=np-2; i>=0; i--) {
        if (!cur || cur->type==NB_NODE_DOCUMENT) { free(sel); return 0; }
        if (combinators[i+1]=='>') {
            if (!match_simple(cur, parts[i])) { free(sel); return 0; }
            cur = cur->parent;
        } else {
            /* descendant — any ancestor */
            int found=0;
            while (cur && cur->type!=NB_NODE_DOCUMENT) {
                if (match_simple(cur, parts[i])) { found=1; cur=cur->parent; break; }
                cur=cur->parent;
            }
            if (!found) { free(sel); return 0; }
        }
    }
    free(sel);
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Style application                                                  */
/* ------------------------------------------------------------------ */
/* Initial values for computed style */
static NbStyle initial_style(void) {
    NbStyle s = {0};
    s.width=s.height=(CssLength){0,CSS_UNIT_AUTO};
    s.min_width=s.min_height=(CssLength){0,CSS_UNIT_AUTO};
    s.max_width=s.max_height=(CssLength){0,CSS_UNIT_NONE};
    s.color=(CssColor){0,0,0,255,0};
    s.background_color=(CssColor){255,255,255,0,1}; /* transparent */
    s.display=DISPLAY_INLINE;
    s.position=POSITION_STATIC;
    s.float_=FLOAT_NONE;
    s.overflow_x=s.overflow_y=OVERFLOW_VISIBLE;
    s.visibility=VISIBILITY_VISIBLE;
    s.opacity=1.0f; s.z_index=0;
    strcpy(s.font_family,"sans-serif");
    s.font_size=16.0f; s.font_weight=400; s.font_style=FONT_STYLE_NORMAL;
    s.line_height=20.0f; s.text_align=TEXT_ALIGN_LEFT;
    s.text_decoration=TEXT_DECORATION_NONE; s.white_space=WHITE_SPACE_NORMAL;
    s.flex_direction=FLEX_DIR_ROW; s.flex_wrap=FLEX_WRAP_NOWRAP;
    s.justify_content=ALIGN_FLEX_START; s.align_items=ALIGN_STRETCH; s.align_self=ALIGN_AUTO;
    s.flex_grow=0.0f; s.flex_shrink=1.0f;
    s.flex_basis=(CssLength){0,CSS_UNIT_AUTO};
    return s;
}

/* Apply a single declaration to style */
static void apply_decl(NbStyle *s, const char *prop, const char *val) {
    if (!prop||!val) return;
    #define PROP(name) (strcmp(prop,(name))==0)
    if (PROP("width"))  s->width=parse_length(val);
    else if (PROP("height")) s->height=parse_length(val);
    else if (PROP("min-width"))  s->min_width=parse_length(val);
    else if (PROP("min-height")) s->min_height=parse_length(val);
    else if (PROP("max-width"))  s->max_width=parse_length(val);
    else if (PROP("max-height")) s->max_height=parse_length(val);
    else if (PROP("margin"))       { CssLength v=parse_length(val); s->margin_top=s->margin_right=s->margin_bottom=s->margin_left=v; }
    else if (PROP("margin-top"))    s->margin_top=parse_length(val);
    else if (PROP("margin-right"))  s->margin_right=parse_length(val);
    else if (PROP("margin-bottom")) s->margin_bottom=parse_length(val);
    else if (PROP("margin-left"))   s->margin_left=parse_length(val);
    else if (PROP("padding"))       { CssLength v=parse_length(val); s->padding_top=s->padding_right=s->padding_bottom=s->padding_left=v; }
    else if (PROP("padding-top"))    s->padding_top=parse_length(val);
    else if (PROP("padding-right"))  s->padding_right=parse_length(val);
    else if (PROP("padding-bottom")) s->padding_bottom=parse_length(val);
    else if (PROP("padding-left"))   s->padding_left=parse_length(val);
    else if (PROP("color")) nb_css_parse_color(val, &s->color);
    else if (PROP("background-color")||PROP("background")) nb_css_parse_color(val, &s->background_color);
    else if (PROP("display")) {
        if      (strcmp(val,"block")==0)        s->display=DISPLAY_BLOCK;
        else if (strcmp(val,"inline")==0)       s->display=DISPLAY_INLINE;
        else if (strcmp(val,"inline-block")==0) s->display=DISPLAY_INLINE_BLOCK;
        else if (strcmp(val,"flex")==0)         s->display=DISPLAY_FLEX;
        else if (strcmp(val,"none")==0)         s->display=DISPLAY_NONE;
        else if (strcmp(val,"list-item")==0)    s->display=DISPLAY_LIST_ITEM;
        else if (strcmp(val,"table")==0)        s->display=DISPLAY_TABLE;
        else if (strcmp(val,"table-row")==0)    s->display=DISPLAY_TABLE_ROW;
        else if (strcmp(val,"table-cell")==0)   s->display=DISPLAY_TABLE_CELL;
    }
    else if (PROP("position")) {
        if      (strcmp(val,"static")==0)   s->position=POSITION_STATIC;
        else if (strcmp(val,"relative")==0) s->position=POSITION_RELATIVE;
        else if (strcmp(val,"absolute")==0) s->position=POSITION_ABSOLUTE;
        else if (strcmp(val,"fixed")==0)    s->position=POSITION_FIXED;
    }
    else if (PROP("float")) {
        if      (strcmp(val,"left")==0)  s->float_=FLOAT_LEFT;
        else if (strcmp(val,"right")==0) s->float_=FLOAT_RIGHT;
        else if (strcmp(val,"none")==0)  s->float_=FLOAT_NONE;
    }
    else if (PROP("font-size"))   s->font_size=parse_length(val).value;
    else if (PROP("font-weight")) s->font_weight=atoi(val);
    else if (PROP("font-family")) snprintf(s->font_family,sizeof(s->font_family),"%s",val);
    else if (PROP("line-height")) s->line_height=parse_length(val).value;
    else if (PROP("text-align")) {
        if      (strcmp(val,"left")==0)    s->text_align=TEXT_ALIGN_LEFT;
        else if (strcmp(val,"center")==0)  s->text_align=TEXT_ALIGN_CENTER;
        else if (strcmp(val,"right")==0)   s->text_align=TEXT_ALIGN_RIGHT;
        else if (strcmp(val,"justify")==0) s->text_align=TEXT_ALIGN_JUSTIFY;
    }
    else if (PROP("text-decoration")) {
        if (strstr(val,"underline"))     s->text_decoration|=TEXT_DECORATION_UNDERLINE;
        if (strstr(val,"line-through"))  s->text_decoration|=TEXT_DECORATION_LINE_THROUGH;
        if (strcmp(val,"none")==0)       s->text_decoration=TEXT_DECORATION_NONE;
    }
    else if (PROP("flex-direction")) {
        if      (strcmp(val,"row")==0)    s->flex_direction=FLEX_DIR_ROW;
        else if (strcmp(val,"column")==0) s->flex_direction=FLEX_DIR_COLUMN;
    }
    else if (PROP("justify-content")) {
        if      (strcmp(val,"flex-start")==0)  s->justify_content=ALIGN_FLEX_START;
        else if (strcmp(val,"flex-end")==0)    s->justify_content=ALIGN_FLEX_END;
        else if (strcmp(val,"center")==0)      s->justify_content=ALIGN_CENTER;
        else if (strcmp(val,"space-between")==0) s->justify_content=ALIGN_SPACE_BETWEEN;
    }
    else if (PROP("align-items")) {
        if      (strcmp(val,"stretch")==0)     s->align_items=ALIGN_STRETCH;
        else if (strcmp(val,"center")==0)      s->align_items=ALIGN_CENTER;
        else if (strcmp(val,"flex-start")==0)  s->align_items=ALIGN_FLEX_START;
        else if (strcmp(val,"flex-end")==0)    s->align_items=ALIGN_FLEX_END;
    }
    #undef PROP
}

/* Compute specificity of a selector (crude: id > class > tag) */
static int specificity(const char *sel) {
    int s=0;
    for (const char *p=sel; *p; p++) {
        if (*p=='#') s+=100;
        else if (*p=='.'||*p=='[') s+=10;
        else if (isalpha((unsigned char)*p) && (p==sel || *(p-1)==' '||*(p-1)=='>'||*(p-1)=='+')) s+=1;
    }
    return s;
}

/* Walk DOM tree, apply UA + page styles, attach NbStyle to every element */
static void apply_to_node(NbNode *node, NbStylesheet *ua, NbStylesheet *page,
                           NbArena *style_arena, NbStyle *parent_style) {
    if (!node) return;
    if (node->type == NB_NODE_ELEMENT) {
        NbStyle *s = nb_arena_alloc0(style_arena, sizeof(NbStyle));
        *s = initial_style();

        /* Inherit certain properties from parent */
        if (parent_style) {
            s->color        = parent_style->color;
            s->font_size    = parent_style->font_size;
            s->font_weight  = parent_style->font_weight;
            s->font_style   = parent_style->font_style;
            s->line_height  = parent_style->line_height;
            s->text_align   = parent_style->text_align;
            s->white_space  = parent_style->white_space;
            s->visibility   = parent_style->visibility;
            memcpy(s->font_family, parent_style->font_family, sizeof(s->font_family));
        }

        /* Apply matching UA rules by ascending specificity */
        for (NbRule *r = ua ? ua->rules : NULL; r; r = r->next)
            if (selector_matches(node, r->selector))
                for (NbDecl *d=r->decls; d; d=d->next) apply_decl(s, d->property, d->value);

        /* Apply matching page rules */
        /* Two passes: normal then important */
        for (NbRule *r = page ? page->rules : NULL; r; r = r->next)
            if (!selector_matches(node, r->selector)) {}
            else for (NbDecl *d=r->decls; d; d=d->next) if (!d->important) apply_decl(s, d->property, d->value);
        for (NbRule *r = page ? page->rules : NULL; r; r = r->next)
            if (!selector_matches(node, r->selector)) {}
            else for (NbDecl *d=r->decls; d; d=d->next) if (d->important) apply_decl(s, d->property, d->value);

        /* Inline style attribute */
        const char *inline_style = nb_attr_val(node, "style");
        if (inline_style) {
            NbStylesheet *inl = nb_css_parse(inline_style, strlen(inline_style));
            /* wrap in a dummy rule matching "*" and apply */
            for (NbRule *r = inl ? inl->rules : NULL; r; r = r->next)
                for (NbDecl *d=r->decls; d; d=d->next) apply_decl(s, d->property, d->value);
            /* parse bare declarations too */
            NbStylesheet *bare = calloc(1,sizeof(*bare));
            bare->arena = nb_arena_new(0);
            /* wrap inline style in a fake rule: "x { <inline> }" */
            size_t il = strlen(inline_style)+16;
            char *wrapped = malloc(il);
            snprintf(wrapped, il, "x{%s}", inline_style);
            NbStylesheet *tmp = nb_css_parse(wrapped, strlen(wrapped));
            free(wrapped);
            for (NbRule *r=tmp?tmp->rules:NULL; r; r=r->next)
                for (NbDecl *d=r->decls; d; d=d->next) apply_decl(s, d->property, d->value);
            nb_css_free(tmp);
            nb_css_free(inl);
            nb_css_free(bare);
        }

        /* UA defaults for specific tags */
        const char *tag = node->tag;
        if (strcmp(tag,"div")==0||strcmp(tag,"p")==0||strcmp(tag,"h1")==0||
            strcmp(tag,"h2")==0||strcmp(tag,"h3")==0||strcmp(tag,"h4")==0||
            strcmp(tag,"h5")==0||strcmp(tag,"h6")==0||strcmp(tag,"ul")==0||
            strcmp(tag,"ol")==0||strcmp(tag,"li")==0||strcmp(tag,"table")==0||
            strcmp(tag,"header")==0||strcmp(tag,"footer")==0||strcmp(tag,"nav")==0||
            strcmp(tag,"main")==0||strcmp(tag,"section")==0||strcmp(tag,"article")==0||
            strcmp(tag,"aside")==0||strcmp(tag,"form")==0||strcmp(tag,"blockquote")==0||
            strcmp(tag,"pre")==0||strcmp(tag,"figure")==0||strcmp(tag,"figcaption")==0)
        { if(s->display==DISPLAY_INLINE) s->display=DISPLAY_BLOCK; }

        if (strcmp(tag,"head")==0||strcmp(tag,"script")==0||strcmp(tag,"style")==0||
            strcmp(tag,"meta")==0||strcmp(tag,"link")==0||strcmp(tag,"title")==0||
            strcmp(tag,"noscript")==0)
        { s->display=DISPLAY_NONE; }

        if (strcmp(tag,"strong")==0||strcmp(tag,"b")==0) s->font_weight=700;
        if (strcmp(tag,"em")==0||strcmp(tag,"i")==0) s->font_style=FONT_STYLE_ITALIC;
        if (strcmp(tag,"h1")==0) { s->font_size=2.0f*16.0f; s->font_weight=700; s->display=DISPLAY_BLOCK; }
        if (strcmp(tag,"h2")==0) { s->font_size=1.5f*16.0f; s->font_weight=700; s->display=DISPLAY_BLOCK; }
        if (strcmp(tag,"h3")==0) { s->font_size=1.17f*16.0f; s->font_weight=700; s->display=DISPLAY_BLOCK; }
        if (strcmp(tag,"h4")==0||strcmp(tag,"h5")==0||strcmp(tag,"h6")==0) { s->font_weight=700; s->display=DISPLAY_BLOCK; }
        if (strcmp(tag,"a")==0) { s->color=(CssColor){0,0,238,255,0}; s->text_decoration|=TEXT_DECORATION_UNDERLINE; }
        if (strcmp(tag,"code")==0||strcmp(tag,"pre")==0||strcmp(tag,"kbd")==0||strcmp(tag,"samp")==0)
            strcpy(s->font_family,"monospace");
        if (strcmp(tag,"hr")==0) s->display=DISPLAY_BLOCK;
        if (strcmp(tag,"img")==0) { s->display=DISPLAY_INLINE_BLOCK; }
        if (strcmp(tag,"button")==0||strcmp(tag,"input")==0||strcmp(tag,"select")==0||
            strcmp(tag,"textarea")==0) s->display=DISPLAY_INLINE_BLOCK;

        node->computed_style = s;
        /* recurse */
        for (NbNode *c=node->first_child; c; c=c->next_sibling)
            apply_to_node(c, ua, page, style_arena, s);
    } else {
        for (NbNode *c=node->first_child; c; c=c->next_sibling)
            apply_to_node(c, ua, page, style_arena, parent_style);
    }
}

void nb_css_apply(NbStylesheet *ua, NbStylesheet *page, NbDocument *doc) {
    /* reuse doc->arena for style nodes (freed with doc) */
    apply_to_node(doc->root, ua, page, doc->arena, NULL);
}

/* Built-in UA stylesheet — minimal reset/defaults */
NbStylesheet *nb_css_ua_stylesheet(void) {
    static const char UA_CSS[] =
        "body{margin:8px;}"
        "a{cursor:pointer;}"
        "ul,ol{padding-left:40px;}"
        "li{display:list-item;}"
        "table{border-collapse:collapse;}"
        "td,th{padding:4px;}"
        "blockquote{margin:16px 40px;}"
        "pre{white-space:pre;overflow-x:auto;}"
        "code{font-size:0.9em;}"
        "img{max-width:100%;}"
        "hr{border:1px solid #ccc;margin:8px 0;}"
        "input,button,select{font-size:inherit;}"
        ;
    return nb_css_parse(UA_CSS, strlen(UA_CSS));
}
