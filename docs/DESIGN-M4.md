# Milestone 4 design: threads and user-mode programs

## 4a: kernel threads

- **Model: one big kernel lock, implicitly.** Kernel code never runs in two
  threads at once. A kernel thread gives up the CPU only at blocking points:
  `idle_wait()` / `irq_wait()` (waiting for a device), `mdelay()`, mutexes,
  `thread_yield()`, `thread_sleep()`, exit. So data that is only touched between
  blocking points needs no locking (heap, klog, GUI structures...).
- **What needs a lock** is state held *across* a blocking point. Sleeping,
  recursive mutexes:
  - `storage_lock`: every VFS call, block-device registration, file system code
    and the storage drivers below it (AHCI, NVMe, USB mass storage I/O).
  - `usb_lock`: the USB core and host-controller drivers (transfers, root-hub
    service, enumeration). The input path (`usb_poll` from the GUI loop) only
    *tries* it and skips a round if a transfer is in progress.
  - Order: storage_lock, then usb_lock. USB hot-plug therefore queues block-device
    registration and removal ("storage jobs"), which run later from the GUI loop
    with storage_lock taken first.
- **Scheduler:** round-robin among ready threads. Threads wait for: a device
  interrupt since generation `gen`, a deadline (TSC), a mutex, or another thread.
  When nothing is ready the CPU halts as in milestone 2. Without working
  interrupts (F8 "Disable interrupts") waits poll instead.
- **Context switch:** callee-saved registers, RFLAGS, MXCSR and the x87 control
  word (all SSE registers are caller-saved in the SysV ABI, so a voluntary switch
  doesn't need to save them).
- **First uses:** Explorer copy / move / delete run in a worker thread with a
  progress dialog (Cancel works), so a long copy no longer freezes the desktop.

## 4b: user-mode programs

- **Address space:** each process has its own PML4. Entry 0 (the identity-mapped
  first 4 GiB, supervisor-only) and any other kernel entries are shared; user
  memory lives at 0x2000_0000_0000 and above (PML4 entries 64..255) in 4 KiB
  pages from a page pool (1 MiB chunks taken from the kernel heap).
- **CPU setup:** GDT with user code/data segments and a TSS (RSP0 = the current
  thread's kernel stack, IST for double fault / NMI); `syscall`/`sysret`.
- **Programs:** static position-independent ELF64 executables (`.EXE` files)
  loaded from disk, `C:\PROGRAMS` by default. A small runtime (`userland/`): crt0,
  syscall wrappers, a minimal C library, a GUI library.
- **Preemption:** user threads are preempted by a timer tick. Kernel threads stay
  cooperative, so the implicit big lock still holds.
- **System calls:** process (exit, spawn, wait, sleep, time), memory (sbrk),
  files (open/read/write/seek/close/stat/readdir/mkdir/unlink/rename through the
  VFS with per-process handles), console (stdout/stdin to the Command Prompt that
  started the program), GUI (windows whose client surface lives in the kernel;
  drawing calls like an early-2000s GDI; events; standard controls; message box).
- **Isolation:** a fault in user mode ends only that process; the desktop shows
  "<program> has encountered a problem and needs to close". Its windows, files
  and memory are released. Task Manager (and TASKLIST / TASKKILL) can end a
  process that hangs.
- **Programs shipped:** a console Hello, Calculator and Sonar moved to user mode,
  and a Crash Test program that deliberately misbehaves.

## As built: protection, limits and known gaps

- **Memory protection:** user pages are user-only and, when the CPU has NX,
  not executable unless the ELF segment says so (code is not writable). The
  kernel turns on CR0.WP, and SMEP, SMAP and UMIP when the CPU reports them; it
  reaches program memory only by translating through the program's page tables
  into the identity map, after checking every pointer and length. Device
  memory is never mapped into the user half.
- **Bounded work per call:** drawing coordinates and radii are clamped; a
  program's canvases share an 8M-pixel quota, a window takes at most 256
  controls, and GUI allocations leave 32 MiB of kernel heap untouched. At most
  4 message boxes per program; a program waiting in one can still be ended.
  Program file dialogs (modal) run one at a time. Programs are capped in number
  and need a working timer interrupt (so a loop cannot freeze the desktop).
- **Message boxes and crash reports** are ordinary windows: the program's
  thread waits, the GUI thread never nests a loop on a program's behalf.
- **Kernel stacks** (32 KiB per thread, and the boot stack) carry a canary
  checked on every switch.
- **Calculator and Sonar** are `CALC.EXE` and `SONAR.EXE`; the Start menu
  starts them. The menu bars became button strips (the user API has no menus
  yet). The built-in versions stay in the kernel and are used only when the
  program cannot run (no `C:\PROGRAMS`, or interrupts disabled).
- **Known gap: no permissions between programs.** Every program runs with the
  same rights: it can read and write every file and end any other program
  (`SYS_KILL`), as on a single-user Windows 9x/XP desktop without accounts. The
  separation is about faults, not about trust.
