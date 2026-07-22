#include "nb_string.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>

#define INIT_CAP 64

NbStr nb_str_new(void) {
    NbStr s = {0};
    s.data = malloc(INIT_CAP);
    if (s.data) { s.data[0] = '\0'; s.cap = INIT_CAP; }
    return s;
}

NbStr nb_str_from(const char *cstr) {
    return nb_str_fromn(cstr, cstr ? strlen(cstr) : 0);
}

NbStr nb_str_fromn(const char *cstr, size_t n) {
    NbStr s = nb_str_new();
    if (cstr && n) nb_str_append(&s, cstr, n);
    return s;
}

void nb_str_free(NbStr *s) {
    if (s && s->data) { free(s->data); s->data = NULL; s->len = s->cap = 0; }
}

void nb_str_clear(NbStr *s) {
    if (s && s->data) { s->data[0] = '\0'; s->len = 0; }
}

static int nb_str_grow(NbStr *s, size_t need) {
    if (need < s->cap) return 1;
    size_t ncap = s->cap ? s->cap * 2 : INIT_CAP;
    while (ncap <= need) ncap *= 2;
    char *nd = realloc(s->data, ncap);
    if (!nd) return 0;
    s->data = nd;
    s->cap  = ncap;
    return 1;
}

int nb_str_append(NbStr *s, const char *data, size_t n) {
    if (!s || !data || !n) return 1;
    if (!nb_str_grow(s, s->len + n + 1)) return 0;
    memcpy(s->data + s->len, data, n);
    s->len += n;
    s->data[s->len] = '\0';
    return 1;
}

int nb_str_appendc(NbStr *s, char c) { return nb_str_append(s, &c, 1); }

int nb_str_appends(NbStr *s, const char *cstr) {
    return cstr ? nb_str_append(s, cstr, strlen(cstr)) : 1;
}

int nb_str_appendf(NbStr *s, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return 0;
    return nb_str_append(s, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf)-1);
}

char *nb_str_cstr(NbStr *s) { return s && s->data ? s->data : ""; }

NbStr nb_str_clone(const NbStr *s) {
    if (!s) return nb_str_new();
    return nb_str_fromn(s->data, s->len);
}

int nb_str_eq(const NbStr *a, const char *b) {
    if (!a || !b) return 0;
    return strcmp(nb_str_cstr((NbStr*)a), b) == 0;
}

/* --- NbSv --- */
NbSv nb_sv_from(const char *s) {
    return s ? (NbSv){s, strlen(s)} : (NbSv){0};
}
NbSv nb_sv_fromn(const char *s, size_t n) { return (NbSv){s, n}; }

int nb_sv_eq(NbSv a, NbSv b) {
    return a.len == b.len && memcmp(a.ptr, b.ptr, a.len) == 0;
}
int nb_sv_eqc(NbSv a, const char *b) {
    return nb_sv_eq(a, nb_sv_from(b));
}

NbSv nb_sv_trim(NbSv sv) {
    while (sv.len && isspace((unsigned char)*sv.ptr)) { sv.ptr++; sv.len--; }
    while (sv.len && isspace((unsigned char)sv.ptr[sv.len-1])) sv.len--;
    return sv;
}

int nb_sv_starts(NbSv sv, const char *prefix) {
    size_t pl = strlen(prefix);
    return sv.len >= pl && memcmp(sv.ptr, prefix, pl) == 0;
}
