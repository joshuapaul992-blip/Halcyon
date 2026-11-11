#ifndef PROC_H
#define PROC_H
/* Halcyon OS - user processes (see docs/DESIGN-M4.md) */
#include "types.h"
#include "thread.h"
struct regs;

#define USER_BASE       0x0000200000000000ull   /* PML4 entry 64: programs load here */
#define USER_END        0x0000800000000000ull   /* end of the canonical lower half */
#define USER_STACK_TOP  0x00007FFF00000000ull
#define USER_STACK_SIZE (1024 * 1024)
#define USER_MAX_PAGES  (256 * 1024)            /* 1 GiB per process */
#define PROC_HANDLES    64

enum { H_NONE = 0, H_FILE, H_WINDOW, H_DIR };

typedef struct handle { int type; void *obj; } handle_t;

typedef struct console_ops {
    void (*write)(void *ctx, const char *s, int n);
    int  (*read)(void *ctx, char *buf, int n);     /* blocks for a line; < 0 = end of input */
    void (*exited)(void *ctx, int code);           /* the program ended */
} console_ops_t;

enum { P_RUNNING = 1, P_ZOMBIE };

typedef struct proc {
    u32 pid;
    char name[32];                  /* "CALC.EXE" */
    char path[260];
    char cwd[260];                  /* current directory ("C:/") */
    int state;
    int exit_code;
    volatile int killed;            /* end at the next return to user mode */
    char fault[160];                /* why it was ended, if it crashed */
    u64 *pml4;
    u64 pages;                      /* user pages mapped */
    u64 brk_start, brk;
    u64 image_base, entry;
    int nthreads;
    thread_t *main;
    handle_t handles[PROC_HANDLES];
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

/* grow the process's memory (sbrk) */
int  vm_grow(proc_t *p, u64 va, u64 len);
/* a wait in a system call: like thread_wait, but returns E_CANCELED as soon
 * as the process is being ended */
int  proc_wait(wait_cond_t cond, void *arg, u64 deadline_tsc);

/* handles */
int  handle_alloc(proc_t *p, int type, void *obj);
void *handle_get(proc_t *p, int h, int type);
void handle_free(proc_t *p, int h);

/* hooks for other subsystems */
extern void (*proc_release_hook)(proc_t *p);       /* windows etc. of an ending process */
extern void (*proc_crash_hook)(proc_t *p);         /* show "has encountered a problem" */

/* called from isr_dispatch for exceptions and interrupts taken in user mode */
void proc_user_fault(struct regs *r);
void proc_user_return(struct regs *r);              /* before iretq to user mode */
extern volatile int g_need_resched;

#endif
