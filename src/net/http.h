#ifndef NB_HTTP_H
#define NB_HTTP_H

#include "../util/nb_string.h"

/*
 * Custom HTTP/HTTPS client written from scratch.
 * Uses winsock on Windows, POSIX sockets on Linux.
 * TLS via OpenSSL (if available) or fallback to plain HTTP.
 */

typedef struct {
    int   status_code;    /* e.g. 200, 404 */
    NbStr headers;        /* raw header block */
    NbStr body;           /* response body */
    char *content_type;   /* extracted from headers */
    char *location;       /* redirect location, if any */
    int   error;          /* nonzero if error */
    char  error_msg[256];
} NbHttpResponse;

/* Main API */
NbHttpResponse nb_http_get(const char *url);
NbHttpResponse nb_http_post(const char *url, const char *data, size_t len, const char *content_type);
void           nb_http_response_free(NbHttpResponse *r);

/* URL parsing */
typedef struct {
    char *scheme;   /* http or https */
    char *host;
    int   port;
    char *path;     /* includes query */
} NbUrl;

NbUrl  nb_url_parse(const char *url);
void   nb_url_free(NbUrl *u);
char  *nb_url_resolve(const char *base, const char *relative);  /* returns malloc'd string */

#endif /* NB_HTTP_H */
