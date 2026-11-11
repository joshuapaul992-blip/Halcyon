/* PING.EXE - send ICMP echo requests through a ping socket
 * (socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP)), as Windows' ping:
 *
 *   ping HOST [-n COUNT] [-l SIZE] [-w TIMEOUT_MS] [-t]
 */
#include "hal.h"

int main(int argc, char **argv) {
    const char *host = 0;
    int count = 4, size = 32, timeout = 4000, forever = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-l") && i + 1 < argc) size = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) timeout = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t")) forever = 1;
        else host = argv[i];
    }
    if (!host || count < 1 || size < 0 || size > 65500) {
        printf("Usage: ping host [-n count] [-l size] [-w timeout_ms] [-t]\n");
        return 1;
    }
    unsigned addr;
    int r = resolve(host, &addr, 1, 5000);
    if (r < 1) {
        printf("Ping request could not find host %s: %s\n", host, hal_strerror(r));
        return 1;
    }
    char ip[16];
    inet_ntoa_r(addr, ip);
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (fd < 0) { printf("ping: %s\n", hal_strerror(fd)); return 1; }
    sock_timeouts(fd, timeout, 0);
    unsigned char *out = malloc((size_t)size + 8), *in = malloc((size_t)size + 64);
    if (!strcmp(ip, host)) printf("\nPinging %s with %d bytes of data:\n", ip, size);
    else printf("\nPinging %s [%s] with %d bytes of data:\n", host, ip, size);
    int sent = 0, got = 0;
    long tmin = -1, tmax = 0, tsum = 0;
    for (int seq = 1; forever || seq <= count; seq++) {
        memset(out, 0, 8);
        out[0] = 8;
        out[6] = (unsigned char)(seq >> 8); out[7] = (unsigned char)seq;
        for (int i = 0; i < size; i++) out[8 + i] = (unsigned char)('a' + i % 23);
        struct sockaddr_in to;
        unsigned long long t0 = time_ms();
        long n = sendto(fd, out, (size_t)size + 8, 0, (struct sockaddr *)&to, inet_sockaddr(&to, addr, 0));
        sent++;
        if (n < 0) { printf("General failure: %s\n", hal_strerror((int)n)); sleep_ms(1000); continue; }
        for (;;) {
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            n = recvfrom(fd, in, (size_t)size + 64, 0, (struct sockaddr *)&from, &fl);
            long ms = (long)(time_ms() - t0);
            if (n == E_AGAIN) { printf("Request timed out.\n"); break; }
            if (n == E_HOSTUNREACH || n == E_NETUNREACH) { printf("Reply from %s: Destination host unreachable.\n", ip); break; }
            if (n < 0) { printf("General failure: %s\n", hal_strerror((int)n)); break; }
            if (n < 8 || in[0] != 0 || in[6] != out[6] || in[7] != out[7]) continue;   /* (an older reply) */
            got++;
            tsum += ms;
            if (tmin < 0 || ms < tmin) tmin = ms;
            if (ms > tmax) tmax = ms;
            char fip[16];
            printf("Reply from %s: bytes=%ld time%s%ldms\n", inet_ntoa_r(from.sin_addr.s_addr, fip), n - 8, ms ? "=" : "<", ms ? ms : 1);
            if (ms < 1000) sleep_ms((unsigned)(1000 - ms));
            break;
        }
    }
    printf("\nPing statistics for %s:\n    Packets: Sent = %d, Received = %d, Lost = %d (%d%% loss),\n", ip, sent, got,
           sent - got, sent ? (sent - got) * 100 / sent : 0);
    if (got) printf("Approximate round trip times in milli-seconds:\n    Minimum = %ldms, Maximum = %ldms, Average = %ldms\n",
                    tmin, tmax, tsum / got);
    close(fd);
    return got ? 0 : 1;
}
