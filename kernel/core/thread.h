#ifndef THREAD_H
#define THREAD_H
/* Halcyon OS - kernel threads and the scheduler (see docs/DESIGN-M4.md).
 *
 * Kernel code runs under an implicit big kernel lock: a kernel thread only
 * gives up the CPU at a blocking point (a wait for a device or a deadline, a
 * mutex, thread_yield/sleep, exit). Data used only between blocking points
 * needs no lock; state kept across a blocking point is protected by a mutex. */
#include "types.h"

#define THREAD_STACK (32 * 1024)

typedef struct thread thread_t;
typedef int (*wait_cond_t)(void *arg);

enum { T_READY = 1, T_RUNNING, T_WAITING, T_DEAD };

struct thread {
    u32 id;
    char name[24];
    int state;
    u64 rsp;                    /* saved stack pointer while switched out */
    u8 *stack;                  /* NULL for the boot thread */
    u64 stack_top;
    /* waiting: until cond(arg) is true or the TSC passes deadline */
    wait_cond_t cond;
    void *cond_arg;
    u64 deadline;
    void (*fn)(void *);
    void *arg;
    void *proc;                 /* user process (milestone 4b), NULL for kernel threads */
    u64 fs_base;                /* user threads: FS segment base (thread-local storage) */
    u32 utid;                   /* user threads: thread id within the process */
    u64 cpu_tsc;                /* time spent running */
    u64 run_start;
    thread_t *next;             /* all threads */
};

void thread_init(void);                       /* the boot thread becomes thread 1 ("main") */
thread_t *thread_current(void);
thread_t *thread_create(const char *name, void (*fn)(void *), void *arg);
NORETURN void thread_exit(void);
void thread_yield(void);                      /* let other ready threads run */
void thread_sleep_ms(u64 ms);
/* block until cond(arg) returns nonzero (checked by the scheduler with
 * interrupts disabled: keep it trivial) or the TSC reaches deadline (~0 =
 * never). Returns 1 if the condition became true, 0 on timeout. */
int  thread_wait(wait_cond_t cond, void *arg, u64 deadline_tsc);
/* wait for a device interrupt after generation gen, or the deadline */
void thread_wait_event(u32 gen, u64 deadline_tsc);
int  thread_alive(u32 id);
int  thread_count(void);
thread_t *thread_first(void);

/* sleeping, recursive mutex */
typedef struct mutex {
    thread_t *owner;
    u32 depth;
    const char *name;
    u32 contended;
} mutex_t;
#define MUTEX_INIT(n) { NULL, 0, n, 0 }
void mutex_lock(mutex_t *m);
int  mutex_trylock(mutex_t *m);               /* 1 = acquired */
void mutex_unlock(mutex_t *m);
int  mutex_held(mutex_t *m);                  /* by the current thread */

#endif
