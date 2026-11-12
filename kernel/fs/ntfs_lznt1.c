/* Halcyon OS - NTFS: LZNT1 decompression (compressed files).
 *
 * Written from the format description in Microsoft's [MS-XCA] (2.5,
 * "LZNT1 Algorithm Details"): a compression unit is a sequence of chunks,
 * each with a 16-bit header (bits 0-11: data bytes - 1, bit 15: compressed,
 * header 0: the end). A compressed chunk decodes to at most 4096 bytes:
 * a flag byte, then eight items, each a literal byte (flag bit 0) or a
 * 16-bit back reference (flag bit 1) whose split between offset and length
 * depends on how much of the chunk is already decoded. An uncompressed
 * chunk is 4096 raw bytes. */
#include "core/types.h"
#include "core/lib.h"
#include "fs/ntfs_priv.h"

#define CHUNK 4096u

/* decode in[0..inlen) into out[0..outlen); returns the bytes produced (the
 * caller zero-fills the rest), or -1 if the data is corrupt */
int nt_lznt1_decompress(const u8 *in, u32 inlen, u8 *out, u32 outlen) {
    u32 ip = 0, op = 0;
    while (ip + 2 <= inlen && op < outlen) {
        u16 h = (u16)(in[ip] | (in[ip + 1] << 8));
        ip += 2;
        if (!h) break;
        u32 clen = (h & 0x0FFFu) + 1;
        if (clen > inlen - ip) return -1;
        const u8 *c = in + ip, *ce = in + ip + clen;
        ip += clen;
        u32 cstart = op, cmax = MIN(outlen, op + CHUNK);
        if (!(h & 0x8000)) {
            /* stored */
            u32 n = MIN(clen, cmax - op);
            memcpy(out + op, c, n);
            op += n;
            continue;
        }
        while (c < ce && op < cmax) {
            u8 flags = *c++;
            for (int bit = 0; bit < 8 && c < ce && op < cmax; bit++, flags >>= 1) {
                if (!(flags & 1)) {
                    out[op++] = *c++;
                    continue;
                }
                if (ce - c < 2) return -1;
                u16 t = (u16)(c[0] | (c[1] << 8));
                c += 2;
                u32 pos = op - cstart, lbits = 12;
                if (!pos) return -1;
                for (u32 i = pos - 1; i >= 0x10; i >>= 1) lbits--;
                u32 off = ((u32)t >> lbits) + 1;
                u32 len = ((u32)t & ((1u << lbits) - 1)) + 3;
                if (off > pos) return -1;
                if (len > cmax - op) len = cmax - op;
                for (u32 k = 0; k < len; k++, op++) out[op] = out[op - off];
            }
        }
        /* a chunk shorter than 4096 bytes ends the data only at the end of
         * the unit; inside it, the gap reads as zeros */
        if (op < cmax && ip + 2 <= inlen && (in[ip] | in[ip + 1])) {
            memset(out + op, 0, cmax - op);
            op = cmax;
        }
    }
    return (int)op;
}
