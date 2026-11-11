/* Halcyon OS - random numbers for the kernel and programs (TCP sequence
 * numbers, DNS ids and ports, TLS keys).
 *
 * A ChaCha20 generator with "fast key erasure": every request runs ChaCha20
 * with the current key and immediately replaces the key with the first 32
 * bytes of output, so earlier output cannot be recovered from the state. The
 * key is seeded from RDSEED / RDRAND when the CPU has them, plus TSC jitter,
 * and keeps absorbing entropy (random_add: interrupt timings, packet
 * arrival times) by hashing it into the key. */
#include "kernel.h"
#include "lib.h"
#include "io.h"
#include "irq.h"

static u32 key[8];
static u64 nonce;
static int seeded;

#define ROTL(a, b) (((a) << (b)) | ((a) >> (32 - (b))))
#define QR(a, b, c, d) (a += b, d ^= a, d = ROTL(d, 16), c += d, b ^= c, b = ROTL(b, 12), \
                        a += b, d ^= a, d = ROTL(d, 8), c += d, b ^= c, b = ROTL(b, 7))

static void chacha_block(const u32 k[8], u64 n, u64 ctr, u32 out[16]) {
    u32 x[16] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
                  k[0], k[1], k[2], k[3], k[4], k[5], k[6], k[7],
                  (u32)ctr, (u32)(ctr >> 32), (u32)n, (u32)(n >> 32) };
    u32 in[16];
    memcpy(in, x, sizeof x);
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]); QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]); QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) out[i] = x[i] + in[i];
}

static int cpu_rdseed(u64 *v) {
    u8 ok;
    __asm__ volatile("rdseed %0; setc %1" : "=r"(*v), "=qm"(ok));
    return ok;
}
static int cpu_rdrand(u64 *v) {
    u8 ok;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok));
    return ok;
}

/* mix 32 bytes of new material into the key (ChaCha20 as a compression step) */
static void mix(const u32 m[8]) {
    u32 k2[8], out[16];
    for (int i = 0; i < 8; i++) k2[i] = key[i] ^ m[i];
    chacha_block(k2, ~0ull, nonce++, out);
    memcpy(key, out, sizeof key);
    memset(out, 0, sizeof out);
}

/* events (interrupt times) are only XORed into a pool here, so this is safe
 * in an interrupt handler; random_bytes() folds the pool into the key */
static volatile u32 pool[8];
static volatile u32 pool_n;
void random_add(u64 v) {
    u32 i = pool_n++;
    pool[i % 8] ^= (u32)v ^ (u32)(v >> 32) ^ ((u32)rdtsc() << (i % 13));
}

void random_init(void) {
    u32 c1, c2, c3, c4, ecx7 = 0, ebx7 = 0;
    __asm__ volatile("cpuid" : "=a"(c1), "=b"(c2), "=c"(c3), "=d"(c4) : "a"(1), "c"(0));
    int has_rdrand = (c3 >> 30) & 1;
    __asm__ volatile("cpuid" : "=a"(c1), "=b"(ebx7), "=c"(ecx7), "=d"(c4) : "a"(7), "c"(0));
    int has_rdseed = (ebx7 >> 18) & 1;
    int hw = 0;
    for (int round = 0; round < 4; round++) {
        u32 m[8];
        for (int i = 0; i < 8; i += 2) {
            u64 v = 0;
            int ok = 0;
            for (int t = 0; t < 10 && !ok; t++) ok = has_rdseed ? cpu_rdseed(&v) : 0;
            for (int t = 0; t < 10 && !ok; t++) ok = has_rdrand ? cpu_rdrand(&v) : 0;
            hw += ok;
            /* TSC jitter: the low bits of short timing loops */
            u64 j = 0;
            for (int s = 0; s < 64; s++) { u64 t0 = rdtsc(); for (volatile int w = 0; w < (int)(t0 & 31); w++) {} j = (j << 1 | j >> 63) ^ (rdtsc() - t0); }
            v ^= j ^ rdtsc();
            m[i] = (u32)v; m[i + 1] = (u32)(v >> 32);
        }
        mix(m);
    }
    seeded = 1;
    kprintf("random: seeded (%s, TSC jitter)\n", hw ? (has_rdseed ? "RDSEED" : "RDRAND") : "no hardware generator");
}

void random_bytes(void *buf, size_t n) {
    if (!seeded) random_init();
    if (pool_n >= 64) {
        u32 m[8];
        u64 f = irq_save();
        for (int i = 0; i < 8; i++) { m[i] = pool[i]; pool[i] = 0; }
        pool_n = 0;
        irq_restore(f);
        mix(m);
    }
    u8 *p = buf;
    u32 out[16];
    u64 n0 = nonce++;
    chacha_block(key, n0, 0, out);
    memcpy(key, out, 32);                     /* fast key erasure: next key first */
    memcpy(p, (u8 *)out + 32, MIN(n, (size_t)32));
    size_t done = MIN(n, (size_t)32);
    for (u64 ctr = 1; done < n; ctr++) {
        chacha_block(key, n0, ctr, out);
        size_t c = MIN(n - done, sizeof out);
        memcpy(p + done, out, c);
        done += c;
    }
    if (n > 32) {                             /* a fresh key again after a long output */
        chacha_block(key, nonce++, 0, out);
        memcpy(key, out, 32);
    }
    memset(out, 0, sizeof out);
}

u32 random_u32(void) { u32 v; random_bytes(&v, sizeof v); return v; }
