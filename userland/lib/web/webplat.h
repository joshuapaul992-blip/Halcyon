/* libweb's platform layer: Halcyon (hal.h) or, with -DWEB_HOST, Linux.
 * Everything that differs between the two is behind the wp_* functions
 * (plat.c); results are Halcyon's E_* codes on both. Internal to libweb. */
#ifndef WEBPLAT_H
#define WEBPLAT_H
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#ifdef WEB_HOST
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define E_IO      -1
#define E_NOMEM   -2
#define E_NOENT   -3
#define E_INVAL   -8
#define E_TIMEOUT -9
#define E_NOTSUP  -11
#define E_AGAIN   -12
#define E_CANCELED -18
#define E_NOTCONN     -21
#define E_CONNREFUSED -23
#define E_CONNRESET   -24
#define E_PIPE        -25
#define E_AFNOSUPPORT -26
#define E_INPROGRESS  -29
#define E_HOSTUNREACH -30
#define E_NETUNREACH  -31
const char *hal_strerror(int err);
#else
#include "hal.h"
#endif

#include "web.h"

/* poll events */
#define WP_IN  1
#define WP_OUT 4
#define WP_ERR 8
#define WP_HUP 16

uint64_t wp_ms(void);
void wp_sleep(int ms);
int  wp_random(void *buf, size_t n);                    /* 0 or error */
/* IPv4 addresses (network byte order) of host; count, or WEB_E_DNS / WEB_E_NONET / WEB_E_TIMEOUT */
int  wp_resolve(const char *host, uint32_t *ip, int max, int timeout_ms, char *why, size_t whyn);
int  wp_tcp_open(void);                                  /* non-blocking TCP socket, or error */
int  wp_tcp_connect(int fd, uint32_t ip, int port);      /* 0, E_INPROGRESS or error */
int  wp_poll(int fd, int events, int ms);                /* WP_* that happened, 0 on timeout, or error */
int  wp_so_error(int fd);                                /* pending error (E_*) or 0 */
long wp_send(int fd, const void *b, size_t n);           /* bytes, E_AGAIN or error */
long wp_recv(int fd, void *b, size_t n);                 /* bytes (0 = end), E_AGAIN or error */
void wp_close(int fd);
/* the time for certificate checks: seconds since 1970 (UTC) and how far
 * off it may be because the clock's time zone is not known (0 = exact) */
void wp_utc_now(long long *unix_s, long *uncertain_s);
/* a detached thread running fn(arg); 0 or error */
int  wp_thread(int (*fn)(void *), void *arg);
typedef struct { volatile int v; } wp_mutex_t;           /* spin + sleep lock; zero = unlocked */
void wp_lock(wp_mutex_t *m);
void wp_unlock(wp_mutex_t *m);

/* small string helpers (Halcyon's C library is a subset) */
int    w_strncasecmp(const char *a, const char *b, size_t n);
const void *w_memmem(const void *h, size_t hn, const void *n, size_t nn);
void  *w_memchr(const void *s, int c, size_t n);
size_t w_strcspn(const char *s, const char *reject);
size_t w_strlcpy(char *d, const char *s, size_t n);
size_t w_strlcat(char *d, const char *s, size_t n);
int    w_snprintf(char *b, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int    w_hexval(int c);                                  /* 0..15 or -1 */
int    w_isspace(int c);
int    w_tolower(int c);
char  *w_strndup(const char *s, size_t n);

#endif
