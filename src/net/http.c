#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Windows / MinGW compatibility */
#ifdef _WIN32
#  define strcasecmp  _stricmp
#  define strncasecmp _strnicmp
#endif

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef SOCKET sock_t;
  #define SOCK_INVALID INVALID_SOCKET
  #define sock_close closesocket
  static void sock_init(void) { WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa); }
#else
  #include <unistd.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  typedef int sock_t;
  #define SOCK_INVALID (-1)
  #define sock_close close
  static void sock_init(void) {}
#endif

/* Optional OpenSSL support for HTTPS */
#ifdef NB_USE_OPENSSL
  #include <openssl/ssl.h>
  #include <openssl/err.h>
  static SSL_CTX *g_ssl_ctx = NULL;
  static void ssl_init(void) {
      if (g_ssl_ctx) return;
      SSL_load_error_strings();
      SSL_library_init();
      g_ssl_ctx = SSL_CTX_new(TLS_client_method());
  }
#else
  typedef void SSL;
  static void ssl_init(void) {}
#endif

/* --- URL Parsing --- */
NbUrl nb_url_parse(const char *url) {
    NbUrl u = {0};
    if (!url) return u;
    
    const char *p = url;
    const char *schend = strstr(p, "://");
    if (schend) {
        u.scheme = strndup(p, schend - p);
        p = schend + 3;
    } else {
        u.scheme = strdup("http");
    }
    
    const char *hostend = p;
    while (*hostend && *hostend != '/' && *hostend != ':') hostend++;
    
    if (*hostend == ':') {
        u.host = strndup(p, hostend - p);
        hostend++;
        u.port = atoi(hostend);
        while (*hostend && *hostend != '/') hostend++;
    } else {
        u.host = strndup(p, hostend - p);
        u.port = strcmp(u.scheme, "https") == 0 ? 443 : 80;
    }
    
    if (*hostend) u.path = strdup(hostend);
    else          u.path = strdup("/");
    
    return u;
}

void nb_url_free(NbUrl *u) {
    if (!u) return;
    free(u->scheme); free(u->host); free(u->path);
    memset(u, 0, sizeof(*u));
}

char *nb_url_resolve(const char *base, const char *rel) {
    /* Simplified resolver — absolute wins, else append to base */
    if (!rel || !*rel) return base ? strdup(base) : NULL;
    if (strstr(rel, "://")) return strdup(rel);
    if (rel[0] == '/') {
        NbUrl bu = nb_url_parse(base);
        char *result = malloc(strlen(bu.scheme) + strlen(bu.host) + strlen(rel) + 20);
        sprintf(result, "%s://%s:%d%s", bu.scheme, bu.host, bu.port, rel);
        nb_url_free(&bu);
        return result;
    }
    /* relative path — simple join (not fully spec-compliant) */
    char *result = malloc(strlen(base) + strlen(rel) + 2);
    sprintf(result, "%s%s", base, rel);
    return result;
}

/* --- Socket Helpers --- */
static sock_t connect_tcp(const char *host, int port) {
    sock_init();
    
    struct addrinfo hints = {0}, *res;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    
    char portstr[16];
    sprintf(portstr, "%d", port);
    
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return SOCK_INVALID;
    
    sock_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == SOCK_INVALID) { freeaddrinfo(res); return SOCK_INVALID; }
    
    if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
        sock_close(s);
        freeaddrinfo(res);
        return SOCK_INVALID;
    }
    
    freeaddrinfo(res);
    return s;
}

static int sock_send(sock_t s, SSL *ssl, const char *data, size_t len) {
#ifdef NB_USE_OPENSSL
    if (ssl) return SSL_write(ssl, data, (int)len) > 0;
#endif
    return send(s, data, (int)len, 0) > 0;
}

static int sock_recv(sock_t s, SSL *ssl, char *buf, size_t sz) {
#ifdef NB_USE_OPENSSL
    if (ssl) return SSL_read(ssl, buf, (int)sz);
#endif
    return recv(s, buf, (int)sz, 0);
}

/* --- HTTP Request/Response --- */
static NbHttpResponse do_request(const char *method, const char *url, const char *body, size_t bodylen, const char *ct) {
    NbHttpResponse res = {0};
    res.headers = nb_str_new();
    res.body    = nb_str_new();
    
    NbUrl u = nb_url_parse(url);
    if (!u.host) {
        res.error = 1;
        snprintf(res.error_msg, sizeof(res.error_msg), "Invalid URL");
        nb_url_free(&u);
        return res;
    }
    
    int use_ssl = (strcmp(u.scheme, "https") == 0);
    if (use_ssl) ssl_init();
    
    sock_t s = connect_tcp(u.host, u.port);
    if (s == SOCK_INVALID) {
        res.error = 1;
        snprintf(res.error_msg, sizeof(res.error_msg), "Connection failed");
        nb_url_free(&u);
        return res;
    }
    
    SSL *ssl = NULL;
#ifdef NB_USE_OPENSSL
    if (use_ssl && g_ssl_ctx) {
        ssl = SSL_new(g_ssl_ctx);
        SSL_set_fd(ssl, (int)s);
        if (SSL_connect(ssl) != 1) {
            SSL_free(ssl);
            sock_close(s);
            res.error = 1;
            snprintf(res.error_msg, sizeof(res.error_msg), "TLS handshake failed");
            nb_url_free(&u);
            return res;
        }
    }
#endif
    
    /* Build HTTP request */
    NbStr req = nb_str_new();
    nb_str_appendf(&req, "%s %s HTTP/1.1\r\n", method, u.path);
    nb_str_appendf(&req, "Host: %s\r\n", u.host);
    nb_str_appends(&req, "User-Agent: NishantBrowser/1.0\r\n");
    nb_str_appends(&req, "Connection: close\r\n");
    if (body && bodylen && ct) {
        nb_str_appendf(&req, "Content-Type: %s\r\n", ct);
        nb_str_appendf(&req, "Content-Length: %zu\r\n", bodylen);
    }
    nb_str_appends(&req, "\r\n");
    if (body && bodylen) nb_str_append(&req, body, bodylen);
    
    sock_send(s, ssl, req.data, req.len);
    nb_str_free(&req);
    
    /* Read response */
    char buf[4096];
    int header_done = 0;
    while (1) {
        int n = sock_recv(s, ssl, buf, sizeof(buf));
        if (n <= 0) break;
        
        if (!header_done) {
            const char *split = "\r\n\r\n";
            for (int i = 0; i < n - 3; i++) {
                if (memcmp(buf + i, split, 4) == 0) {
                    nb_str_append(&res.headers, buf, i);
                    nb_str_append(&res.body, buf + i + 4, n - i - 4);
                    header_done = 1;
                    break;
                }
            }
            if (!header_done) nb_str_append(&res.headers, buf, n);
        } else {
            nb_str_append(&res.body, buf, n);
        }
    }
    
    /* Parse status code */
    if (res.headers.len > 12 && memcmp(res.headers.data, "HTTP/", 5) == 0) {
        sscanf(res.headers.data + 9, "%d", &res.status_code);
    }
    
    /* Extract Content-Type */
    char *cth = strstr(res.headers.data, "Content-Type:");
    if (cth) {
        cth += 13;
        while (*cth == ' ') cth++;
        char *end = strchr(cth, '\r');
        if (end) res.content_type = strndup(cth, end - cth);
        else     res.content_type = strdup(cth);
    }
    
    /* Extract Location for redirects */
    char *loc = strstr(res.headers.data, "Location:");
    if (loc) {
        loc += 9;
        while (*loc == ' ') loc++;
        char *end = strchr(loc, '\r');
        if (end) res.location = strndup(loc, end - loc);
        else     res.location = strdup(loc);
    }
    
#ifdef NB_USE_OPENSSL
    if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
#endif
    sock_close(s);
    nb_url_free(&u);
    return res;
}

NbHttpResponse nb_http_get(const char *url) {
    return do_request("GET", url, NULL, 0, NULL);
}

NbHttpResponse nb_http_post(const char *url, const char *data, size_t len, const char *ct) {
    return do_request("POST", url, data, len, ct);
}

void nb_http_response_free(NbHttpResponse *r) {
    if (!r) return;
    nb_str_free(&r->headers);
    nb_str_free(&r->body);
    free(r->content_type);
    free(r->location);
    memset(r, 0, sizeof(*r));
}
