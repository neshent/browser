/*
 * http.c — Custom HTTP/1.1 + HTTPS client
 * Uses OpenSSL for TLS (always on), Winsock2 on Windows, POSIX sockets on Linux.
 */
#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  define strcasecmp  _stricmp
#  define strncasecmp _strnicmp
/* strcasestr missing on Windows — provide our own */
static char *strcasestr(const char *hay, const char *needle) {
    if (!needle || !*needle) return (char*)hay;
    size_t nl = strlen(needle);
    for (; *hay; hay++)
        if (_strnicmp(hay, needle, nl) == 0) return (char*)hay;
    return NULL;
}
typedef SOCKET sock_t;
#  define SOCK_INVALID INVALID_SOCKET
#  define sock_close   closesocket
static int g_wsa_init = 0;
static void sock_init(void) {
    if (!g_wsa_init) { WSADATA w; WSAStartup(MAKEWORD(2,2), &w); g_wsa_init = 1; }
}
#else
#  include <unistd.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
typedef int sock_t;
#  define SOCK_INVALID (-1)
#  define sock_close   close
static void sock_init(void) {}
#endif

/* Always use OpenSSL */
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>

static SSL_CTX *g_ssl_ctx = NULL;

static void ssl_global_init(void) {
    if (g_ssl_ctx) return;
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
    g_ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!g_ssl_ctx) return;
    /* Don't verify cert — avoids CA bundle issues on Windows */
    SSL_CTX_set_verify(g_ssl_ctx, SSL_VERIFY_NONE, NULL);
    SSL_CTX_set_options(g_ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
}

/* ------------------------------------------------------------------ */
/*  URL parsing                                                        */
/* ------------------------------------------------------------------ */
NbUrl nb_url_parse(const char *url) {
    NbUrl u = {0};
    if (!url || !*url) return u;

    const char *p = url;
    /* scheme */
    const char *sc = strstr(p, "://");
    if (sc) { u.scheme = strndup(p, sc - p); p = sc + 3; }
    else     { u.scheme = strdup("http"); }

    /* strip user:pass@ */
    const char *at = strchr(p, '@');
    const char *sl = strchr(p, '/');
    if (at && (!sl || at < sl)) p = at + 1;

    /* host[:port] */
    const char *he = p;
    while (*he && *he != '/' && *he != '?' && *he != '#' && *he != ':') he++;
    u.host = strndup(p, he - p);

    if (*he == ':') {
        he++;
        u.port = atoi(he);
        while (*he && *he != '/' && *he != '?' && *he != '#') he++;
    } else {
        u.port = strcasecmp(u.scheme, "https") == 0 ? 443 : 80;
    }

    u.path = (*he) ? strdup(he) : strdup("/");
    return u;
}

void nb_url_free(NbUrl *u) {
    if (!u) return;
    free(u->scheme); free(u->host); free(u->path);
    memset(u, 0, sizeof(*u));
}

char *nb_url_resolve(const char *base, const char *rel) {
    if (!rel || !*rel)  return base ? strdup(base) : strdup("");
    if (strstr(rel, "://")) return strdup(rel);
    if (strncmp(rel,"//",2)==0) {
        /* protocol-relative */
        NbUrl bu = nb_url_parse(base);
        size_t n = strlen(bu.scheme)+2+strlen(rel)+4;
        char *r = malloc(n); sprintf(r, "%s:%s", bu.scheme, rel);
        nb_url_free(&bu); return r;
    }
    if (rel[0] == '/') {
        NbUrl bu = nb_url_parse(base);
        size_t n = strlen(bu.scheme)+3+strlen(bu.host)+10+strlen(rel)+4;
        char *r = malloc(n);
        if (bu.port==80||bu.port==443)
            sprintf(r, "%s://%s%s", bu.scheme, bu.host, rel);
        else
            sprintf(r, "%s://%s:%d%s", bu.scheme, bu.host, bu.port, rel);
        nb_url_free(&bu); return r;
    }
    /* relative — strip last path component from base */
    const char *sc = strstr(base, "://");
    const char *path_start = sc ? sc + 3 : base;
    const char *last_slash = strrchr(path_start, '/');
    if (last_slash) {
        size_t prefix_len = last_slash - base + 1;
        char *r = malloc(prefix_len + strlen(rel) + 2);
        memcpy(r, base, prefix_len);
        strcpy(r + prefix_len, rel);
        return r;
    }
    size_t n = strlen(base)+strlen(rel)+2;
    char *r = malloc(n); sprintf(r, "%s/%s", base, rel);
    return r;
}

/* ------------------------------------------------------------------ */
/*  Socket helpers                                                     */
/* ------------------------------------------------------------------ */
static sock_t connect_tcp(const char *host, int port) {
    sock_init();
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char ps[16]; sprintf(ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return SOCK_INVALID;
    sock_t s = SOCK_INVALID;
    for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
        s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (s == SOCK_INVALID) continue;
        /* 10-second timeout */
#ifdef _WIN32
        DWORD tv = 10000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#else
        struct timeval tv = {10, 0};
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
        if (connect(s, rp->ai_addr, (int)rp->ai_addrlen) == 0) break;
        sock_close(s); s = SOCK_INVALID;
    }
    freeaddrinfo(res);
    return s;
}

/* ------------------------------------------------------------------ */
/*  Chunked transfer-encoding decoder                                  */
/* ------------------------------------------------------------------ */
static NbStr decode_chunked(const char *data, size_t len) {
    NbStr out = nb_str_new();
    const char *p = data, *end = data + len;
    while (p < end) {
        /* read chunk size (hex) */
        char *nl = memchr(p, '\n', end - p);
        if (!nl) break;
        size_t chunk_size = (size_t)strtoul(p, NULL, 16);
        p = nl + 1;
        if (chunk_size == 0) break;
        if (p + chunk_size > end) chunk_size = end - p;
        nb_str_append(&out, p, chunk_size);
        p += chunk_size;
        if (p < end && *p == '\r') p++;
        if (p < end && *p == '\n') p++;
    }
    return out;
}

/* ------------------------------------------------------------------ */
/*  Core request                                                       */
/* ------------------------------------------------------------------ */
static NbHttpResponse do_request(const char *method, const char *url,
                                  const char *body, size_t bodylen,
                                  const char *ct) {
    NbHttpResponse res = {0};
    res.headers = nb_str_new();
    res.body    = nb_str_new();

    ssl_global_init();

    NbUrl u = nb_url_parse(url);
    if (!u.host || !u.host[0]) {
        res.error = 1;
        snprintf(res.error_msg, sizeof(res.error_msg), "Invalid URL: %s", url ? url : "(null)");
        nb_url_free(&u); return res;
    }

    int use_ssl = (strcasecmp(u.scheme, "https") == 0);

    sock_t s = connect_tcp(u.host, u.port);
    if (s == SOCK_INVALID) {
        res.error = 1;
        snprintf(res.error_msg, sizeof(res.error_msg),
                 "TCP connect failed to %s:%d", u.host, u.port);
        nb_url_free(&u); return res;
    }

    SSL *ssl = NULL;
    if (use_ssl) {
        if (!g_ssl_ctx) {
            res.error = 1;
            snprintf(res.error_msg, sizeof(res.error_msg), "OpenSSL init failed");
            sock_close(s); nb_url_free(&u); return res;
        }
        ssl = SSL_new(g_ssl_ctx);
        SSL_set_fd(ssl, (int)s);
        /* SNI */
        SSL_set_tlsext_host_name(ssl, u.host);
        if (SSL_connect(ssl) != 1) {
            unsigned long e = ERR_get_error();
            char ebuf[256] = {0}; ERR_error_string_n(e, ebuf, sizeof(ebuf));
            res.error = 1;
            snprintf(res.error_msg, sizeof(res.error_msg), "TLS failed: %s", ebuf);
            SSL_free(ssl); sock_close(s); nb_url_free(&u); return res;
        }
    }

    /* Build request */
    NbStr req = nb_str_new();
    nb_str_appendf(&req, "%s %s HTTP/1.1\r\n", method, u.path);
    if (u.port == 80 || u.port == 443)
        nb_str_appendf(&req, "Host: %s\r\n", u.host);
    else
        nb_str_appendf(&req, "Host: %s:%d\r\n", u.host, u.port);
    nb_str_appends(&req,
        "User-Agent: Mozilla/5.0 NishantBrowser/1.0\r\n"
        "Accept: text/html,application/xhtml+xml,*/*;q=0.9\r\n"
        "Accept-Language: en-US,en;q=0.9\r\n"
        "Accept-Encoding: identity\r\n"   /* no gzip — keep it simple */
        "Connection: close\r\n");
    if (body && bodylen && ct) {
        nb_str_appendf(&req, "Content-Type: %s\r\n", ct);
        nb_str_appendf(&req, "Content-Length: %zu\r\n", bodylen);
    }
    nb_str_appends(&req, "\r\n");
    if (body && bodylen) nb_str_append(&req, body, bodylen);

    /* Send */
    size_t sent = 0;
    while (sent < req.len) {
        int n;
        if (ssl) n = SSL_write(ssl, req.data + sent, (int)(req.len - sent));
        else     n = send(s, req.data + sent, (int)(req.len - sent), 0);
        if (n <= 0) break;
        sent += n;
    }
    nb_str_free(&req);

    /* Receive all */
    NbStr raw = nb_str_new();
    char buf[8192];
    while (1) {
        int n;
        if (ssl) n = SSL_read(ssl, buf, sizeof(buf));
        else     n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        nb_str_append(&raw, buf, n);
    }

    if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
    sock_close(s);
    nb_url_free(&u);

    if (!raw.len) {
        res.error = 1;
        snprintf(res.error_msg, sizeof(res.error_msg), "No response received");
        nb_str_free(&raw); return res;
    }

    /* Split headers / body */
    const char *hdr_end = NULL;
    for (size_t i = 0; i + 3 < raw.len; i++) {
        if (memcmp(raw.data + i, "\r\n\r\n", 4) == 0) {
            hdr_end = raw.data + i; break;
        }
    }
    if (!hdr_end) {
        /* No header boundary — treat everything as body */
        nb_str_append(&res.body, raw.data, raw.len);
        nb_str_free(&raw); return res;
    }

    size_t hdr_len  = hdr_end - raw.data;
    size_t body_off = hdr_len + 4;
    nb_str_append(&res.headers, raw.data, hdr_len);

    /* Status code */
    if (hdr_len > 12 && memcmp(raw.data, "HTTP/", 5) == 0)
        sscanf(raw.data + 9, "%d", &res.status_code);

    /* Check chunked */
    int chunked = 0;
    char *te = strcasestr(res.headers.data, "Transfer-Encoding:");
    if (te) {
        char *v = te + 18; while (*v == ' ') v++;
        if (strncasecmp(v, "chunked", 7) == 0) chunked = 1;
    }

    size_t body_len = raw.len - body_off;
    if (chunked) {
        NbStr decoded = decode_chunked(raw.data + body_off, body_len);
        nb_str_free(&res.body);
        res.body = decoded;
    } else {
        nb_str_append(&res.body, raw.data + body_off, body_len);
    }

    nb_str_free(&raw);

    /* Extract Content-Type */
    char *cth = strcasestr(res.headers.data, "Content-Type:");
    if (cth) {
        cth += 13; while (*cth == ' ') cth++;
        char *e = cth; while (*e && *e != '\r' && *e != '\n') e++;
        res.content_type = strndup(cth, e - cth);
    }

    /* Extract Location */
    char *loc = strcasestr(res.headers.data, "\nLocation:");
    if (loc) {
        loc += 10; while (*loc == ' ') loc++;
        char *e = loc; while (*e && *e != '\r' && *e != '\n') e++;
        res.location = strndup(loc, e - loc);
    }

    return res;
}

NbHttpResponse nb_http_get(const char *url)  { return do_request("GET",  url, NULL, 0, NULL); }
NbHttpResponse nb_http_post(const char *url, const char *d, size_t l, const char *ct)
                                              { return do_request("POST", url, d,    l, ct);   }

void nb_http_response_free(NbHttpResponse *r) {
    if (!r) return;
    nb_str_free(&r->headers);
    nb_str_free(&r->body);
    free(r->content_type); r->content_type = NULL;
    free(r->location);     r->location     = NULL;
    memset(r, 0, sizeof(*r));
}
