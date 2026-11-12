#ifndef PROC_H
#define PROC_H
/* Halcyon OS - user processes (see docs/DESIGN-M4.md) */
#include "types.h"
#include "thread.h"
struct regs;

#define USER_BASE       0x0000200000000000ull   /* PML4 entry 64: programs load here */
/* end of user memory: one page short of the canonical lower half, so the
 * address after a SYSCALL instruction (SYSRET's RCX) is always canonical */
#define USER_END        0x00007FFFFFFFF000ull
#define USER_STACK_TOP  0x00007FFF00000000ull
#define USER_STACK_SIZE (1024 * 1024)
#define USER_MAX_PAGES  (256 * 1024)            /* 1 GiB per process */
#define PROC_HANDLES    64
#define MAX_PROCS       64                      /* running programs */
/* threads: slot 0 is the main thread; slot i's stack is the 1 MiB below
 * USER_STACK_TOP - i * THREAD_SLOT (the rest of the slot stays unmapped as a
 * guard) */
#define MAX_UTHREADS    32
#define THREAD_SLOT     (4ull << 20)
#define UTHREAD_STACK   (1024 * 1024)
/* shared mappings (window canvases, shared memory): 256 slots of 64 MiB */
#define MAP_BASE        0x0000600000000000ull
#define MAP_SLOT        (64ull << 20)
#define MAP_SLOTS       256

enum { H_NONE = 0, H_FILE, H_WINDOW, H_DIR, H_SOCKET, H_SHM, H_SOUND };

typedef struct handle { int type; void *obj; } handle_t;

typedef struct console_ops {
    void (*write)(void *ctx, const char *s, int n);
    int  (*read)(void *ctx, char *buf, int n);     /* blocks for a line; < 0 = end of input */
    void (*attach)(void *ctx, u32 pid);            /* another program uses this console */
    void (*exited)(void *ctx, u32 pid, int code);  /* a program using it ended */
} console_ops_t;

enum { P_RUNNING = 1, P_ZOMBIE };

typedef struct uthread {
    thread_t *t;                    /* NULL once ended */
    u32 tid;                        /* 0 = free slot */
    int done, code, detached;
} uthread_t;

typedef struct fwait {              /* a thread waiting in SYS_FUTEX_WAIT */
    u64 addr;
    volatile int woken;
    struct fwait *next;
} fwait_t;

typedef struct proc {
    u32 pid;
    char name[32];                  /* "CALC.EXE" */
    char path[260];
    char cwd[260];                  /* current directory ("C:/") */
    int state;
    int exit_code;
    volatile int killed;            /* end at the next return to user mode */
    char fault[160];                /* why it ended, if it did not exit by itself */
    int crashed;                    /* it ended because of an exception */
    u64 *pml4;
    u64 pages;                      /* user pages mapped */
    u64 brk_start, brk;
    u64 image_base, entry;
    int nthreads;
    thread_t *main;                 /* NULL once it has ended */
    uthread_t uthreads[MAX_UTHREADS];
    u32 next_tid;
    fwait_t *futexq;
    u64 map_used[MAP_SLOTS / 64];   /* shared mapping slots in use */
    u32 map_pages[MAP_SLOTS];
    u32 confirm_pending;            /* a permission question is on screen */
    u64 cpu_tsc;                    /* CPU time of threads that have ended */
    handle_t handles[PROC_HANDLES];
    void *gui;                      /* window system state (gui/usergui.c) */
    const console_ops_t *con;       /* stdout / stdin (Command Prompt) or NULL */
    void *con_ctx;
    u32 parent;
    u64 start_ms;
    struct proc *next;
} proc_t;

void proc_init(void);
/* load an executable and start it; returns the pid or E_* */
int  proc_spawn(const char *path, const char *args, const console_ops_t *con, void *con_ctx);
proc_t *proc_current(void);         /* NULL in kernel threads */
proc_t *proc_by_pid(u32 pid);
proc_t *proc_first(void);
int  proc_kill(u32 pid, const char *why);
NORETURN void proc_exit(int code);  /* the current process */
int  proc_alive(u32 pid);
int  proc_exit_code(u32 pid);       /* after it ended, until reaped */
void proc_detach_console(void *con_ctx);

/* user memory, checked page by page against the current process's tables */
int  copyin(void *dst, u64 uva, u64 len);
int  copyout(u64 uva, const void *src, u64 len);
int  copyin_str(char *dst, u64 uva, u64 max);       /* E_NAMETOOLONG if no NUL within max */
int  user_writable(u64 uva, u64 len);               /* mapped and writable by the current process */

/* grow the process's memory (sbrk) */
int  vm_grow(proc_t *p, u64 va, u64 len);
/* a wait in a system call: like thread_wait, but returns E_CANCELED as soon
 * as the process is being ended */
int  proc_wait(wait_cond_t cond, void *arg, u64 deadline_tsc);

/* threads (SYS_THREAD_*): returns the thread id or E_* */
int  uthread_create(proc_t *p, u64 rip, u64 arg);
NORETURN void uthread_exit(int code);
int  uthread_join(proc_t *p, u32 tid);
int  uthread_detach(proc_t *p, u32 tid);
int  futex_wait(proc_t *p, u64 addr, u32 expected, u64 deadline_tsc);
int  futex_wake(proc_t *p, u64 addr, int n);
void uthread_set_tls(u64 base);

/* map already allocated kernel memory (page aligned, size a multiple of 4 KiB)
 * into the process: returns the user address or 0. The pages stay owned by
 * the caller: unmap before freeing them. */
u64  vm_map_shared(proc_t *p, void *mem, u64 size, int writable);
int  vm_unmap_shared(proc_t *p, u64 va);
int  proc_is_ancestor(u32 ancestor, u32 pid);

/* named shared memory (shm.c) */
long shm_open(proc_t *p, u64 uname, u64 size, u64 flags);
long shm_map(proc_t *p, int h, u64 usize);
long shm_unmap(proc_t *p, u64 va);
int  shm_close(proc_t *p, int h);
void shm_release_all(proc_t *p);

/* handles */
int  handle_alloc(proc_t *p, int type, void *obj);
void *handle_get(proc_t *p, int h, int type);
void handle_free(proc_t *p, int h);

/* hooks for other subsystems */
extern void (*proc_release_hook)(proc_t *p);       /* windows etc. of an ending process */
extern void (*proc_crash_hook)(proc_t *p);         /* show "has encountered a problem" */

/* sockets (net/, weak): close / read / write of an H_SOCKET handle, and the
 * sockets of an ending process */
int  socket_handle_close(proc_t *p, int h) __attribute__((weak));
long socket_handle_io(proc_t *p, int h, u64 buf, u64 len, int write) __attribute__((weak));
void socket_release_all(proc_t *p) __attribute__((weak));

/* sound streams (sound/sndsys.c, weak): close of an H_SOUND handle, poll,
 * and the streams of an ending process */
int  sound_handle_close(proc_t *p, int h) __attribute__((weak));
long sound_handle_write(proc_t *p, int h, u64 buf, u64 len) __attribute__((weak));   /* write() */
int  sound_handle_poll(proc_t *p, int h) __attribute__((weak));    /* POLL* bits */
u32  sound_wait_gen(void) __attribute__((weak));                   /* changes when a stream may have room */
void sound_release_all(proc_t *p) __attribute__((weak));

/* called from isr_dispatch for exceptions and interrupts taken in user mode */
void proc_user_fault(struct regs *r);
void proc_user_return(struct regs *r);              /* before iretq to user mode */
/* the current user thread's time slice: yield if it is used up, else make
 * sure the timer will interrupt it when it is (called before returning to
 * user mode, interrupts disabled) */
void proc_slice_check(void);
u64  proc_cpu_ms(proc_t *p);
extern volatile int g_need_resched;

#endif
