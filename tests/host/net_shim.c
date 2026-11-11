/* Host shim for the network stack: the kernel services kernel/net/ uses
 * (memory, log, clock, randomness, settings, the network device registry,
 * user memory, the one process), on top of libc, plus the functions the
 * simulator calls (netsim.h). One copy of this file and the stack makes one
 * simulated computer; netsim.c loads several copies as shared objects. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#define timeval hal_timeval            /* (libc has its own) */
#include "core/types.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/proc.h"
#include "core/settings.h"
#include "net/netdev.h"
#include "net/socket.h"
#include "net/inet.h"
#include "netsim.h"

#define EXPORT __attribute__((visibility("default")))

static const sim_host_t *host;
static int node_id;
static long live_allocs;
static u64 rng;

/* ---------------- memory ---------------- */
void *kmalloc(size_t n) { void *p = malloc(n ? n : 1); if (p) live_allocs++; return p; }
void *kzalloc(size_t n) { void *p = calloc(1, n ? n : 1); if (p) live_allocs++; return p; }
void *kmalloc_aligned(size_t n, size_t a) { (void)a; return kmalloc(n); }
void *krealloc(void *p, size_t n) { void *q = realloc(p, n ? n : 1); if (q && !p) live_allocs++; return q; }
void kfree(void *p) { if (p) { live_allocs--; free(p); } }
u64 mm_heap_free(void) { return 1ull << 30; }

/* ---------------- log ---------------- */
void kprintf(const char *fmt, ...) {
    if (!host || !host->verbose) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "    [%llu.%03llu n%d] ", *host->now_ms / 1000, *host->now_ms % 1000, node_id);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void panic(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "PANIC (node %d): ", node_id);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    abort();
}

/* ---------------- clock, randomness, fw_cfg ---------------- */
u64 time_ms(void) { return host ? *host->now_ms : 0; }
u64 time_us(void) { return time_ms() * 1000 + (rng & 999); }
u64 time_ms_to_tsc(u64 ms) { return ms; }

static u64 next_rand(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return rng;
}
u32 random_u32(void) { return (u32)(next_rand() >> 16); }
void random_bytes(void *buf, size_t n) {
    u8 *b = buf;
    for (size_t i = 0; i < n; i++) b[i] = (u8)(next_rand() >> 24);
}
void random_add(u64 v) { rng ^= v; }
int qemu_fw_file(const char *name, char *buf, int bufsz) { (void)name; (void)buf; (void)bufsz; return 0; }

/* ---------------- settings ---------------- */
#define NSET 32
static struct { char k[48], v[208]; } sets[NSET];
static int nsets;

const char *settings_get(const char *key, const char *def) {
    for (int i = 0; i < nsets; i++) if (!strcasecmp(sets[i].k, key)) return sets[i].v;
    return def;
}
int settings_get_int(const char *key, int def) { const char *v = settings_get(key, NULL); return v ? atoi(v) : def; }
void settings_set(const char *key, const char *value) {
    for (int i = 0; i < nsets; i++) {
        if (strcasecmp(sets[i].k, key)) continue;
        if (!value) { sets[i] = sets[--nsets]; return; }
        snprintf(sets[i].v, sizeof sets[i].v, "%s", value);
        return;
    }
    if (!value || nsets >= NSET) return;
    snprintf(sets[nsets].k, sizeof sets[nsets].k, "%s", key);
    snprintf(sets[nsets].v, sizeof sets[nsets].v, "%s", value);
    nsets++;
}
void settings_set_int(const char *key, int value) { char b[16]; snprintf(b, sizeof b, "%d", value); settings_set(key, b); }
int settings_save(void) { return 0; }

/* ---------------- network devices (netdev.c's registry) ---------------- */
static netdev_t dev;
static int ndev;
int netdev_register(netdev_t *d) { (void)d; return 0; }
int netdev_count(void) { return ndev; }
netdev_t *netdev_get(int i) { return i == 0 && ndev ? &dev : NULL; }
void netdev_kick(void) {}
void netdev_rx(netdev_t *d, const void *frame, u32 len) {
    if (len < ETH_HDR || len > ETH_MAX_FRAME + 4) { d->rx_errors++; return; }
    d->rx_packets++;
    d->rx_bytes += len;
    net_stack_input(d, frame, len);
}

static int sim_xmit(netdev_t *d, const void *frame, u32 len) {
    if (len > ETH_MAX_FRAME) return E_INVAL;
    d->tx_packets++;
    d->tx_bytes += len;
    host->xmit(node_id, frame, len);
    return 0;
}
static void sim_devpoll(netdev_t *d) { (void)d; }

/* ---------------- the one process, user memory ---------------- */
static proc_t proc;
proc_t *proc_current(void) { return &proc; }
int copyin(void *dst, u64 uva, u64 len) { if (!uva && len) return E_INVAL; if (len) memcpy(dst, (const void *)(uptr)uva, len); return 0; }
int copyout(u64 uva, const void *src, u64 len) { if (!uva && len) return E_INVAL; if (len) memcpy((void *)(uptr)uva, src, len); return 0; }
int user_writable(u64 uva, u64 len) { return uva >= 4096 || !len; }
int copyin_str(char *dst, u64 uva, u64 max) {
    if (uva < 4096) return E_INVAL;
    const char *s = (const char *)(uptr)uva;
    for (u64 i = 0; i < max; i++) { dst[i] = s[i]; if (!s[i]) return 0; }
    if (max) dst[max - 1] = 0;
    return E_NAMETOOLONG;
}
int proc_wait(wait_cond_t cond, void *arg, u64 deadline_tsc) {
    (void)deadline_tsc;
    return cond && cond(arg) ? 1 : 0;           /* never sleeps: "timed out" */
}
int handle_alloc(proc_t *p, int type, void *obj) {
    for (int i = 3; i < PROC_HANDLES; i++)
        if (p->handles[i].type == H_NONE) { p->handles[i].type = type; p->handles[i].obj = obj; return i; }
    return E_NOMEM;
}
void *handle_get(proc_t *p, int h, int type) {
    if (h < 0 || h >= PROC_HANDLES || p->handles[h].type != type) return NULL;
    return p->handles[h].obj;
}
void handle_free(proc_t *p, int h) { if (h >= 0 && h < PROC_HANDLES) { p->handles[h].type = H_NONE; p->handles[h].obj = NULL; } }

/* (the generic socket layer calls these for other handle types; unused here) */
long socket_syscall(int nr, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6);
int socket_handle_close(proc_t *p, int h);

/* ---------------- what the simulator calls ---------------- */

EXPORT int sim_init(const sim_host_t *h, int node, const unsigned char mac[6], unsigned seed) {
    host = h;
    node_id = node;
    rng = 0x9E3779B97F4A7C15ull ^ ((u64)seed << 17) ^ (u64)node;
    for (int i = 0; i < 8; i++) next_rand();
    memset(&proc, 0, sizeof proc);
    proc.pid = 1;
    snprintf(proc.name, sizeof proc.name, "NODE%d", node);
    if (mac) {
        memset(&dev, 0, sizeof dev);
        snprintf(dev.name, sizeof dev.name, "eth0");
        snprintf(dev.desc, sizeof dev.desc, "simulated Ethernet");
        dev.driver = "sim";
        memcpy(dev.mac, mac, 6);
        dev.mtu = 1500;
        dev.link_up = 1;
        dev.speed_mbps = 1000;
        dev.full_duplex = 1;
        dev.xmit = sim_xmit;
        dev.poll = sim_devpoll;
        dev.irq_live = 1;
        ndev = 1;
    }
    net_stack_init();
    return 0;
}

EXPORT void sim_setting(const char *key, const char *value) { settings_set(key, value); }
EXPORT void sim_start(void) { net_settings_loaded(); net_stack_poll(time_ms()); }
EXPORT void sim_rx(const void *frame, unsigned len) { if (ndev) netdev_rx(&dev, frame, len); }
EXPORT unsigned long long sim_poll(void) { return net_stack_poll(time_ms()); }
EXPORT void sim_link(int up) { dev.link_up = up; net_stack_link(&dev); }
EXPORT long sim_call(int nr, long a1, long a2, long a3, long a4, long a5, long a6) {
    if (nr == 21) return socket_handle_close(&proc, (int)a1);    /* SYS_CLOSE */
    return socket_syscall(nr, (u64)a1, (u64)a2, (u64)a3, (u64)a4, (u64)a5, (u64)a6);
}
/* as sys_resolve (inet.c): literals and the cache first, then a query */
static struct { int n; u32 a[DNS_MAX_ADDRS]; } fastq;
EXPORT void *sim_dns_start(const char *name, unsigned timeout_ms, int *err) {
    *err = 0;
    if (dns_literal(name, fastq.a)) { fastq.n = 1; return &fastq; }
    if (!dns_name_ok(name)) { *err = E_INVAL; return NULL; }
    int n = dns_cached(name, fastq.a, DNS_MAX_ADDRS);
    if (n) { fastq.n = n; return &fastq; }
    return dns_start(name, time_ms() + timeout_ms, err);
}
EXPORT int sim_dns_result(void *q, unsigned *out, int max) {
    if (q == &fastq) {
        if (fastq.n < 0) return fastq.n;
        int k = fastq.n < max ? fastq.n : max;
        for (int i = 0; i < k; i++) out[i] = fastq.a[i];
        return k;
    }
    return dns_result(q, out, max);
}
EXPORT void sim_dns_put(void *q) { if (q != &fastq) dns_put(q); }
EXPORT long sim_allocs(void) { return live_allocs; }
EXPORT unsigned long long sim_netmem(void) {
    hal_sockstat_t st;
    socket_syscall(SYS_SOCKSTAT, (u64)(uptr)&st, 0, 0, 0, 0, 0);
    return st.mem;
}
