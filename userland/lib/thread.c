/* Halcyon user-mode runtime: threads, mutexes and condition variables on top
 * of the kernel's futexes (SYS_FUTEX_WAIT / WAKE). */
#include "hal.h"

typedef struct tstart { int (*fn)(void *); void *arg; } tstart_t;

/* every thread starts here (the kernel enters with rdi = the tstart) */
__attribute__((noreturn)) void __hal_thread_start(tstart_t *st) {
    int (*fn)(void *) = st->fn;
    void *arg = st->arg;
    free(st);
    thread_exit(fn(arg));
}

int thread_create(int (*fn)(void *), void *arg) {
    tstart_t *st = malloc(sizeof *st);
    if (!st) return E_NOMEM;
    st->fn = fn;
    st->arg = arg;
    int r = (int)SC2(SYS_THREAD_CREATE, __hal_thread_start, st);
    if (r < 0) free(st);
    return r;
}

__attribute__((noreturn)) void thread_exit(int code) {
    SC1(SYS_THREAD_EXIT, code);
    for (;;) {}
}

/* mutex: 0 unlocked, 1 locked, 2 locked with waiters (Drepper, "Futexes Are Tricky") */
void mutex_lock(hal_mutex_t *m) {
    int c = __sync_val_compare_and_swap(&m->v, 0, 1);
    if (!c) return;
    if (c != 2) c = __sync_lock_test_and_set(&m->v, 2);
    while (c) {
        futex_wait(&m->v, 2, -1);
        c = __sync_lock_test_and_set(&m->v, 2);
    }
}

int mutex_trylock(hal_mutex_t *m) { return __sync_val_compare_and_swap(&m->v, 0, 1) == 0; }

void mutex_unlock(hal_mutex_t *m) {
    if (__sync_fetch_and_sub(&m->v, 1) != 1) {
        m->v = 0;
        futex_wake(&m->v, 1);
    }
}

/* condition variable: a sequence number that every signal changes */
int cond_wait_ms(hal_cond_t *c, hal_mutex_t *m, int timeout_ms) {
    int seq = c->seq;
    mutex_unlock(m);
    int r = futex_wait(&c->seq, seq, timeout_ms);
    mutex_lock(m);
    return r == E_TIMEOUT ? E_TIMEOUT : 0;
}
void cond_wait(hal_cond_t *c, hal_mutex_t *m) { cond_wait_ms(c, m, -1); }
void cond_signal(hal_cond_t *c) { __sync_fetch_and_add(&c->seq, 1); futex_wake(&c->seq, 1); }
void cond_broadcast(hal_cond_t *c) { __sync_fetch_and_add(&c->seq, 1); futex_wake(&c->seq, 0x7FFFFFFF); }

/* shared memory and the window canvas */
void *shm_map(int h, size_t *size) {
    long r = SC2(SYS_SHM_MAP, h, size);
    return r < 0 ? 0 : (void *)r;
}
