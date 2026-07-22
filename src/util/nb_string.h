#ifndef NB_STRING_H
#define NB_STRING_H

#include <stddef.h>
#include <stdint.h>

/* Dynamic string / string-view helpers used throughout the browser */

typedef struct {
    char   *data;
    size_t  len;
    size_t  cap;
} NbStr;

NbStr   nb_str_new(void);
NbStr   nb_str_from(const char *s);
NbStr   nb_str_fromn(const char *s, size_t n);
void    nb_str_free(NbStr *s);
void    nb_str_clear(NbStr *s);
int     nb_str_append(NbStr *s, const char *data, size_t n);
int     nb_str_appendc(NbStr *s, char c);
int     nb_str_appends(NbStr *s, const char *cstr);
int     nb_str_appendf(NbStr *s, const char *fmt, ...);
char   *nb_str_cstr(NbStr *s);          /* always null-terminated */
NbStr   nb_str_clone(const NbStr *s);
int     nb_str_eq(const NbStr *a, const char *b);

/* Lightweight string-view (no ownership) */
typedef struct {
    const char *ptr;
    size_t      len;
} NbSv;

#define NB_SV(literal) ((NbSv){(literal), sizeof(literal)-1})
#define NB_SV_FMT      "%.*s"
#define NB_SV_ARG(sv)  (int)(sv).len, (sv).ptr

NbSv    nb_sv_from(const char *s);
NbSv    nb_sv_fromn(const char *s, size_t n);
int     nb_sv_eq(NbSv a, NbSv b);
int     nb_sv_eqc(NbSv a, const char *b);
NbSv    nb_sv_trim(NbSv sv);
int     nb_sv_starts(NbSv sv, const char *prefix);

#endif /* NB_STRING_H */
