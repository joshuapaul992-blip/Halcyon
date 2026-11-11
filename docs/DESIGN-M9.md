# Milestone 9 design: program platform

Milestone 9 makes user programs capable enough to carry the desktop's own
applications: threads, shared memory, direct access to window pixels, menus
and the other window controls, sockets, and a first permission rule. Notepad,
Paint and Explorer then moved out of the kernel into `C:\PROGRAMS` (the
built-in versions remain only as a fallback, see "The shell" below).

## Threads

- Up to 32 threads per program (`thread_create`, `thread_join`,
  `thread_detach`, `thread_exit`; slot 0 is the main thread). Each kernel
  thread of a program has its own kernel stack, as before; the user stack of
  thread slot *i* is the 1 MiB below `USER_STACK_TOP - i * 4 MiB`, and the
  rest of the 4 MiB slot stays unmapped, so a stack overflow faults instead of
  running into the next thread's stack.
- `exit()` (and a crash in any thread, or being ended) ends the whole program:
  the other threads stop at their next return to user mode or blocking wait
  (`proc_wait` returns `E_CANCELED`). The last thread to stop releases the
  process. `thread_exit` ends only the calling thread and frees its stack.
- Preemption is per thread (10 ms slices), as it was per program.
- Futexes (`SYS_FUTEX_WAIT/WAKE`) are per process, keyed by address: a wait
  compares the 32-bit word and sleeps in one step (system calls run under the
  big kernel lock until they block, so no wake-up can slip in between). The
  library builds mutexes (three-state, after Drepper's "Futexes Are Tricky")
  and condition variables (a sequence number) on them; `malloc` is locked.
- Thread-local storage: each thread has an FS base (`SYS_SET_TLS`), loaded on
  every switch to it. The compiler's `__thread` is not supported yet (the
  loader does not set up a TLS segment); M14 needs the FS base for Linux
  programs.

## Shared memory and window pixels

- Mappings of memory that a process does not own live in their own region of
  the address space: 256 slots of 64 MiB from `0x6000_0000_0000`. Their page
  table entries carry a software bit (`PTE_SHARED`) so tearing down the
  process never frees those frames.
- Named shared memory (`shm_open(name, size, SHM_CREATE)`, `shm_map`,
  `shm_unmap`): page-aligned kernel memory, alive while a handle or mapping
  refers to it; 64 MiB per object, 128 MiB in all, 64 MiB of kernel heap kept
  free.
- `win_canvas(win, &c)` maps the window's canvas into the program: drawing
  calls and direct pixel writes go to the same memory (canvases are now
  allocated in whole, page-aligned pages). When the user resizes the window,
  the kernel allocates the new canvas but keeps the old one alive while the
  program still has it mapped; the program maps again after `EVT_RESIZE` and
  only then is the old one freed, so a program drawing at the moment of the
  resize never touches freed memory.

## Permission to end programs

A program may end itself and its descendants. Ending any other program asks
the user in a kernel window ("TASKMGR.EXE (process 2) wants to end SONAR.EXE
(process 1)..."), with No as the default button; the asking thread waits
(killably), one question per program at a time, and `kill` returns `E_PERM`
if the answer is no. Programs cannot inject input, so only the user can
answer. Task Manager's End Process relies on this instead of its own warning.
There are no other permissions yet: every program can read and write every
file and bind any socket name.

## The shell

- `shell_open(path)` opens a file the way a double click does (programs by
  file type, folders in Explorer), `shell_format(drive)` opens the kernel's
  Format dialog (one at a time; the user decides there), `eject(drive)`,
  `volume_info(drive)`, `vfs_generation()` (changes when drives come and go)
  and `datetime()`.
- The desktop, Start menu, file types and the Command Prompt start
  NOTEPAD.EXE, PAINT.EXE and EXPLORER.EXE with the file or folder as one
  quoted argument. If the program cannot start (no `C:\PROGRAMS`, or no timer
  interrupt in the fully polled mode, where programs are not allowed), the
  built-in kernel version opens instead. The built-in versions are frozen:
  new features go into the programs.

## Window API additions

Menus (a flat item array with title entries for a menu bar; popups; enable,
check; keyboard access with F10, Alt and mnemonics, now for all windows),
scroll bars, tool buttons with icons and tooltips, a docked status bar, cursor
shapes, a clip rectangle and origin for drawing calls, font metrics copied to
the program so text is measured locally, theme colours, drawing helpers
(gradients, rounded rectangles), control moves and keyboard focus, and a
clipboard of up to 1 MiB. `GUIDEMO.EXE` exercises all of them.

## Sockets and poll

Programs get BSD sockets now, with one address family, AF_LOCAL (programs on
this computer, by name), so that programs can talk to each other and so that
milestone 6 only has to add the internet families. Code: `kernel/net/`
(`socket.c` generic layer, `local.c` AF_LOCAL), ABI in `kernel/net/sockabi.h`
(included by `core/sysabi.h`), user wrappers in `userland/lib/net.c` and
`hal.h`. Test: `SOCKTEST.EXE` (boot test variant `sock`).

### The interface

Names, argument order, structures and constant values are BSD's as Linux lays
them out (a 16-bit family first in every address; Linux's `AF_`, `SOCK_`,
`SO_`, `MSG_`, `POLL*` and `FIONBIO`/`FIONREAD` numbers), so the Linux layer
(M14) can pass most of them through. Results are Halcyon's negative `E_*`
codes. Sockets are handles (`H_SOCKET`) in the process's handle table:
`read()`, `write()` and `close()` work on them.

| # | Call | Notes |
|---|------|-------|
| 100 | `socket(domain, type, protocol)` | `type` may carry `SOCK_NONBLOCK` (`SOCK_CLOEXEC` ignored) |
| 101 | `bind(fd, addr, len)` | |
| 102 | `listen(fd, backlog)` | backlog clamped to 1..16 |
| 103 | `accept4(fd, addr, &len, flags)` | `accept()` is flags 0 |
| 104 | `connect(fd, addr, len)` | |
| 105 | `sendto(fd, buf, len, flags, addr, alen)` | `send()` without an address |
| 106 | `recvfrom(fd, buf, len, flags, addr, &alen)` | `recv()` without an address |
| 107 | `shutdown(fd, how)` | |
| 108 | `getsockname(fd, addr, &len)` | addresses are truncated to `len`; the real length is returned |
| 109 | `getpeername(fd, addr, &len)` | |
| 110 | `socketpair(domain, type, protocol, sv)` | |
| 111 | `setsockopt(fd, level, name, val, len)` | |
| 112 | `getsockopt(fd, level, name, val, &len)` | |
| 113 | `ioctlsocket(fd, cmd, &arg)` | `FIONBIO`, `FIONREAD` (Winsock's name; `arg` is an `unsigned long`) |
| 114 | `poll(fds, nfds, timeout_ms)` | |
| 115 | `sockstat(&st)` | socket use and limits, system-wide and of the caller |

116-139 are free (name resolution, interfaces, `sendmsg`/`recvmsg` later).

- **Flags:** `MSG_DONTWAIT`, `MSG_PEEK`, `MSG_WAITALL` (streams), `MSG_TRUNC`
  (datagrams: return the real length), `MSG_NOSIGNAL` (accepted; there are no
  signals). Unknown flags are `E_NOTSUP`.
- **Options (`SOL_SOCKET`):** `SO_RCVBUF` / `SO_SNDBUF` (clamped to 2 KiB ..
  256 KiB), `SO_RCVTIMEO` / `SO_SNDTIMEO` (a `struct timeval`, or an `int` of
  milliseconds, chosen by the option length), `SO_ERROR` (an `E_*` code, then
  cleared), `SO_TYPE`, `SO_DOMAIN`, `SO_PROTOCOL`, `SO_ACCEPTCONN`,
  `SO_REUSEADDR` (stored for TCP). Others go to the family, or `E_NOTSUP`.
- **Waiting:** blocking calls wait; `E_AGAIN` means the call would wait (a
  non-blocking socket, `MSG_DONTWAIT`) or the socket's timeout passed (as BSD's
  `EAGAIN`). A blocking stream `send` sends everything, unless its timeout
  passes (then it returns what it sent). `recv` and `accept` use `SO_RCVTIMEO`,
  `send` and `connect` `SO_SNDTIMEO`. Every wait ends with `E_CANCELED` when the
  program is being ended.
- **New errors:** `E_ADDRINUSE`, `E_NOTCONN`, `E_ISCONN`, `E_CONNREFUSED`,
  `E_CONNRESET`, `E_PIPE`, `E_AFNOSUPPORT`, `E_MSGSIZE`, `E_NOTSOCK`,
  `E_INPROGRESS` (-20 .. -29).

### Local sockets (AF_LOCAL)

- **Names:** one flat namespace in kernel memory (no files), shared by stream
  and datagram sockets. A name is 1 to 63 characters from `A-Z a-z 0-9 . - _`,
  starts with a letter or digit, and is compared without regard to case
  (`Halcyon.Clipboard` is `halcyon.clipboard`). The convention is
  `vendor.program[.service]`; `halcyon.*` is for the programs that come with
  Halcyon (a convention only, until programs have permissions). A name is held
  by the socket bound to it until that socket is closed or its program ends:
  there are no stale names to clean up. `bind()` with an empty name, and the
  first send of an unnamed datagram socket, take an automatic name `~xxxxxx`,
  which programs cannot choose. The address is `struct sockaddr_local {
  sa_family_t sl_family; char sl_name[64]; }`, length `2 + strlen + 1` (the NUL
  is optional); an unnamed socket's address is just the family (length 2).
- **Streams:** `listen` needs a name. `connect` puts a new socket in the
  listener's backlog at once (the client can send before it is accepted); with
  the backlog full it waits (or `E_AGAIN`). Data is written straight into the
  peer's receive buffer, whose size (the receiver's `SO_RCVBUF`, 64 KiB by
  default) is the back-pressure. A peer that closes, or whose program ends or
  crashes, gives end of stream (0) after the remaining data, preceded once by
  `E_CONNRESET` if data was lost (the closing side had unread data, or it was
  never accepted); sending to it is `E_PIPE`, and `poll` reports `POLLHUP`.
  `shutdown(SHUT_WR)` gives the peer end of stream; `SHUT_RD` discards unread
  data and makes the peer's sends `E_PIPE`. An accepted socket's name is the
  listener's; its peer name is the client's name, if it had one.
- **Datagrams:** at most 64 KiB, queued whole at the receiver (at most 128
  messages and `SO_RCVBUF` bytes; the sender waits or gets `E_AGAIN`), with the
  sender's name for `recvfrom`. `connect()` sets the default destination and,
  as on BSD, a connected socket takes messages from its peer only (others get
  `E_PERM`); `AF_UNSPEC` dissolves the association. When the connected peer
  goes away: `recv` returns 0 once the queue is empty, `send` is
  `E_CONNREFUSED`, `poll` shows `POLLHUP`. No such name: `E_CONNREFUSED`.
- **Who can read what:** data reaches a program only through a socket it
  owns: a stream it connected or accepted, or a datagram socket it bound (where
  anyone may send, unless it is connected). Handles are per process and cannot
  be passed yet.

### poll

`poll(fds, nfds, timeout_ms)` with at most 128 entries. An entry's `fd` is a
socket (`POLLIN` data, end of stream or a connection to accept; `POLLOUT` a
send would not wait (or would fail at once); `POLLERR` a pending error;
`POLLHUP` the peer is gone; `POLLRDHUP`), a file (always readable and
writable), 1 or 2 (console output: writable), `FD_WINDOWS` (the program's
window events: `POLLIN` when `win_get_event()` has an event or a window timer
is due; the wait also ends at the next window timer), or negative (ignored).
Anything else is `POLLNVAL`, including fd 0: console input readiness is not
supported (`con->read` blocks for a whole line and has no "ready" test).

The window state comes from one function in `gui/usergui.c`,
`usergui_poll_ready(p, &next_timer_ms)` (weak in `net/socket.c`, so the
socket code works without the GUI).

### How waiting works

The kernel is one big lock (DESIGN-M4), so a family's state cannot change
while one of its operations runs; only `sock_wait()` blocks, and only the
generic layer calls it. Every socket has a generation counter, bumped by
`sock_wake(s)` on every change. A wait records the counter of the socket and
of the one other socket its answer depended on (`s->watch`: the peer of a
stream, the target of a full datagram queue, the listener of a full backlog,
set by the family) and calls `proc_wait()` with a condition that only compares
the counters: cheap enough for the scheduler, which evaluates it with
interrupts off. `poll` does the same for all its sockets (and the window
queue), then asks each family again. Waiters hold references, so a socket
closed meanwhile (or by another thread, once programs have them) is freed only
after they wake.

### Limits and accounting

| Limit | Value |
|-------|-------|
| sockets, system-wide | 512 |
| sockets per program (including connections waiting in its backlogs) | 128 (and the 64-entry handle table) |
| receive buffer per socket (`SO_RCVBUF`) | 64 KiB default, 2 KiB .. 256 KiB |
| bytes in socket buffers per program | 4 MiB |
| bytes in socket buffers, system-wide | 32 MiB, and 32 MiB of kernel heap always left free |
| listen backlog | 16 |
| datagrams queued per socket | 128 |
| datagram size | 64 KiB |
| poll entries | 128 |

Buffer memory (4000-byte chunks for streams, one buffer per datagram,
including its overhead) is charged to the program that owns the receiving
socket when it is allocated, and given back when it is read or the socket
closes; at a limit a send fails with `E_NOMEM` (back-pressure itself never
uses memory beyond the receive buffer sizes). All socket memory of a program is
released when it ends (`socket_release_all`, from `proc_finish`), which also
tells every peer; `sockstat()` shows that it really was. Every user pointer
goes through `copyin`/`copyout`; results are checked writable before anything
is taken from a queue, so a bad pointer never loses data.

### The address-family layer (for milestone 6 and the Linux layer)

`kernel/net/socket.h` separates the generic socket from its family. The
generic layer owns handles, the system calls, every wait (blocking,
`SOCK_NONBLOCK`, timeouts, being ended), `poll`, socket options, limits and
accounting, and socket buffers (`nqueue_t`). A family is a `net_family_t`
entry in the `families[]` table in `socket.c` plus a `sock_ops_t`. What an
internet family (AF_INET / AF_INET6 with TCP and UDP) has to provide:

- `create(s, type, protocol)`: accept `SOCK_STREAM` (TCP) and `SOCK_DGRAM`
  (UDP) with protocol 0 or `IPPROTO_*`, allocate `s->priv`, set `s->ops`;
  `E_NOTSUP` otherwise. `pair` stays NULL (no `socketpair` for internet).
- Non-blocking operations only: return `E_AGAIN` when the caller would wait,
  having made sure that whatever ends the wait calls `sock_wake()` on the
  socket (or on `s->watch`). `connect` on TCP returns `E_INPROGRESS`; the
  generic layer then waits for `poll()` to show `POLLOUT` (connected) or
  `POLLERR` / `POLLHUP` with `s->error` set (refused, timed out: `E_TIMEOUT`).
- Address parsing (`sockaddr_in` / `sockaddr_in6`, same layout as Linux),
  ports (`bind`, ephemeral ports on `connect` / first `sendto`, `E_ADDRINUSE`
  unless `SO_REUSEADDR`), `getname`, `shutdown`, `poll()` masks, `FIONREAD`,
  and its own options (`IPPROTO_TCP` `TCP_NODELAY`, ...) through
  `setopt`/`getopt`. `SO_RCVBUF`/`SO_SNDBUF` are in `s->rcvbuf`/`s->sndbuf`
  (TCP's windows and send queue).
- Data through `uio_t` (user or kernel buffers) and memory through `nq_*`
  (receive queues; UDP datagrams carry the sender's address like local ones)
  or `net_charge()`/`net_uncharge()` for its own buffers (TCP send and
  out-of-order queues), so the limits above keep holding.
- Work outside system calls (packets from a network card, TCP timers) runs in
  a kernel thread under the same big lock and ends with `sock_wake()`; nothing
  there may block while holding a half-updated socket.
- `release()`: tear down completely (send FIN or RST, free the port, free
  buffers); no other operation is called after it. TCP's TIME_WAIT state lives
  on in the family, not in the `sock_t`.

New system calls for the internet (name resolution, interface list) take
numbers from 116-139. The Linux layer maps its socket calls onto the same
operations (it needs `sendmsg`/`recvmsg`, file-descriptor passing for AF_UNIX,
and a path namespace on top of the local names), and its `E_*` to `errno`
mapping is one table.

### Not done

- Passing handles over local sockets (`SCM_RIGHTS`), peer credentials, and
  `SOCK_SEQPACKET`.
- `select()` (poll only), console input readiness in `poll`.
- No permissions: any program may bind any free name or connect anywhere
  (as everything else in Halcyon today, see DESIGN-M4).
