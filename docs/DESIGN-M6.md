# Milestone 6 design: the internet stack (IPv4)

Milestone 6 connects Halcyon to networks. The network card drivers and the
`net` kernel thread are described in `kernel/net/netdev.h`; the web library and
browser on top in [DESIGN-M6-WEB.md](DESIGN-M6-WEB.md). This document is about
the protocol stack between them: Ethernet, ARP, IPv4, ICMP, UDP, TCP, DHCP and
DNS, offered to programs as the `AF_INET` family of the socket layer
([DESIGN-M9.md](DESIGN-M9.md), "Sockets and poll").

## Structure

| File | What |
|------|------|
| `kernel/net/inet.h` | shared definitions: interfaces, packets, byte order, checksums, limits, counters |
| `kernel/net/inet.c` | the `AF_INET` family (`socket()`), addresses and ports, the hooks the net thread calls (`net_stack_init/input/poll/link`), the loopback queue, the packet-loss test hook, system calls 116-121 |
| `kernel/net/eth.c` | interfaces, Ethernet, ARP (cache, resolution, queued packets, gratuitous ARP, conflict defence), the routing table |
| `kernel/net/ip.c` | IPv4 input (checks, reassembly) and output (routing, fragmentation, DF, path MTU), ICMP |
| `kernel/net/udp.c` | UDP sockets, ping sockets, kernel endpoints (the resolver) and the DHCP client's sends |
| `kernel/net/tcp.c` | TCP |
| `kernel/net/dhcp.c` | the DHCP client, static configuration from the settings, the DNS server list |
| `kernel/net/dns.c` | the resolver |
| `kernel/net/socket.c` | (M9) the generic layer: handles, waits, poll, options, accounting; `nq_copy` / `nq_drop` were added for TCP's send queue |

Everything runs under the big kernel lock, either in the `net` thread (frames
from the drivers, timers) or in a program's system call (sending, reading,
configuration). Nothing in the stack blocks; the generic socket layer does
every wait, and `SYS_RESOLVE` waits (killably) for its query in `inet.c`.

Addresses are `u32` in network byte order everywhere (as in packets and in
`sockaddr_in`); ports are host order inside the stack.

## How a packet travels

**Receiving.** A driver's `poll()` hands each frame to `netdev_rx()`, which
calls `net_stack_input()`: the loss test hook may drop it, then `eth_input()`
checks the destination MAC (ours or broadcast; no multicast is joined) and
dispatches ARP or IPv4. `ip_input()` checks version, header length, total
length against the frame, the header checksum, martian sources (broadcast,
multicast, 127/8 or our own address from the wire) and that the datagram is
for us (an address of any interface, a broadcast, or, while an interface has
no address, UDP for DHCP). Fragments go to reassembly. Then ICMP, UDP or TCP;
anything else gets "protocol unreachable". Transport input checks its own
length and checksum (with the pseudo header), finds the socket and queues the
data (`nq_put_*`, charged to the socket's program), then `sock_wake()`.

**Sending.** A transport builds its header and payload into a buffer with
`HDRROOM` bytes free in front and calls `ip_output()`: route lookup (local
addresses go to the loopback), the path MTU, the IP header (TTL 64 or
`IP_TTL`, DF for TCP), fragmentation when needed and allowed, then
`eth_output()`: broadcast, or the ARP cache, or an ARP request with the packet
queued (8 per destination, 64 in all). `eth_send_frame()` hands the frame to
the driver; when its ring is full (`E_BUSY`) the frame waits in the
interface's transmit queue (256) and is retried on the next poll. Every
output pokes the net thread so that new timers are seen.

**Loopback.** Packets to 127/8 or to one of our own addresses are copied to
the loopback queue (4 MiB) and delivered by the next `net_stack_poll()`, never
recursively from inside a send. The loopback interface `lo` (127.0.0.1/8, MTU
16384) exists even without a network card; the net thread then runs for it
alone.

## Interfaces, ARP, routing

- One interface per network device (`eth0`, ...) plus `lo`. An interface has
  an address, mask and gateway (static or from DHCP), its DNS servers, an ARP
  cache and a transmit queue.
- **ARP** (RFC 826): 64 entries per interface. A miss sends a broadcast
  request and queues the packet; three requests one second apart, then the
  queued packets come back to their sockets as "host unreachable"
  (`E_HOSTUNREACH` for a TCP connect, a connected UDP or ping socket). An
  answer is trusted for 60 s; a stale entry is still used for 20 s while a
  unicast request refreshes it; unused entries are dropped after 5 minutes; a
  full cache replaces the least recently used resolved entry. Only replies
  for entries we have, and requests for our address, create or change
  entries (RFC 826's merge rule); senders with a multicast MAC or a
  non-unicast address are ignored. A new address is announced by gratuitous
  ARP (twice, a second apart). Another host using our address is logged,
  defended (RFC 5227, at most every 10 s) and, right after a DHCP lease, makes
  the client DECLINE it.
- **Routing**: a connected route per configured interface, a default route
  per gateway (lower metric for lower interface numbers), 127/8 on `lo`;
  longest prefix wins. `255.255.255.255` goes out of the first configured
  interface that is up; a subnet broadcast is a link broadcast. No multicast,
  no forwarding (Halcyon is a host).

## IPv4 and ICMP

- **Reassembly** (RFC 815): at most 16 datagrams at a time and 512 KiB of
  fragments, 64 fragments per datagram, 30 s to complete (then "time exceeded
  in reassembly" if the first fragment came). An exact duplicate fragment is
  ignored; any other overlap ends the datagram (RFC 5722's rule, applied to
  IPv4). Lengths are checked against 65535 and the last fragment.
- **Fragmentation**: datagrams larger than the path MTU are fragmented unless
  DF is set (TCP sets it; UDP and ping do not).
- **Path MTU** (RFC 1191): "fragmentation needed" lowers the path MTU of a
  destination (32 entries, 10 minutes, never below 576, plateaus when the
  router gives no MTU); TCP then resends with the smaller segment size.
- **ICMP**: echo requests are answered (not to broadcasts). Destination
  unreachable, time exceeded and parameter problem are checked (the quoted
  header must be ours, about a first fragment) and handed to the socket:
  TCP checks that the quoted sequence number is in flight (RFC 5927); a
  connect in progress fails with the error (`E_CONNREFUSED`, `E_HOSTUNREACH`,
  `E_NETUNREACH`), an established connection keeps it as a soft error,
  reported if it later times out; connected UDP sockets and ping sockets get
  it as their next result. Errors we send (port and protocol unreachable,
  reassembly timeout) are limited to 10 a second (bursts of 20), never about
  ICMP errors, broadcasts, non-first fragments or invalid sources. Redirects,
  source quench and timestamps are ignored.

## UDP and ping sockets

- `socket(AF_INET, SOCK_DGRAM, 0)` is UDP: datagrams up to 65507 bytes,
  queued whole at the receiver (at most 128 and `SO_RCVBUF` bytes, else
  dropped and counted), with the sender's `sockaddr_in`. `connect()` sets the
  default peer and filters what is received; `AF_UNSPEC` dissolves it. A
  connected socket gets `E_CONNREFUSED` after a port unreachable. Sending to
  a broadcast address needs `SO_BROADCAST` (`E_PERM` otherwise); broadcasts
  are received by every socket bound to the port. Checksums are always sent
  and checked when present.
- `socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP)` is a **ping socket** (as on
  Linux): the program sends an ICMP message that must be an echo request
  (type 8, code 0, at least 8 bytes); the kernel puts the socket's port in
  the identifier and computes the checksum. Echo replies with that identifier
  come back whole (header and data) with the sender's address. No raw sockets
  are needed for PING.
- Ports: `bind()` to port 0 or the first send takes a random ephemeral port
  (49152-65535, RFC 6056's first algorithm); `SO_REUSEADDR` on both sockets
  lets two share a port.

## TCP

**Connections.** A tcb per connection; a socket's tcb is its `priv`. When the
program closes the socket, the tcb finishes by itself (an orphan in
FIN_WAIT, LAST_ACK or TIME_WAIT) with its unsent data charged to the system
instead of the program. A listener keeps half-open connections (SYN_RCVD, no
socket yet) in its SYN queue; when the handshake completes they become
sockets, charged to the listener's program, in its accept queue. TIME_WAIT
lasts 2 MSL = 60 s (a new SYN with a higher sequence number may reuse the
pair); an orphan in FIN_WAIT_2 is dropped after 60 s.

**States** are RFC 9293's (`TCPS_*` in `sockabi.h`). Simultaneous open and
close, half-close (`shutdown(SHUT_WR)` sends FIN, the other direction keeps
working) and every RST case are covered by the host tests.

**Receiving** (RFC 9293 3.10.7): sequence acceptability against the window
we offered (an empty segment may sit at its right edge, as BSD and Linux
accept), trimming, then RST, SYN, ACK, data and FIN. In-order data goes to
the receive queue; out-of-order data is kept (64 segments, within the window,
charged to the program), and a duplicate ACK is sent at once. Our FIN, theirs
and TIME_WAIT follow the state diagram.

**Defences** (RFC 5961, 1337, 6528): a RST is accepted only at exactly
`rcv_nxt`; one elsewhere in the window, and any SYN in a synchronized state,
gets a challenge ACK (at most 5 a second per connection); an ACK for data
never sent or older than the window gets an ACK and is dropped; RST in
TIME_WAIT is ignored; replies to empty segments outside the window are
limited to 5 a second. Initial sequence numbers are a 4-microsecond clock
plus a SipHash of the address pair under a random key. ICMP errors must quote
a sequence number in flight.

**Timers** (all per connection, deadlines in milliseconds; `tcp_timer()` only
walks the connections when the earliest one is due):

| Timer | Value |
|-------|-------|
| retransmission | RFC 6298: SRTT/RTTVAR from one timed segment at a time (Karn: never a retransmitted one), RTO = SRTT + 4 RTTVAR, minimum 200 ms (as Linux; RFC 6298 suggests 1 s), maximum 60 s, initial 1 s, doubled on every timeout; the estimate restarts after 4 timeouts |
| giving up | SYN: 5 retransmissions (63 s, then `E_TIMEOUT`); SYN-ACK: 5; data: 12 (several minutes with backoff), then RST and `E_TIMEOUT` (or the ICMP soft error) |
| delayed ACK | every second segment is acknowledged at once, otherwise after 40 ms; out-of-order data, a filled hole, FIN and window updates at once |
| persist | zero window (or one too small to use) with nothing in flight: probes from the RTO backing off to 60 s, each an old byte the peer must answer; a small window is used when the timer fires (SWS override); 10 unanswered probes drop the connection |
| keepalive | `SO_KEEPALIVE`: after `TCP_KEEPIDLE` seconds idle (7200), probes every `TCP_KEEPINTVL` (75), `TCP_KEEPCNT` (9) unanswered ones end it with `E_TIMEOUT` |
| TIME_WAIT, FIN_WAIT_2 | 60 s |

**Sending.** Up to `SO_SNDBUF` bytes are queued (`nq_*`, charged to the
program); `tcp_output()` sends what the smaller of the peer's window and
`cwnd` allows, in segments of the MSS: full segments always, a short one only
when nothing is in flight (Nagle, unless `TCP_NODELAY`), when it is at least
half the largest window the peer offered, or when retransmitting. Pure ACKs
and RSTs carry `snd_max` (as BSD): after a timeout pulls `snd_nxt` back, an
ACK with that old number would be outside the peer's window (both ends doing
it made an ACK storm in the simulator).

**Congestion control** (RFC 5681, 6582, 6928, 3465): initial window 10
segments; slow start with byte counting (L = 2 segments, since ACKs come
every second segment); congestion avoidance adding one segment per window;
three duplicate ACKs (same ACK, no data, same window, something in flight)
start fast retransmit and NewReno recovery (partial ACKs retransmit the next
hole); a timeout sets `ssthresh` to half the flight, `cwnd` to one segment,
and goes back to `snd_una`; `cwnd` is reduced after an idle period longer
than the RTO.

**Windows.** We offer `SO_RCVBUF` minus what is queued (default 64 KiB, up to
256 KiB) and never shrink an offer; the right edge moves only in steps of at
least min(half the buffer, one MSS) (receiver SWS avoidance), and stays put
while there is a hole: BSD-derived senders such as QEMU's slirp count a
duplicate ACK only when the window is unchanged, and a window update in the
middle of a run of duplicates turned every loss into a timeout. Reading
sends a window update once the window has grown by two segments or half the
buffer. Window scaling (RFC 7323): we offer a shift of 3 (enough for 256
KiB) and scale both ways when the peer offers one too. The MSS we offer is
the interface MTU minus 40 (1460, loopback 16344); we use the smaller of the
peer's (536 when it gives none, never below 64) and the path's.

**Ports and options.** Ephemeral ports are random (49152-65535) and unique
per local port. `bind()` fails with `E_ADDRINUSE` while anything holds the
port (TIME_WAIT included) unless `SO_REUSEADDR`, which still never shares a
listener's port. `listen()` without `bind()` takes an ephemeral port. The
backlog (at most 16) bounds the accept queue; SYNs are dropped while it is
full, and the SYN queue holds at most max(8, 2 x backlog) (64) half-open
connections per listener and 256 in all. Options: `TCP_NODELAY`,
`TCP_MAXSEG` (read: the MSS in use), `TCP_KEEPIDLE/INTVL/CNT`,
`SO_KEEPALIVE`, `SO_LINGER` (`{1, 0}`: close sends RST), `SO_RCVBUF`,
`SO_SNDBUF`, `SO_REUSEADDR`, `IP_TTL`, `IP_TOS`; `FIONREAD`;
`getsockname` / `getpeername`.

**What programs see.** `connect()` blocks until the handshake completes, or
returns `E_INPROGRESS` on a non-blocking socket (then `POLLOUT`, or `POLLERR`
with `SO_ERROR`). Errors: `E_CONNREFUSED` (RST to our SYN), `E_TIMEOUT`,
`E_HOSTUNREACH` / `E_NETUNREACH`, `E_CONNRESET`. After a reset or timeout,
the first `recv` or `send` returns the error, later ones 0 (end of stream) and
`E_PIPE`. `recv` returns 0 after the peer's FIN. Closing with unread data,
or a connection still in a listener's accept queue, sends RST (RFC 2525).

## Configuration: DHCP and static

Settings (`C:\HALCYON\SETTINGS.INI`, read once `settings_load()` ran):

| Key | Meaning |
|-----|---------|
| `net.eth0.mode` | `dhcp` (default) or `static` |
| `net.eth0.ip`, `.mask`, `.gateway` | static configuration (mask by address class if missing; gateway optional, must be on the subnet) |
| `net.dns` | DNS servers for the whole system, comma-separated, up to 3 (`a.b.c.d[:port]`; the port is for tests); empty: the servers from DHCP |

The DHCP client (RFC 2131/2132) runs per interface: DISCOVER, the first
valid OFFER, REQUEST, ACK. Retransmissions back off from 2 s to 64 s with a
second of jitter. Replies must match the transaction id and our MAC, carry the
magic cookie and well-formed options; an ACK must be for the offer and from
the server we chose. The lease runs from when the REQUEST was sent: at T1
(half the lease) a unicast REQUEST renews it, at T2 (7/8) a broadcast one
rebinds, at expiry the address is dropped and DISCOVER starts again. A NAK
drops it at once. When the link comes back, INIT-REBOOT asks for the
address still leased; without an answer the lease is used while it lasts.
`NETCTL_RELEASE` sends RELEASE. The log shows the result:
`net: eth0: 10.0.2.15/24 via 10.0.2.2, DNS 10.0.2.3 (DHCP)`.

## The resolver

`SYS_RESOLVE` answers `localhost` (and `*.localhost`) and dotted quads at
once and checks the name (letters, digits, `-`, `_`; labels of 1-63, at most
253 characters). Otherwise it asks the configured servers for A records:
one server at a time, 1 s, then 2 s, then 4 s per attempt, going round the
servers until the caller's timeout (default 10 s, at most 60 s). Every
attempt has a new random id and a new random source port, and the last four
attempts' answers are still accepted. An answer counts only if it comes from
the server's address and port to that source port, with that id, `QR` set,
opcode 0 and the same question (RFC 5452); everything else is counted as
rejected. Names are read with compression pointers that must point
backwards (no loops), bounded in length and number. CNAME chains are
followed within an answer and, when the answer stops at a CNAME, with a new
query for its target, at most 8 links. NXDOMAIN and "no address" are
`E_NOENT`, cached for the SOA's negative TTL (at most 5 minutes); SERVFAIL /
REFUSED from every server is `E_IO`; no answer is `E_TIMEOUT`; no server is
`E_NODEV`. Answers are cached for their smallest TTL along the chain (at
most a day; 64 names, least recently used replaced). Concurrent lookups of a
name share one query. A truncated answer without addresses counts as a
failure (no TCP fallback).

## System calls

| # | Call | |
|---|------|-|
| 116 | `resolve(host, unsigned *ipv4_out, max, timeout_ms)` | addresses in network order; count (>= 1) or `E_NOENT`, `E_TIMEOUT`, `E_INVAL`, `E_IO`, `E_NODEV`; waits killably |
| 117 | `net_ifinfo(index, hal_ifinfo_t *)` | name, description, driver, MAC, MTU, link, speed, duplex, mode, DHCP state, server, lease and time left, address, mask, gateway, DNS servers, counters; `lo` is last; `E_NOENT` past the end |
| 118 | `net_ifconfig(const hal_ifconfig_t *)` | `NETCFG_ADDR`: DHCP or static (validated: unicast address, contiguous mask, gateway on the subnet); `NETCFG_DNS`: the DNS servers; `NETCFG_SAVE`: also into `SETTINGS.INI` |
| 119 | `net_ctl(op, ifname)` | `NETCTL_RENEW`, `NETCTL_RELEASE`, `NETCTL_DNS_FLUSH`, `NETCTL_ARP_FLUSH` |
| 120 | `net_stats(hal_netstats_t *)` | IP, ICMP, ARP, UDP, TCP and DNS counters |
| 121 | `net_conns(hal_netconn_t *, max)` | TCP connections and listeners, UDP and ping sockets: addresses, state, queued bytes, owning program |

122-127 are free. Structures and constants are in `kernel/net/sockabi.h`
(`sockaddr_in`, `IPPROTO_*`, `TCP_*`, `SO_KEEPALIVE`, `SO_LINGER`,
`SO_BROADCAST`, the `hal_*` structures); `hal.h` has the wrappers and
`htons`/`htonl`/`ntohs`/`ntohl`, `inet_aton`, `inet_addr`, `inet_ntoa(_r)`,
`inet_sockaddr`, and `inet_connect(host, port, timeout_ms)` (resolve and try
each address). New errors: `E_HOSTUNREACH` (-30), `E_NETUNREACH` (-31),
`E_ADDRNOTAVAIL` (-32). As everywhere in Halcyon there are no permissions: any
program may change the configuration (DESIGN-M4).

`PING.EXE` and `IPCONFIG.EXE` (`/all`, `/renew`, `/release`, `/flushdns`,
`/stats`, `/conns`) are small programs on these calls.

## Limits

| | |
|-|-|
| ARP | 64 entries per interface, 8 packets queued per resolution, 64 in all |
| transmit queue (ring full) | 256 frames per interface |
| loopback queue | 4 MiB |
| reassembly | 16 datagrams, 512 KiB, 64 fragments each, 30 s |
| path MTU cache | 32 destinations, 10 minutes, at least 576 |
| ICMP errors sent | 10 a second, bursts of 20 |
| UDP | 65507-byte datagrams, 128 queued per socket within `SO_RCVBUF` |
| TCP | send queue `SO_SNDBUF`, receive window `SO_RCVBUF` (64 KiB default, 2-256 KiB), 64 out-of-order segments, SYN queue max(8, 2 x backlog) up to 64 per listener and 256 in all, accept queue = backlog (<= 16), 256 orphans, 1024 TIME_WAIT (the oldest is recycled) |
| DNS | 64 cached names, 16 lookups in flight, 16 addresses per name, CNAME chains of 8 |
| sockets, buffer memory | the socket layer's (DESIGN-M9): 512 sockets, 128 per program, 4 MiB per program, 32 MiB in all |

Memory that belongs to a socket (receive queues, UDP datagrams, TCP send and
out-of-order queues) is charged to its program through `nq_*` and
`net_charge()`; an orphan's send queue is moved to the system's account. Stack
memory that belongs to no socket (ARP queues, fragments, the loopback and
transmit queues, tcbs) has the fixed bounds above.

## Every byte is hostile

Every length is checked against what was received before it is used
(Ethernet, ARP, IP header and total length, options, fragment offsets, UDP
and TCP lengths and data offsets, TCP options, DHCP options, DNS names,
records and rdata). Checksums are checked on every layer (not on the
loopback). Martian addresses are dropped. Nothing received can make the stack
allocate without a bound, loop without a bound (DNS pointers go backwards;
option walks always advance), or answer with more than it received
(no broadcast echo replies, rate-limited ICMP errors and challenge ACKs).

## Tests

- **Host simulator** (`tests/host/netsim.c`, run by `tests/host/run.sh`, about
  half a second): kernel/net/ is built with `net_shim.c` as a shared object
  and loaded once per simulated computer; they share a simulated Ethernet
  that can lose, duplicate, reorder, delay, corrupt and rate-limit frames and
  impose a smaller path MTU, with a "router" written independently in the
  simulator (ARP, ICMP echo, a DHCP server that can NAK, go away or offer an
  address in use, two DNS servers with a test zone and forged answers, ICMP
  "fragmentation needed"). Time is simulated, so minutes of timeouts take
  moments and every run is reproducible (`-s SEED`). 308 checks: ARP (cache
  bound, aging, failure), DHCP (lease, renew, rebind, expiry, link down/up,
  NAK, release, static, DECLINE), DNS (CNAMEs, NXDOMAIN and its caching, TTLs,
  the cache limit, a dead first server, timeouts, SERVFAIL, forged answers,
  compression loops), UDP (echo up to 65507 bytes, fragments duplicated and
  reordered, overlapping fragments, a flood of incomplete datagrams,
  port unreachable, broadcast, queue limits), TCP (transfers and names,
  TIME_WAIT, half-close, simultaneous open and close, RST cases including
  RFC 5961, backlog and SYN drops, keepalive, Nagle and delayed ACKs, zero
  window and probes, RTO backoff and giving up, path MTU, loopback, 50
  connections, window scaling over an 80 ms 5 MB/s path: 2.6 MB/s, where 64
  KiB windows allow 0.82), and transfers under 5% and 15% loss, duplication and
  reordering, corruption, and all at once. Every test ends with all sockets
  closed, TIME_WAIT passed and each stack's memory checked. `-f` adds 12
  randomized torture rounds (three connections both ways under random
  impairments). Fixed thanks to it: an ACK storm after go-back-N, the right
  edge acceptance, ARP aging without traffic, slow start with ACKs every
  second segment.
- **Fuzzer** (`tests/host/fuzz_net.c`, `make -C tests/host fuzz-net-run`):
  libFuzzer on the receive path of one stack with a listener, a connection
  being opened, UDP and ping sockets and a DNS query in flight (and DHCP on
  request). Records are frames as they are, frames with fixed checksums, or
  frames also made to fit (TCP sequence numbers, DHCP xid, DNS id / port /
  server), time passing, and the program acting. Seeds are frames the
  simulator sent; `fuzz-net-regress` (run by `run.sh`) replays them. No
  finding in about 6 million runs (15 minutes on two cores).
- **QEMU** (`tools/boottest.py`, variants `net` and `net+loss`, both in the
  default matrix; `net+virtio`, `net+e1000e`, ... on other adapters):
  NETTEST.EXE on QEMU's user network with servers on the host
  (`tools/netservers.py`): the DHCP configuration, ping, DNS against a test
  zone (`net.dns=10.0.2.2:PORT` in SETTINGS.INI), a 32 MiB download and an 8
  MiB upload with CRC-32 checks, 40 parallel connections, UDP echo up to 8000
  bytes, the loopback, error cases, and a server in Halcyon that the host
  reaches through `hostfwd`. With e1000: 34-38 MB/s download, 21-24 MB/s
  upload (e1000e 50 / 25, virtio-net 37 / 23, RTL8139 45 / 24; QEMU without
  KVM, slirp on the host side). With 3% of frames dropped on receive and
  transmit (`opt/halcyon/netloss`), the run still passes with 40-odd
  retransmissions; the download direction is slow there because slirp's own
  TCP (4.4BSD Reno, no SACK) falls back to timeouts on two losses in a window.

## Not done

- IPv6 (below), multicast (IGMP), raw sockets, IP options (received ones are
  skipped), forwarding.
- TCP: SACK (RFC 2018), timestamps and PAWS (RFC 7323 beyond window scaling),
  ECN, TCP Fast Open, urgent data (it is ordinary data), `SO_LINGER` with a
  time, `MSG_OOB`, `TCP_INFO`; listen backlogs above 16 (the socket layer's
  limit).
- DHCP: options in the sname/file fields (overload), static routes, DHCP
  INFORM; DECLINE relies on seeing the other host's ARP (no ARP probe before
  using an address).
- DNS: TCP fallback for truncated answers, AAAA, search domains, DNSSEC,
  0x20 case randomization.
- `sendmsg`/`recvmsg`, `IP_RECVERR`-style error queues, `SO_BINDTODEVICE`.

## IPv6 notes

What IPv6 would need, with the places it fits:

- an `AF_INET6` family beside `inet_family`, `sockaddr_in6`, and dual-stack
  sockets (`IPV6_V6ONLY`); TCP and UDP take their addresses as 128-bit values
  (the tcb and the pcbs grow an address family; the TCP hash and checksums
  already go through helpers);
- `ip6.c`: the fixed header, extension headers (hop-by-hop, routing type 0
  refused, fragment, destination options) with strict limits, reassembly with
  RFC 5722's overlap rule (as here), no fragmentation by routers so path MTU
  discovery is mandatory (1280 minimum);
- ICMPv6 with Neighbor Discovery instead of ARP (neighbor cache states,
  solicited-node multicast, which needs `set_multicast` from the drivers and
  MLD), router solicitation / advertisement, SLAAC with stable private
  addresses (RFC 7217) and temporary addresses (RFC 8981), duplicate address
  detection;
- DHCPv6 (stateless for DNS at least) or RDNSS in router advertisements; AAAA
  queries in the resolver (and `resolve6`), address selection (RFC 6724),
  Happy Eyeballs in `inet_connect`;
- per-interface link-local addresses with scope ids, and the route table
  keyed by prefix length up to 128.
