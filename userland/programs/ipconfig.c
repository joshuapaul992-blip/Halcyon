/* IPCONFIG.EXE - the network interfaces and their configuration, as
 * Windows' ipconfig:
 *
 *   ipconfig [/all] [/renew [if]] [/release [if]] [/flushdns] [/stats] [/conns]
 */
#include "hal.h"

static const char *ip(unsigned a, char *b) { return inet_ntoa_r(a, b); }

static const char *dhcp_state(int s) {
    static const char *n[] = { "off", "init", "selecting", "requesting", "bound", "renewing", "rebinding", "rebooting" };
    return s >= 0 && s < 8 ? n[s] : "?";
}

static void show(const hal_ifinfo_t *f, int all) {
    char b1[16], b2[16], b3[16];
    printf("\n%s adapter %s:\n\n", f->loopback ? "Loopback" : "Ethernet", f->name);
    if (!f->loopback && !f->link_up) {
        printf("   Media State . . . . . . . . . . . : Media disconnected\n");
        if (!all) return;
    }
    if (all) {
        printf("   Description . . . . . . . . . . . : %s\n", f->desc);
        if (!f->loopback) {
            printf("   Physical Address. . . . . . . . . : %02X-%02X-%02X-%02X-%02X-%02X\n", f->mac[0], f->mac[1], f->mac[2],
                   f->mac[3], f->mac[4], f->mac[5]);
            printf("   Driver, Speed . . . . . . . . . . : %s, %d Mbit/s%s\n", f->driver, f->speed_mbps,
                   f->full_duplex ? " full duplex" : "");
            printf("   DHCP Enabled. . . . . . . . . . . : %s\n", f->mode == NET_MODE_DHCP ? "Yes" : "No");
        }
        printf("   MTU . . . . . . . . . . . . . . . : %u\n", f->mtu);
    }
    if (f->addr) {
        printf("   IPv4 Address. . . . . . . . . . . : %s%s\n", ip(f->addr, b1), f->conflict ? " (conflict)" : "");
        printf("   Subnet Mask . . . . . . . . . . . : %s\n", ip(f->mask, b2));
        printf("   Default Gateway . . . . . . . . . : %s\n", f->gateway ? ip(f->gateway, b3) : "");
    } else if (!f->loopback) {
        printf("   IPv4 Address. . . . . . . . . . . : (none%s%s)\n", f->mode == NET_MODE_DHCP ? ", DHCP " : "",
               f->mode == NET_MODE_DHCP ? dhcp_state(f->dhcp_state) : "");
    }
    if (all && !f->loopback) {
        if (f->mode == NET_MODE_DHCP) {
            printf("   DHCP Server . . . . . . . . . . . : %s (%s)\n", f->dhcp_server ? ip(f->dhcp_server, b1) : "", dhcp_state(f->dhcp_state));
            if (f->lease_s >= 0)
                printf("   Lease . . . . . . . . . . . . . . : %lld s, %lld s left\n", f->lease_s, f->lease_left_s);
        }
        printf("   DNS Servers . . . . . . . . . . . :");
        for (int i = 0; i < 3 && f->dns[i]; i++) printf(" %s", ip(f->dns[i], b1));
        printf("\n");
        printf("   Received / Sent . . . . . . . . . : %llu / %llu packets, %llu / %llu bytes\n", f->rx_packets,
               f->tx_packets, f->rx_bytes, f->tx_bytes);
        printf("   Errors, Dropped . . . . . . . . . : %llu, %llu received; %llu, %llu sent\n", f->rx_errors, f->rx_dropped,
               f->tx_errors, f->tx_dropped);
    }
}

static void stats(void) {
    hal_netstats_t s;
    if (net_stats(&s)) return;
    printf("\nIP:   %llu in, %llu out, %llu bad, %llu fragments in (%llu reassembled, %llu failed), %llu out, %llu no route\n",
           s.ip_in, s.ip_out, s.ip_bad, s.ip_frag_in, s.ip_reasm_ok, s.ip_reasm_fail, s.ip_frag_out, s.ip_noroute);
    printf("ICMP: %llu in, %llu out, %llu errors in, %llu errors out (%llu rate-limited)\n", s.icmp_in, s.icmp_out,
           s.icmp_err_in, s.icmp_err_out, s.icmp_ratelimited);
    printf("ARP:  %llu in, %llu out, %llu failed, %d entries\n", s.arp_in, s.arp_out, s.arp_fail, s.arp_entries);
    printf("UDP:  %llu in, %llu out, %llu to closed ports, %llu dropped; %d sockets\n", s.udp_in, s.udp_out, s.udp_noport,
           s.udp_drop, s.udp_socks);
    printf("TCP:  %llu in, %llu out, %llu bad, %llu retransmitted (%llu fast), %llu timeouts, RST %llu in / %llu out\n",
           s.tcp_in, s.tcp_out, s.tcp_bad, s.tcp_rexmt, s.tcp_fast_rexmt, s.tcp_timeouts, s.tcp_rst_in, s.tcp_rst_out);
    printf("      %llu active, %llu passive opens, %llu established; %d connections now\n", s.tcp_active, s.tcp_passive,
           s.tcp_established, s.tcp_conns);
    printf("DNS:  %llu queries, %llu cache hits, %llu answers rejected, %d names cached\n", s.dns_queries, s.dns_hits,
           s.dns_rejected, s.dns_cached);
}

static void conns(void) {
    static hal_netconn_t c[256];
    static const char *st[] = { "CLOSED", "LISTENING", "SYN_SENT", "SYN_RECEIVED", "ESTABLISHED", "FIN_WAIT_1", "FIN_WAIT_2",
                                "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT" };
    int n = net_conns(c, 256);
    printf("\n  Proto  Local Address          Foreign Address        State          PID\n");
    for (int i = 0; i < n; i++) {
        char l[32], r[32], b[16];
        snprintf(l, sizeof l, "%s:%u", ip(c[i].laddr, b), c[i].lport);
        if (c[i].raddr || c[i].rport) snprintf(r, sizeof r, "%s:%u", ip(c[i].raddr, b), c[i].rport);
        else strcpy(r, "*:*");
        const char *p = c[i].proto == NETC_TCP ? "TCP" : c[i].proto == NETC_UDP ? "UDP" : "ICMP";
        printf("  %-6s %-22s %-22s %-14s %u\n", p, l, r, c[i].proto == NETC_TCP && c[i].state <= 10 ? st[c[i].state] : "", c[i].pid);
    }
}

int main(int argc, char **argv) {
    int all = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *ifn = i + 1 < argc && argv[i + 1][0] != '/' ? argv[i + 1] : 0;
        if (!strcmp(a, "/all")) all = 1;
        else if (!strcmp(a, "/renew") || !strcmp(a, "/release")) {
            int op = a[2] == 'e' && a[3] == 'n' ? NETCTL_RENEW : NETCTL_RELEASE;
            int r = net_ctl(op, ifn);
            if (r) { printf("ipconfig: %s: %s\n", a + 1, hal_strerror(r)); return 1; }
            if (op == NETCTL_RENEW) {                   /* wait a little for the answer */
                for (int k = 0; k < 50; k++) {
                    hal_ifinfo_t f;
                    if (!net_ifinfo(0, &f) && f.addr && f.dhcp_state == NET_DHCP_BOUND) break;
                    sleep_ms(100);
                }
            }
            if (ifn) i++;
        } else if (!strcmp(a, "/flushdns")) {
            net_ctl(NETCTL_DNS_FLUSH, 0);
            printf("\nSuccessfully flushed the DNS Resolver Cache.\n");
            return 0;
        } else if (!strcmp(a, "/stats")) { stats(); return 0; }
        else if (!strcmp(a, "/conns")) { conns(); return 0; }
        else { printf("Usage: ipconfig [/all] [/renew [if]] [/release [if]] [/flushdns] [/stats] [/conns]\n"); return 1; }
    }
    printf("\nHalcyon IP Configuration\n");
    hal_ifinfo_t f;
    for (int i = 0; !net_ifinfo(i, &f); i++) show(&f, all);
    return 0;
}
