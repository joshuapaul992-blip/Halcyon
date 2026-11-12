/* NSLOOKUP.EXE - ask a DNS server about a name, as Windows' nslookup (the
 * non-interactive form):
 *
 *   nslookup [-type=A|AAAA|CNAME|MX|NS|TXT|PTR|ANY] [-timeout=SECONDS] NAME [SERVER]
 *
 * It sends its own query over UDP (not resolve(), which only gives
 * addresses and caches them), to SERVER or the first DNS server the
 * network settings give. An address as NAME asks for its name (PTR). */
#include "hal.h"

enum { T_A = 1, T_NS = 2, T_CNAME = 5, T_SOA = 6, T_PTR = 12, T_MX = 15, T_TXT = 16, T_AAAA = 28, T_ANY = 255 };

static const struct { const char *name; int type; } types[] = {
    { "A", T_A }, { "AAAA", T_AAAA }, { "CNAME", T_CNAME }, { "MX", T_MX }, { "NS", T_NS },
    { "TXT", T_TXT }, { "PTR", T_PTR }, { "SOA", T_SOA }, { "ANY", T_ANY },
};

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int ieq(const char *a, const char *b) {
    while (*a && lower(*a) == lower(*b)) { a++; b++; }
    return !*a && !*b;
}

/* a dotted name into DNS labels at p; length, or -1 */
static int put_name(unsigned char *p, int room, const char *name) {
    int n = 0;
    const char *s = name;
    if (!*s || !strcmp(s, ".")) { if (room < 1) return -1; p[0] = 0; return 1; }
    while (*s) {
        const char *dot = strchr(s, '.');
        int len = dot ? (int)(dot - s) : (int)strlen(s);
        if (len < 1 || len > 63 || n + len + 2 > room) return -1;
        p[n++] = (unsigned char)len;
        memcpy(p + n, s, (size_t)len);
        n += len;
        s += len;
        if (*s == '.') s++;
    }
    p[n++] = 0;
    return n <= 255 ? n : -1;
}

/* read a (possibly compressed) name at off into out; the offset after it, or -1 */
static int get_name(const unsigned char *m, int len, int off, char *out, int outsz) {
    int after = -1, o = 0, jumps = 0;
    out[0] = 0;
    for (;;) {
        if (off >= len) return -1;
        int c = m[off];
        if ((c & 0xC0) == 0xC0) {
            if (off + 1 >= len || ++jumps > 16) return -1;
            if (after < 0) after = off + 2;
            off = (c & 0x3F) << 8 | m[off + 1];
            continue;
        }
        if (c & 0xC0) return -1;
        off++;
        if (!c) break;
        if (off + c > len || o + c + 2 > outsz) return -1;
        if (o) out[o++] = '.';
        for (int i = 0; i < c; i++) {
            unsigned char ch = m[off + i];
            out[o++] = ch > 32 && ch < 127 ? (char)ch : '?';
        }
        out[o] = 0;
        off += c;
    }
    if (!o) strcpy(out, ".");
    return after >= 0 ? after : off;
}

static const char *type_name(int t) {
    for (unsigned i = 0; i < sizeof types / sizeof types[0]; i++) if (types[i].type == t) return types[i].name;
    return "?";
}

static void print_aaaa(const unsigned char *p) {
    /* the longest run (2 or more) of zero groups becomes "::" */
    int best = -1, bestlen = 0;
    for (int i = 0; i < 8; ) {
        if (!p[2 * i] && !p[2 * i + 1]) {
            int j = i;
            while (j < 8 && !p[2 * j] && !p[2 * j + 1]) j++;
            if (j - i > bestlen && j - i > 1) { best = i; bestlen = j - i; }
            i = j;
        } else i++;
    }
    for (int i = 0; i < 8; i++) {
        if (i == best) { printf("::"); i += bestlen - 1; continue; }
        if (i && i != best + bestlen) printf(":");
        printf("%x", p[2 * i] << 8 | p[2 * i + 1]);
    }
}

/* one resource record's data, after "name  " */
static int print_rr(const unsigned char *m, int len, int type, int off, int rdlen) {
    char name[300];
    switch (type) {
    case T_A:
        if (rdlen != 4) return -1;
        printf("internet address = %u.%u.%u.%u\n", m[off], m[off + 1], m[off + 2], m[off + 3]);
        return 0;
    case T_AAAA:
        if (rdlen != 16) return -1;
        printf("AAAA IPv6 address = ");
        print_aaaa(m + off);
        printf("\n");
        return 0;
    case T_CNAME: case T_NS: case T_PTR:
        if (get_name(m, len, off, name, sizeof name) < 0) return -1;
        printf("%s = %s\n", type == T_CNAME ? "canonical name" : type == T_NS ? "nameserver" : "name", name);
        return 0;
    case T_MX:
        if (rdlen < 3 || get_name(m, len, off + 2, name, sizeof name) < 0) return -1;
        printf("MX preference = %d, mail exchanger = %s\n", m[off] << 8 | m[off + 1], name);
        return 0;
    case T_TXT: {
        printf("text =\n");
        int p = off;
        while (p < off + rdlen) {
            int l = m[p++];
            if (p + l > off + rdlen) return -1;
            printf("\n        \"");
            for (int i = 0; i < l; i++) printf("%c", m[p + i] >= 32 && m[p + i] < 127 ? m[p + i] : '?');
            printf("\"");
            p += l;
        }
        printf("\n");
        return 0;
    }
    case T_SOA: {
        char mail[300];
        int p = get_name(m, len, off, name, sizeof name);
        if (p < 0 || (p = get_name(m, len, p, mail, sizeof mail)) < 0 || p + 20 > off + rdlen) return -1;
        unsigned v[5];
        for (int i = 0; i < 5; i++) v[i] = (unsigned)m[p + 4 * i] << 24 | m[p + 4 * i + 1] << 16 | m[p + 4 * i + 2] << 8 | m[p + 4 * i + 3];
        printf("\n        primary name server = %s\n        responsible mail addr = %s\n        serial  = %u\n"
               "        refresh = %u\n        retry   = %u\n        expire  = %u\n        default TTL = %u\n",
               name, mail, v[0], v[1], v[2], v[3], v[4]);
        return 0;
    }
    }
    printf("type %d, %d bytes\n", type, rdlen);
    return 0;
}

static const char *rcode_text(int rc) {
    switch (rc) {
    case 1: return "Format error";
    case 2: return "Server failed";
    case 3: return "Non-existent domain";
    case 4: return "Not implemented";
    case 5: return "Query refused";
    }
    return "Unknown error";
}

static unsigned rand16(void) {
    unsigned short v = 0;
    if (getrandom(&v, sizeof v) != sizeof v) v = (unsigned short)time_ms();
    return v;
}

int main(int argc, char **argv) {
    const char *name = 0, *server = 0;
    int qtype = 0, timeout_s = 2;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if ((a[0] == '-' || a[0] == '/') && a[1]) {
            const char *eq = strchr(a, '=');
            if ((!strncmp(a + 1, "type=", 5) || !strncmp(a + 1, "querytype=", 10) || !strncmp(a + 1, "q=", 2)) && eq) {
                qtype = -1;
                for (unsigned k = 0; k < sizeof types / sizeof types[0]; k++)
                    if (ieq(eq + 1, types[k].name)) qtype = types[k].type;
                if (qtype < 0) { printf("*** Unknown query type: %s\n", eq + 1); return 1; }
            } else if (!strncmp(a + 1, "timeout=", 8) && eq) {
                timeout_s = atoi(eq + 1);
                if (timeout_s < 1) timeout_s = 1;
                if (timeout_s > 60) timeout_s = 60;
            } else {
                printf("Usage: nslookup [-type=A|AAAA|CNAME|MX|NS|TXT|PTR|SOA|ANY] [-timeout=seconds] name [server]\n");
                return a[1] == '?' ? 0 : 1;
            }
        } else if (!name) name = a;
        else if (!server) server = a;
    }
    if (!name) {
        printf("Usage: nslookup [-type=A|AAAA|CNAME|MX|NS|TXT|PTR|SOA|ANY] [-timeout=seconds] name [server]\n");
        return 1;
    }

    /* the server: as given, or the first one in use */
    unsigned srv = 0;
    if (server) {
        int r = resolve(server, &srv, 1, timeout_s * 1000);
        if (r < 1) { printf("*** Can't find address for server %s: %s\n", server, hal_strerror(r)); return 1; }
    } else {
        hal_ifinfo_t inf;
        for (int i = 0; !srv && net_ifinfo(i, &inf) == 0; i++) srv = inf.dns[0];
        if (!srv) {
            printf("*** No DNS server is set up: the network may be disconnected (see ipconfig /all).\n");
            return 1;
        }
    }
    char srvip[16];
    inet_ntoa_r(srv, srvip);
    char srvname[300] = "UnKnown";

    /* an address asks for its name */
    char qname[300];
    struct in_addr ia;
    int reverse = inet_aton(name, &ia);
    if (reverse) {
        const unsigned char *b = (const unsigned char *)&ia.s_addr;
        snprintf(qname, sizeof qname, "%u.%u.%u.%u.in-addr.arpa", b[3], b[2], b[1], b[0]);
        if (!qtype) qtype = T_PTR;
    } else {
        strncpy(qname, name, sizeof qname - 1);
        qname[sizeof qname - 1] = 0;
        size_t l = strlen(qname);
        if (l > 1 && qname[l - 1] == '.') qname[l - 1] = 0;
        if (!qtype) qtype = T_A;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { printf("*** %s\n", hal_strerror(fd)); return 1; }
    sock_timeouts(fd, timeout_s * 1000, 0);
    struct sockaddr_in to;
    socklen_t tl = inet_sockaddr(&to, srv, 53);
    if (connect(fd, (struct sockaddr *)&to, tl) < 0) { printf("*** Can't reach %s\n", srvip); close(fd); return 1; }

    /* the server's own name (a PTR query for its address), as nslookup shows */
    unsigned char q[512], m[4096];
    for (int pass = 0; pass < 2; pass++) {
        char qn[300];
        int qt = qtype;
        if (pass == 0) {
            const unsigned char *b = (const unsigned char *)&srv;
            snprintf(qn, sizeof qn, "%u.%u.%u.%u.in-addr.arpa", b[3], b[2], b[1], b[0]);
            qt = T_PTR;
        } else strcpy(qn, qname);
        unsigned id = rand16();
        memset(q, 0, 12);
        q[0] = (unsigned char)(id >> 8); q[1] = (unsigned char)id;
        q[2] = 0x01;                                /* recursion desired */
        q[5] = 1;                                   /* one question */
        int n = put_name(q + 12, (int)sizeof q - 16, qn);
        if (n < 0) { printf("*** %s is not a valid name\n", name); close(fd); return 1; }
        n += 12;
        q[n++] = 0; q[n++] = (unsigned char)qt; q[n++] = 0; q[n++] = 1;
        long got = -1;
        for (int tries = 0; tries < 2 && got < 0; tries++) {
            if (send(fd, q, (size_t)n, 0) < 0) break;
            for (;;) {
                got = recv(fd, m, sizeof m, 0);
                if (got < 0) break;
                if (got >= 12 && m[0] == q[0] && m[1] == q[1] && (m[2] & 0x80)) break;
                got = -1;                           /* (not our answer) */
            }
        }
        if (got < 0) {
            if (pass == 0) {
                printf("Server:  %s\nAddress:  %s\n\n", srvname, srvip);
                continue;
            }
            printf("DNS request timed out.\n    timeout was %d seconds.\n*** Request to %s timed-out\n", timeout_s, srvname);
            close(fd);
            return 1;
        }
        int len = (int)got;
        int rcode = m[3] & 0x0F, aa = (m[2] & 0x04) != 0, tc = (m[2] & 0x02) != 0;
        int qd = m[4] << 8 | m[5], an = m[6] << 8 | m[7], ns = m[8] << 8 | m[9];
        int off = 12;
        char tmp[300];
        for (int i = 0; i < qd && off >= 0; i++) {
            off = get_name(m, len, off, tmp, sizeof tmp);
            if (off >= 0) off += 4;
        }
        if (pass == 0) {
            if (!rcode && an && off >= 0) {
                for (int i = 0; i < an && off >= 0; i++) {
                    off = get_name(m, len, off, tmp, sizeof tmp);
                    if (off < 0 || off + 10 > len) break;
                    int t = m[off] << 8 | m[off + 1], rdl = m[off + 8] << 8 | m[off + 9];
                    off += 10;
                    if (t == T_PTR && off + rdl <= len && get_name(m, len, off, srvname, sizeof srvname) >= 0) break;
                    off += rdl;
                }
            }
            printf("Server:  %s\nAddress:  %s\n\n", srvname, srvip);
            continue;
        }
        if (off < 0) { printf("*** %s: the answer could not be read\n", srvname); close(fd); return 1; }
        if (rcode) {
            printf("*** %s can't find %s: %s\n", srvname, name, rcode_text(rcode));
            close(fd);
            return 1;
        }
        if (tc) printf("(The answer was truncated; showing what fit in one UDP message.)\n");
        if (!an) {
            /* NODATA: the name exists without records of this type */
            int shown = 0;
            for (int i = 0; i < ns && off >= 0 && off < len; i++) {
                char owner[300];
                off = get_name(m, len, off, owner, sizeof owner);
                if (off < 0 || off + 10 > len) break;
                int t = m[off] << 8 | m[off + 1], rdl = m[off + 8] << 8 | m[off + 9];
                off += 10;
                if (off + rdl > len) break;
                if (t == T_SOA) {
                    if (!shown++) printf("%s\n", aa ? "" : "Non-authoritative answer:");
                    printf("%s\n        ", owner);
                    print_rr(m, len, t, off, rdl);
                }
                off += rdl;
            }
            if (!shown) printf("*** No %s records available for %s\n", type_name(qtype), name);
            close(fd);
            return 0;
        }
        if (!aa) printf("Non-authoritative answer:\n");
        /* A and AAAA in Windows' layout: Name, Addresses, Aliases */
        if (qtype == T_A || qtype == T_AAAA) {
            char canon[300], aliases[8][300];
            int nal = 0, naddr = 0, ntotal = 0;
            strcpy(canon, qname);
            int o = off;
            for (int i = 0; i < an && o >= 0; i++) {
                char owner[300];
                o = get_name(m, len, o, owner, sizeof owner);
                if (o < 0 || o + 10 > len) break;
                int t = m[o] << 8 | m[o + 1], rdl = m[o + 8] << 8 | m[o + 9];
                o += 10;
                if (o + rdl > len) break;
                if (t == qtype) ntotal++;
                if (t == T_CNAME) {
                    if (nal < 8) strcpy(aliases[nal++], owner);
                    get_name(m, len, o, canon, sizeof canon);
                }
                o += rdl;
            }
            printf("Name:    %s\n", canon);
            o = off;
            for (int i = 0; i < an && o >= 0; i++) {
                char owner[300];
                o = get_name(m, len, o, owner, sizeof owner);
                if (o < 0 || o + 10 > len) break;
                int t = m[o] << 8 | m[o + 1], rdl = m[o + 8] << 8 | m[o + 9];
                o += 10;
                if (o + rdl > len) break;
                if (t == qtype) {
                    printf("%s", naddr ? "          " : ntotal > 1 ? "Addresses:  " : "Address:  ");
                    if (t == T_A) printf("%u.%u.%u.%u\n", m[o], m[o + 1], m[o + 2], m[o + 3]);
                    else { print_aaaa(m + o); printf("\n"); }
                    naddr++;
                }
                o += rdl;
            }
            for (int i = 0; i < nal; i++) printf("%s%s\n", i ? "          " : "Aliases:  ", aliases[i]);
        } else {
            for (int i = 0; i < an && off >= 0; i++) {
                char owner[300];
                off = get_name(m, len, off, owner, sizeof owner);
                if (off < 0 || off + 10 > len) break;
                int t = m[off] << 8 | m[off + 1], rdl = m[off + 8] << 8 | m[off + 9];
                off += 10;
                if (off + rdl > len) break;
                printf("%s\t", reverse && t == T_PTR ? name : owner);
                if (print_rr(m, len, t, off, rdl) < 0) printf("(a malformed %s record)\n", type_name(t));
                off += rdl;
            }
        }
    }
    close(fd);
    return 0;
}
