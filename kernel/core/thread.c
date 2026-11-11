/* Halcyon OS - kernel threads, scheduler, mutexes (see thread.h) */
#include "kernel.h"
#include "lib.h"
#include "io.h"
#include "mm.h"
#include "irq.h"
#include "thread.h"

#define STACK_CANARY 0x57AC4CA9A5A5F00Dull

extern void ctx_switch(u64 *save_rsp, u64 new_rsp);   /* entry.asm */
extern void thread_entry(void);                        /* entry.asm */
void cpu_idle_until(u64 deadline_tsc);                 /* irq.c, IF=0 in and out */
void tss_set_rsp0(u64 rsp0) __attribute__((weak));     /* cpu.c (user mode) */
int g_panicking;
void (*thread_switch_hook)(thread_t *next);           /* proc.c: address space */

static thread_t boot_thread;
static thread_t *threads, *cur;
static u32 next_id = 1;
static int nthreads;

thread_t *thread_current(void) { return cur; }
thread_t *thread_first(void) { return threads; }
int thread_count(void) { return nthreads; }

extern u8 boot_stack[], boot_stack_top[];       /* entry.asm */

void thread_init(void) {
    thread_t *t = &boot_thread;
    /* the boot stack gets the same overflow canary as the others; the
     * canary sits at its lowest address (the stack has room to spare below
     * what is in use by now) */
    t->stack = boot_stack;
    t->stack_top = (u64)(uptr)boot_stack_top;
    *(u64 *)boot_stack = STACK_CANARY;
    t->id = next_id++;
    strlcpy(t->name, "main", sizeof t->name);
    t->state = T_RUNNING;
    t->run_start = rdtsc();
    threads = cur = t;
    nthreads = 1;
}

static void link(thread_t *t) {
    thread_t **pp = &threads;
    while (*pp) pp = &(*pp)->next;
    *pp = t;
    nthreads++;
}

/* C entry of a new thread (reached through thread_entry in entry.asm) */
void thread_start(void) {
    __asm__ volatile("sti" ::: "memory");
    cur->fn(cur->arg);
    thread_exit();
}

thread_t *thread_create(const char *name, void (*fn)(void *), void *arg) {
    if (!cur) return NULL;
    thread_t *t = kzalloc(sizeof *t);
    u8 *stack = t ? kmalloc_aligned(THREAD_STACK, 16) : NULL;
    if (!stack) { kfree(t); return NULL; }
    *(u64 *)stack = STACK_CANARY;
    t->id = next_id++;
    strlcpy(t->name, name, sizeof t->name);
    t->stack = stack;
    t->stack_top = (u64)(uptr)(stack + THREAD_STACK);
    t->fn = fn;
    t->arg = arg;
    /* initial frame for ctx_switch: x87 CW / MXCSR, r15..rbp, rflags, return */
    u64 *sp = (u64 *)(uptr)t->stack_top;
    *--sp = 0;                                   /* alignment */
    *--sp = (u64)(uptr)thread_entry;             /* ret */
    *--sp = 0x002;                               /* rflags: IF off until thread_start */
    for (int i = 0; i < 6; i++) *--sp = 0;       /* rbp rbx r12 r13 r14 r15 */
    *--sp = ((u64)0x037F << 32) | 0x1F80;        /* MXCSR (low), x87 control word (high) */
    t->rsp = (u64)(uptr)sp;
    t->state = T_READY;
    u64 f = irq_save();
    link(t);
    irq_restore(f);
    return t;
}

int thread_alive(u32 id) {
    for (thread_t *t = threads; t; t = t->next) if (t->id == id && t->state != T_DEAD) return 1;
    return 0;
}

/* free the stacks of threads that have exited (never the current one) */
static void reap(void) {
    thread_t **pp = &threads;
    while (*pp) {
        thread_t *t = *pp;
        if (t->state == T_DEAD && t != cur && t != &boot_thread) {
            *pp = t->next;
            nthreads--;
            kfree(t->stack);
            kfree(t);
            continue;
        }
        pp = &t->next;
    }
}

static int runnable(thread_t *t, u64 now) {
    if (t->state == T_READY || t->state == T_RUNNING) return 1;
    if (t->state != T_WAITING) return 0;
    if (t->cond && t->cond(t->cond_arg)) return 1;
    return now >= t->deadline;
}

static void switch_to(thread_t *n) {
    thread_t *o = cur;
    u64 now = rdtsc();
    o->cpu_tsc += now - o->run_start;
    n->run_start = now;
    if (o->state == T_RUNNING) o->state = T_READY;
    n->state = T_RUNNING;
    if (n->stack && *(u64 *)n->stack != STACK_CANARY) panic("kernel stack overflow in thread '%s'", n->name);
    cur = n;
    if (n->stack && tss_set_rsp0) tss_set_rsp0(n->stack_top);
    if (thread_switch_hook) thread_switch_hook(n);
    ctx_switch(&o->rsp, n->rsp);
}

/* Run the next runnable thread (round robin, the current one last). Returns
 * when the current thread is chosen again. Halts the CPU while nothing can
 * run. Works with interrupts disabled throughout, except while halting. */
static void schedule(void) {
    u64 f = irq_save();
    for (;;) {
        reap();
        u64 now = rdtsc();
        thread_t *start = cur->next ? cur->next : threads, *t = start, *pick = NULL;
        do {
            if (runnable(t, now)) { pick = t; break; }
            t = t->next ? t->next : threads;
        } while (t != start);
        if (pick) {
            if (pick != cur) switch_to(pick);
            else {
                cur->state = T_RUNNING;
                if (thread_switch_hook) thread_switch_hook(cur);   /* a user thread gets a new time slice */
            }
            break;
        }
        /* nothing can run: sleep until the earliest deadline or an interrupt */
        u64 dl = ~0ull;
        for (t = threads; t; t = t->next) if (t->state == T_WAITING && t->deadline < dl) dl = t->deadline;
        cpu_idle_until(dl);
        /* acknowledge level-triggered lines (needs interrupts on) */
        __asm__ volatile("sti" ::: "memory");
        irq_service();
        __asm__ volatile("cli" ::: "memory");
    }
    irq_restore(f);
}

void thread_yield(void) {
    if (!cur || g_panicking) { cpu_relax(); return; }
    cur->state = T_READY;
    schedule();
}

int thread_wait(wait_cond_t cond, void *arg, u64 deadline) {
    if (!cur || g_panicking) { cpu_relax(); return cond ? cond(arg) : 0; }
    cur->cond = cond;
    cur->cond_arg = arg;
    cur->deadline = deadline;
    cur->state = T_WAITING;
    schedule();
    cur->cond = NULL;
    return cond ? (cond(arg) != 0) : 0;
}

static int gen_changed(void *arg) { return irq_generation() != (u32)(uptr)arg; }
void thread_wait_event(u32 gen, u64 deadline) { thread_wait(gen_changed, (void *)(uptr)gen, deadline); }

void thread_sleep_ms(u64 ms) { thread_wait(NULL, NULL, time_ms_to_tsc(time_ms() + ms)); }

NORETURN void thread_exit(void) {
    if (cur == &boot_thread) panic("the main thread exited");
    __asm__ volatile("cli" ::: "memory");
    cur->state = T_DEAD;
    schedule();
    panic("dead thread scheduled");
}

/* ---------------- mutexes ---------------- */
static int mutex_free(void *arg) { return ((mutex_t *)arg)->owner == NULL; }

void mutex_lock(mutex_t *m) {
    if (g_panicking || !cur) return;
    if (m->owner == cur) { m->depth++; return; }
    while (m->owner) {
        m->contended++;
        thread_wait(mutex_free, m, ~0ull);
    }
    m->owner = cur;
    m->depth = 1;
}

int mutex_trylock(mutex_t *m) {
    if (g_panicking || !cur) return 1;
    if (m->owner && m->owner != cur) return 0;
    if (m->owner == cur) m->depth++;
    else { m->owner = cur; m->depth = 1; }
    return 1;
}

void mutex_unlock(mutex_t *m) {
    if (g_panicking || !cur) return;
    if (m->owner != cur) { kprintf("mutex %s: unlock by a thread that does not hold it\n", m->name); return; }
    if (--m->depth) return;
    m->owner = NULL;
    /* hand over: let a waiting thread take it before we relock it in a loop */
    if (m->contended) {
        m->contended = 0;
        thread_yield();
    }
}

int mutex_held(mutex_t *m) { return g_panicking || m->owner == cur; }
