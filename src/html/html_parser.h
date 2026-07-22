#ifndef NB_HTML_PARSER_H
#define NB_HTML_PARSER_H

#include "dom.h"

/*
 * HTML5-inspired tokenizer + tree-builder.
 * Not fully spec-compliant, but handles real-world HTML including:
 *  - Self-closing void elements
 *  - Attribute parsing (quoted/unquoted/boolean)
 *  - Script/style raw text elements
 *  - HTML entities (&amp; &lt; &#nnnn; etc.)
 *  - Nested optional-close elements (p, li, td, tr, etc.)
 *  - DOCTYPE
 *  - Comments
 */

NbDocument *nb_html_parse(const char *html, size_t len);

/* Decode HTML entities in-place, returns new string (caller owns) */
char *nb_html_decode_entities(const char *s, size_t len, size_t *out_len);

#endif /* NB_HTML_PARSER_H */
