#include "html_parser.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

/* Windows / MinGW compatibility */
#ifdef _WIN32
#  define strncasecmp _strnicmp
#  define strcasecmp  _stricmp
#endif

/* ------------------------------------------------------------------ */
/*  Entity table (most common HTML5 named entities)                    */
/* ------------------------------------------------------------------ */
typedef struct { const char *name; unsigned int cp; } NbEntity;
static const NbEntity ENTITIES[] = {
    {"amp",38},{"lt",60},{"gt",62},{"quot",34},{"apos",39},
    {"nbsp",160},{"copy",169},{"reg",174},{"trade",8482},
    {"mdash",8212},{"ndash",8211},{"ldquo",8220},{"rdquo",8221},
    {"lsquo",8216},{"rsquo",8217},{"hellip",8230},{"middot",183},
    {"euro",8364},{"pound",163},{"yen",165},{"cent",162},
    {"laquo",171},{"raquo",187},{"times",215},{"divide",247},
    {"deg",176},{"plusmn",177},{"frac12",189},{"frac14",188},
    {"frac34",190},{"sup2",178},{"sup3",179},
    {NULL,0}
};

/* Encode a codepoint as UTF-8 into buf, return bytes written */
static int cp_to_utf8(unsigned int cp, char *buf) {
    if (cp < 0x80)        { buf[0]=cp; return 1; }
    if (cp < 0x800)       { buf[0]=0xC0|(cp>>6); buf[1]=0x80|(cp&0x3F); return 2; }
    if (cp < 0x10000)     { buf[0]=0xE0|(cp>>12); buf[1]=0x80|((cp>>6)&0x3F); buf[2]=0x80|(cp&0x3F); return 3; }
    buf[0]=0xF0|(cp>>18); buf[1]=0x80|((cp>>12)&0x3F);
    buf[2]=0x80|((cp>>6)&0x3F); buf[3]=0x80|(cp&0x3F); return 4;
}

char *nb_html_decode_entities(const char *s, size_t len, size_t *out_len) {
    char *out = malloc(len * 4 + 1);
    size_t wi = 0, ri = 0;
    while (ri < len) {
        if (s[ri] == '&') {
            size_t start = ri;
            ri++;
            if (ri < len && s[ri] == '#') {
                ri++;
                unsigned int cp = 0;
                if (ri < len && (s[ri]=='x'||s[ri]=='X')) {
                    ri++;
                    while (ri < len && isxdigit((unsigned char)s[ri])) {
                        cp = cp*16 + (isdigit((unsigned char)s[ri]) ? s[ri]-'0' : tolower((unsigned char)s[ri])-'a'+10);
                        ri++;
                    }
                } else {
                    while (ri < len && isdigit((unsigned char)s[ri])) { cp = cp*10 + (s[ri]-'0'); ri++; }
                }
                if (ri < len && s[ri] == ';') ri++;
                wi += cp_to_utf8(cp, out+wi);
            } else {
                char ename[32]; size_t el = 0;
                while (ri < len && isalpha((unsigned char)s[ri]) && el < 31) { ename[el++] = s[ri++]; }
                ename[el] = '\0';
                if (ri < len && s[ri] == ';') ri++;
                unsigned int cp = 0;
                for (int i = 0; ENTITIES[i].name; i++) {
                    if (strcmp(ENTITIES[i].name, ename)==0) { cp = ENTITIES[i].cp; break; }
                }
                if (cp) wi += cp_to_utf8(cp, out+wi);
                else { memcpy(out+wi, s+start, ri-start); wi += ri-start; }
            }
        } else {
            out[wi++] = s[ri++];
        }
    }
    out[wi] = '\0';
    if (out_len) *out_len = wi;
    return out;
}

/* ------------------------------------------------------------------ */
/*  Tokenizer                                                          */
/* ------------------------------------------------------------------ */

/* Void elements (self-closing, never have children) */
static const char *VOID_ELEMENTS[] = {
    "area","base","br","col","embed","hr","img","input",
    "link","meta","param","source","track","wbr", NULL
};
static int is_void(const char *tag) {
    for (int i = 0; VOID_ELEMENTS[i]; i++)
        if (strcmp(VOID_ELEMENTS[i], tag)==0) return 1;
    return 0;
}

/* Raw-text elements (content treated as text, no child tags) */
static const char *RAW_ELEMENTS[] = { "script","style","textarea","title", NULL };
static int is_raw(const char *tag) {
    for (int i = 0; RAW_ELEMENTS[i]; i++)
        if (strcmp(RAW_ELEMENTS[i], tag)==0) return 1;
    return 0;
}

/* Optional-close elements: auto-close previous same/sibling tag */
typedef struct { const char *tag; const char *closes[8]; } AutoClose;
static const AutoClose AUTO_CLOSE[] = {
    {"li",    {"li",NULL}},
    {"dt",    {"dt","dd",NULL}},
    {"dd",    {"dd","dt",NULL}},
    {"p",     {"p",NULL}},
    {"tr",    {"tr",NULL}},
    {"th",    {"th","td",NULL}},
    {"td",    {"td","th",NULL}},
    {"colgroup",{"colgroup",NULL}},
    {"caption",{"caption",NULL}},
    {"option",{"option","optgroup",NULL}},
    {"optgroup",{"optgroup",NULL}},
    {"thead", {"thead","tbody","tfoot",NULL}},
    {"tbody", {"tbody","tfoot",NULL}},
    {"tfoot", {"tfoot",NULL}},
    {NULL,    {NULL}}
};

/* ------------------------------------------------------------------ */
/*  Parser state                                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    const char  *html;
    size_t       len;
    size_t       pos;
    NbDocument  *doc;
    /* open element stack */
    NbNode      *stack[256];
    int          stack_top;
} Parser;

static char peek(Parser *p) { return p->pos < p->len ? p->html[p->pos] : '\0'; }
static char consume(Parser *p) { return p->pos < p->len ? p->html[p->pos++] : '\0'; }
static void skip_whitespace(Parser *p) { while (p->pos < p->len && isspace((unsigned char)p->html[p->pos])) p->pos++; }

static NbNode *current(Parser *p) {
    return p->stack_top >= 0 ? p->stack[p->stack_top] : p->doc->root;
}

static void push(Parser *p, NbNode *n) {
    if (p->stack_top < 255) p->stack[++p->stack_top] = n;
}

static void pop(Parser *p) { if (p->stack_top >= 0) p->stack_top--; }

/* Pop until we've closed tag (inclusive) */
static void pop_to(Parser *p, const char *tag) {
    for (int i = p->stack_top; i >= 0; i--) {
        if (p->stack[i]->type == NODE_ELEMENT && strcmp(p->stack[i]->tag, tag)==0) {
            p->stack_top = i - 1;
            return;
        }
    }
}

/* Check if tag is open anywhere in stack */
static int is_open(Parser *p, const char *tag) {
    for (int i = p->stack_top; i >= 0; i--)
        if (p->stack[i]->type == NODE_ELEMENT && strcmp(p->stack[i]->tag, tag)==0) return 1;
    return 0;
}

/* Auto-close rules before inserting new open tag */
static void maybe_auto_close(Parser *p, const char *new_tag) {
    for (int i = 0; AUTO_CLOSE[i].tag; i++) {
        if (strcmp(AUTO_CLOSE[i].tag, new_tag)==0) {
            for (int j = 0; AUTO_CLOSE[i].closes[j]; j++) {
                const char *ct = AUTO_CLOSE[i].closes[j];
                if (p->stack_top >= 0 &&
                    strcmp(p->stack[p->stack_top]->tag, ct)==0) {
                    pop(p);
                    break;
                }
            }
            break;
        }
    }
}

/* ---- attribute parsing ---- */
static void parse_attrs(Parser *p, NbNode *node) {
    while (1) {
        skip_whitespace(p);
        char c = peek(p);
        if (!c || c=='>' || (c=='/' && p->html[p->pos+1]=='>')) break;

        /* attribute name */
        char name[256]; size_t ni = 0;
        while (p->pos < p->len) {
            c = peek(p);
            if (isspace((unsigned char)c)||c=='='||c=='>'||c=='/') break;
            name[ni++] = tolower((unsigned char)consume(p));
            if (ni >= 255) break;
        }
        name[ni] = '\0';
        if (!ni) { consume(p); continue; }

        skip_whitespace(p);
        if (peek(p) != '=') {
            /* boolean attribute */
            nb_attr_set(p->doc, node, name, name);
            continue;
        }
        consume(p); /* '=' */
        skip_whitespace(p);

        /* attribute value */
        char val[4096]; size_t vi = 0;
        c = peek(p);
        if (c=='"'||c=='\'') {
            char quote = consume(p);
            while (p->pos < p->len && peek(p) != quote && vi < 4095)
                val[vi++] = consume(p);
            if (peek(p)==quote) consume(p);
        } else {
            while (p->pos < p->len) {
                c = peek(p);
                if (isspace((unsigned char)c)||c=='>'||c=='/') break;
                val[vi++] = consume(p);
                if (vi >= 4095) break;
            }
        }
        val[vi] = '\0';
        /* decode entities in value */
        size_t decoded_len;
        char *decoded = nb_html_decode_entities(val, vi, &decoded_len);
        nb_attr_set(p->doc, node, name, decoded);
        free(decoded);
    }
}

/* ---- raw text element reader ---- */
static void parse_raw_text(Parser *p, NbNode *parent, const char *tag) {
    size_t start = p->pos;
    char close[64];
    snprintf(close, sizeof(close), "</%s", tag);
    size_t cl = strlen(close);
    while (p->pos < p->len) {
        if (p->pos + cl <= p->len &&
            strncasecmp(p->html + p->pos, close, cl)==0) {
            /* consume raw text node */
            if (p->pos > start) {
                size_t decoded_len;
                char *decoded = nb_html_decode_entities(p->html+start, p->pos-start, &decoded_len);
                NbNode *tn = nb_node_text(p->doc, decoded, decoded_len);
                free(decoded);
                nb_node_append_child(parent, tn);
            }
            /* skip closing tag */
            while (p->pos < p->len && p->html[p->pos] != '>') p->pos++;
            if (p->pos < p->len) p->pos++;
            return;
        }
        p->pos++;
    }
    /* EOF inside raw element — emit what we have */
    if (p->pos > start) {
        size_t decoded_len;
        char *decoded = nb_html_decode_entities(p->html+start, p->pos-start, &decoded_len);
        NbNode *tn = nb_node_text(p->doc, decoded, decoded_len);
        free(decoded);
        nb_node_append_child(parent, tn);
    }
}

/* ---- main parsing loop ---- */
static void parse_tokens(Parser *p) {
    while (p->pos < p->len) {
        if (peek(p) != '<') {
            /* text node */
            size_t start = p->pos;
            while (p->pos < p->len && peek(p) != '<') p->pos++;
            size_t decoded_len;
            char *decoded = nb_html_decode_entities(p->html+start, p->pos-start, &decoded_len);
            /* skip pure-whitespace text nodes between block elements */
            int all_ws = 1;
            for (size_t i = 0; i < decoded_len; i++)
                if (!isspace((unsigned char)decoded[i])) { all_ws = 0; break; }
            if (!all_ws) {
                NbNode *tn = nb_node_text(p->doc, decoded, decoded_len);
                nb_node_append_child(current(p), tn);
            }
            free(decoded);
            continue;
        }

        consume(p); /* '<' */
        if (peek(p) == '!') {
            consume(p);
            if (p->pos+1 < p->len && p->html[p->pos]=='-' && p->html[p->pos+1]=='-') {
                p->pos += 2;
                /* comment — skip to --> */
                while (p->pos+2 < p->len &&
                       !(p->html[p->pos]=='-'&&p->html[p->pos+1]=='-'&&p->html[p->pos+2]=='>'))
                    p->pos++;
                p->pos += 3;
            } else if (p->pos+6 < p->len && strncasecmp(p->html+p->pos,"DOCTYPE",7)==0) {
                while (p->pos < p->len && p->html[p->pos] != '>') p->pos++;
                if (p->pos < p->len) p->pos++;
            } else {
                while (p->pos < p->len && p->html[p->pos] != '>') p->pos++;
                if (p->pos < p->len) p->pos++;
            }
            continue;
        }

        if (peek(p) == '?') {
            /* processing instruction — skip */
            while (p->pos < p->len && peek(p) != '>') p->pos++;
            if (p->pos < p->len) p->pos++;
            continue;
        }

        int is_close = (peek(p) == '/');
        if (is_close) consume(p);

        /* tag name */
        char tag[128]; size_t ti = 0;
        while (p->pos < p->len && !isspace((unsigned char)peek(p)) &&
               peek(p) != '>' && peek(p) != '/' && ti < 127)
            tag[ti++] = tolower((unsigned char)consume(p));
        tag[ti] = '\0';
        if (!ti) {
            /* skip malformed '<' */
            while (p->pos < p->len && peek(p) != '>') p->pos++;
            if (p->pos < p->len) p->pos++;
            continue;
        }

        if (is_close) {
            /* closing tag */
            while (p->pos < p->len && peek(p) != '>') p->pos++;
            if (p->pos < p->len) p->pos++;
            if (is_open(p, tag)) pop_to(p, tag);
            continue;
        }

        /* opening tag */
        maybe_auto_close(p, tag);
        NbNode *elem = nb_node_element(p->doc, tag);
        parse_attrs(p, elem);

        int self_close = (peek(p) == '/');
        if (self_close) consume(p);
        if (peek(p) == '>') consume(p);

        nb_node_append_child(current(p), elem);

        if (!is_void(tag) && !self_close) {
            if (is_raw(tag)) {
                parse_raw_text(p, elem, tag);
            } else {
                push(p, elem);
            }
        }
    }
}

NbDocument *nb_html_parse(const char *html, size_t len) {
    NbDocument *doc = nb_doc_new();
    Parser p = {0};
    p.html = html;
    p.len  = len;
    p.doc  = doc;
    p.stack_top = -1;
    parse_tokens(&p);
    return doc;
}
