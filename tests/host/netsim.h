/* Host test harness for the network stack: the interface between one
 * instance of the stack (kernel/net/ compiled with net_shim.c into a shared
 * object, loaded once per simulated computer) and the simulator (netsim.c)
 * or the fuzzer (fuzz_net.c). */
#ifndef NETSIM_H
#define NETSIM_H

typedef struct sim_host {
    const unsigned long long *now_ms;           /* the simulated clock */
    void (*xmit)(int node, const void *frame, unsigned len);
    int verbose;                                /* print the stack's kprintf */
} sim_host_t;

/* the functions every instance exports (dlsym names in quotes) */
typedef int  (*sim_init_fn)(const sim_host_t *h, int node, const unsigned char mac[6], unsigned seed);  /* "sim_init" */
typedef void (*sim_setting_fn)(const char *key, const char *value);     /* "sim_setting": before sim_start */
typedef void (*sim_start_fn)(void);                                     /* "sim_start": settings read, configuration */
typedef void (*sim_rx_fn)(const void *frame, unsigned len);             /* "sim_rx" */
typedef unsigned long long (*sim_poll_fn)(void);                        /* "sim_poll": timers; next deadline */
typedef void (*sim_link_fn)(int up);                                    /* "sim_link" */
/* "sim_call": a socket / internet system call (100-127) of the one program;
 * pointers are this process's memory */
typedef long (*sim_call_fn)(int nr, long a1, long a2, long a3, long a4, long a5, long a6);
typedef void *(*sim_dns_start_fn)(const char *name, unsigned timeout_ms, int *err);   /* "sim_dns_start" */
typedef int  (*sim_dns_result_fn)(void *q, unsigned *out, int max);    /* "sim_dns_result": E_AGAIN while pending */
typedef void (*sim_dns_put_fn)(void *q);                                /* "sim_dns_put" */
typedef long (*sim_allocs_fn)(void);                                    /* "sim_allocs": live kernel allocations */
typedef unsigned long long (*sim_netmem_fn)(void);                      /* "sim_netmem": bytes charged to sockets */

#endif
