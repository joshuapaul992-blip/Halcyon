/* Halcyon OS - partition tables for the installer (see parttab.h) */
#include "fs/parttab.h"
#include "core/lib.h"
#include "core/mm.h"

static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u64 rd64(const u8 *p) { return rd32(p) | ((u64)rd32(p + 4) << 32); }
static void wr16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void wr32(u8 *p, u32 v) { for (int i = 0; i < 4; i++) p[i] = (u8)(v >> (8 * i)); }
static void wr64(u8 *p, u64 v) { for (int i = 0; i < 8; i++) p[i] = (u8)(v >> (8 * i)); }

const u8 PT_GUID_ESP[16] = { 0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B };
const u8 PT_GUID_BIOS_BOOT[16] = { 0x48, 0x61, 0x68, 0x21, 0x49, 0x64, 0x6F, 0x6E, 0x74, 0x4E, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49 };
const u8 PT_GUID_BASIC_DATA[16] = { 0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 };
const u8 PT_GUID_MS_RESERVED[16] = { 0x16, 0xE3, 0xC9, 0xE3, 0x5C, 0x0B, 0xB8, 0x4D, 0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE };
const u8 PT_GUID_WIN_RECOVERY[16] = { 0xA4, 0xBB, 0x94, 0xDE, 0xD1, 0x06, 0x40, 0x4D, 0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC };
static const u8 GUID_LINUX_FS[16] = { 0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 };
static const u8 GUID_LINUX_SWAP[16] = { 0x6D, 0xFD, 0x57, 0x06, 0xAB, 0xA4, 0xC4, 0x43, 0x84, 0xE5, 0x09, 0x33, 0xC8, 0x4B, 0x4F, 0x4F };

#define GPT_HDR_SIZE 92
#define MBR_LIMIT 0x100000000ull           /* MBR entries hold 32-bit LBAs */

/* ---------------- helpers ---------------- */

u32 pt_crc32(const void *data, size_t n) {
    static u32 tab[256];
    static int ready;
    if (!ready) {
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tab[i] = c;
        }
        ready = 1;
    }
    const u8 *p = data;
    u32 c = 0xFFFFFFFFu;
    while (n--) c = tab[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

int pt_guid_zero(const u8 g[16]) {
    for (int i = 0; i < 16; i++) if (g[i]) return 0;
    return 1;
}

void pt_guid_v4(u8 g[16], const u8 r[16]) {
    memcpy(g, r, 16);
    g[7] = (u8)((g[7] & 0x0F) | 0x40);        /* version 4 (the high nibble of the third field) */
    g[8] = (u8)((g[8] & 0x3F) | 0x80);        /* RFC 4122 variant */
}

void pt_guid_str(const u8 g[16], char out[37]) {
    snprintf(out, 37, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", rd32(g), rd16(g + 4), rd16(g + 6),
             g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

const char *pt_type_name(int kind, const pt_part_t *p) {
    if (kind == PT_GPT) {
        if (!memcmp(p->type_guid, PT_GUID_ESP, 16)) return "EFI system partition";
        if (!memcmp(p->type_guid, PT_GUID_BIOS_BOOT, 16)) return "BIOS boot partition";
        if (!memcmp(p->type_guid, PT_GUID_BASIC_DATA, 16)) return "Basic data partition";
        if (!memcmp(p->type_guid, PT_GUID_MS_RESERVED, 16)) return "Microsoft reserved partition";
        if (!memcmp(p->type_guid, PT_GUID_WIN_RECOVERY, 16)) return "Windows recovery partition";
        if (!memcmp(p->type_guid, GUID_LINUX_FS, 16)) return "Linux file system";
        if (!memcmp(p->type_guid, GUID_LINUX_SWAP, 16)) return "Linux swap";
        return "Partition";
    }
    switch (p->mbr_type) {
    case 0x01: return "FAT12";
    case 0x04: case 0x06: case 0x0E: return "FAT16";
    case 0x05: case 0x0F: case 0x85: return "Extended partition";
    case 0x07: return "NTFS / exFAT";
    case 0x0B: case 0x0C: return "FAT32";
    case 0x27: return "Windows recovery";
    case 0x82: return "Linux swap";
    case 0x83: return "Linux";
    case 0x8E: return "Linux LVM";
    case 0xA5: case 0xA6: case 0xA9: return "BSD";
    case 0xAF: return "Mac OS X";
    case 0xEF: return "EFI system partition";
    default: return "Partition";
    }
}

static int overlaps(u64 s1, u64 c1, u64 s2, u64 c2) { return s1 < s2 + c2 && s2 < s1 + c1; }

static void other(pt_table_t *t, const char *why) {
    t->kind = PT_OTHER;
    strlcpy(t->problem, why, sizeof t->problem);
}

/* UTF-16LE (BMP) <-> UTF-8 for GPT names */
static void name_from_utf16(const u8 *s, int max_units, char *out, size_t outsz) {
    size_t o = 0;
    for (int i = 0; i < max_units; i++) {
        u32 c = rd16(s + 2 * i);
        if (!c) break;
        if (c >= 0xD800 && c < 0xE000) c = '?';
        char b[3];
        int n;
        if (c < 0x80) { b[0] = (char)c; n = 1; }
        else if (c < 0x800) { b[0] = (char)(0xC0 | (c >> 6)); b[1] = (char)(0x80 | (c & 0x3F)); n = 2; }
        else { b[0] = (char)(0xE0 | (c >> 12)); b[1] = (char)(0x80 | ((c >> 6) & 0x3F)); b[2] = (char)(0x80 | (c & 0x3F)); n = 3; }
        if (o + (size_t)n + 1 > outsz) break;
        memcpy(out + o, b, (size_t)n);
        o += (size_t)n;
    }
    out[o] = 0;
}

static void name_to_utf16(const char *s, u8 *out, int max_units) {
    const u8 *p = (const u8 *)(s ? s : "");
    int n = 0;
    while (*p && n < max_units) {
        u32 c = *p++;
        if (c >= 0xE0 && p[0] && p[1]) { c = ((c & 0x0F) << 12) | ((u32)(p[0] & 0x3F) << 6) | (p[1] & 0x3F); p += 2; }
        else if (c >= 0xC0 && p[0]) { c = ((c & 0x1F) << 6) | (p[0] & 0x3F); p++; }
        else if (c >= 0x80) c = '?';
        wr16(out + 2 * n++, (u16)c);
    }
}

/* MBR CHS address, 255 heads and 63 sectors per track; beyond cylinder
 * 1023 the conventional 1023/254/63 */
static void chs(u8 *o, u64 lba) {
    if (lba >= 1024ull * 255 * 63) { o[0] = 0xFE; o[1] = 0xFF; o[2] = 0xFF; return; }
    u32 c = (u32)(lba / (255 * 63)), h = (u32)(lba / 63 % 255), s = (u32)(lba % 63 + 1);
    o[0] = (u8)h;
    o[1] = (u8)(s | ((c >> 2) & 0xC0));
    o[2] = (u8)c;
}

static void mbr_entry(u8 *e, u64 start, u64 count, u8 type, int active) {
    memset(e, 0, 16);
    e[0] = active ? 0x80 : 0x00;
    chs(e + 1, start);
    e[4] = type;
    chs(e + 5, start + count - 1);
    wr32(e + 8, (u32)start);
    wr32(e + 12, (u32)count);
}

static int is_ext(u8 t) { return t == 0x05 || t == 0x0F || t == 0x85; }

/* ---------------- reading ---------------- */

static int looks_like_bpb(const u8 *s) {
    if (s[0] != 0xEB && s[0] != 0xE9) return 0;
    u16 bps = rd16(s + 11);
    return (bps == 512 || bps == 1024 || bps == 2048 || bps == 4096) &&
           (!memcmp(s + 3, "NTFS    ", 8) || !memcmp(s + 3, "EXFAT   ", 8) || s[13]);
}

static pt_part_t *new_part(pt_table_t *t) {
    if (t->nparts >= (int)ARRAY_SIZE(t->parts)) return NULL;
    pt_part_t *p = &t->parts[t->nparts++];
    memset(p, 0, sizeof *p);
    return p;
}

static int read_mbr(const pt_io_t *io, pt_table_t *t, const u8 *s0, u8 *buf) {
    int nent = 0;
    for (int i = 0; i < 4; i++) {
        const u8 *e = s0 + 446 + 16 * i;
        if (!e[4]) { t->free_slots++; continue; }
        u32 start = rd32(e + 8), size = rd32(e + 12);
        if ((e[0] != 0 && e[0] != 0x80) || !start || !size) {
            if (looks_like_bpb(s0)) other(t, "A file system fills the whole disk (it has no partition table).");
            else other(t, "The disk's partition table (MBR) is damaged.");
            return 0;
        }
        if ((u64)start + size > io->sectors) {
            other(t, "A partition extends beyond the end of the disk.");
            return 0;
        }
        pt_part_t *p = new_part(t);
        p->start = start; p->count = size; p->index = i + 1;
        p->mbr_type = e[4]; p->active = e[0] == 0x80; p->extended = (u8)is_ext(e[4]);
        nent++;
        if (!p->extended) continue;
        t->has_extended = 1;
        /* logical partitions: a chain of EBRs, each moving forward */
        u64 cur = start;
        int index = 5;
        for (int k = 0; k < PT_MAX_PARTS; k++) {
            if (cur >= io->sectors || io->read(io->ctx, cur, 1, buf)) break;
            if (buf[510] != 0x55 || buf[511] != 0xAA) break;
            const u8 *l0 = buf + 446, *l1 = buf + 462;
            if (l0[4] && !is_ext(l0[4]) && rd32(l0 + 8) && rd32(l0 + 12)) {
                pt_part_t *lp = new_part(t);
                if (!lp) break;
                lp->start = cur + rd32(l0 + 8); lp->count = rd32(l0 + 12);
                lp->index = index++; lp->mbr_type = l0[4]; lp->logical = 1;
            }
            if (!is_ext(l1[4]) || !rd32(l1 + 8)) break;
            u64 next = start + rd32(l1 + 8);
            if (next <= cur) break;
            cur = next;
        }
    }
    if (!nent && looks_like_bpb(s0)) {
        other(t, "A file system fills the whole disk (it has no partition table).");
        return 0;
    }
    t->kind = PT_MBR;
    /* primary partitions (and extended containers) must not overlap */
    for (int i = 0; i < t->nparts; i++)
        for (int j = i + 1; j < t->nparts; j++)
            if (!t->parts[i].logical && !t->parts[j].logical &&
                overlaps(t->parts[i].start, t->parts[i].count, t->parts[j].start, t->parts[j].count)) {
                other(t, "Partitions on the disk overlap.");
                return 0;
            }
    return 0;
}

/* a GPT header (the first GPT_HDR_SIZE.. bytes of h) and its entry array */
static int gpt_header_valid(const pt_io_t *io, u8 *h, u64 lba) {
    if (memcmp(h, "EFI PART", 8)) return 0;
    u32 hsize = rd32(h + 12);
    if (hsize < GPT_HDR_SIZE || hsize > io->sector_size) return 0;
    u32 crc = rd32(h + 16);
    wr32(h + 16, 0);
    u32 calc = pt_crc32(h, hsize);
    wr32(h + 16, crc);
    if (calc != crc || rd64(h + 24) != lba) return 0;
    u64 first = rd64(h + 40), last = rd64(h + 48), alt = rd64(h + 32), ent = rd64(h + 72);
    u32 num = rd32(h + 80), esz = rd32(h + 84);
    if (esz < 128 || esz > 4096 || (esz & (esz - 1)) || !num || (u64)num * esz > (1u << 20)) return 0;
    if (first > last || last >= io->sectors || alt >= io->sectors || ent >= io->sectors) return 0;
    u64 entsec = ((u64)num * esz + io->sector_size - 1) / io->sector_size;
    if (ent + entsec > io->sectors) return 0;
    return 1;
}

static u8 *read_entries(const pt_io_t *io, const u8 *h, u32 *bytes_out) {
    u32 num = rd32(h + 80), esz = rd32(h + 84);
    u32 bytes = num * esz;
    u32 nsec = (bytes + io->sector_size - 1) / io->sector_size;
    u8 *e = kzalloc((size_t)nsec * io->sector_size);
    if (!e) return NULL;
    if (io->read(io->ctx, rd64(h + 72), nsec, e) || pt_crc32(e, bytes) != rd32(h + 88)) { kfree(e); return NULL; }
    *bytes_out = bytes;
    return e;
}

static int read_gpt(const pt_io_t *io, pt_table_t *t, u8 *buf) {
    if (io->read(io->ctx, 1, 1, buf) || !gpt_header_valid(io, buf, 1)) {
        other(t, "The disk's GPT partition table is damaged (the primary header is not valid).");
        return 0;
    }
    u8 hdr[GPT_HDR_SIZE];
    memcpy(hdr, buf, sizeof hdr);
    u32 bytes;
    u8 *ents = read_entries(io, hdr, &bytes);
    if (!ents) {
        other(t, "The disk's GPT partition table is damaged (the entry array's CRC does not match).");
        return 0;
    }
    t->first_usable = rd64(hdr + 40); t->last_usable = rd64(hdr + 48);
    t->alt_lba = rd64(hdr + 32); t->ent_lba = rd64(hdr + 72);
    t->nent = rd32(hdr + 80); t->esz = rd32(hdr + 84);
    memcpy(t->disk_guid, hdr + 56, 16);
    t->kind = PT_GPT;
    for (u32 i = 0; i < t->nent; i++) {
        const u8 *e = ents + (size_t)i * t->esz;
        if (pt_guid_zero(e)) { t->free_entries++; continue; }
        u64 s = rd64(e + 32), l = rd64(e + 40);
        if (l < s || s < t->first_usable || l > t->last_usable) { other(t, "A GPT partition lies outside the usable part of the disk."); break; }
        pt_part_t *p = new_part(t);
        if (!p) { other(t, "The disk has more partitions than the installer handles."); break; }
        p->start = s; p->count = l - s + 1; p->index = (int)i + 1;
        memcpy(p->type_guid, e, 16);
        memcpy(p->guid, e + 16, 16);
        p->attrs = rd64(e + 48);
        name_from_utf16(e + 56, 36, p->name, sizeof p->name);
    }
    kfree(ents);
    if (t->kind != PT_GPT) return 0;
    for (int i = 0; i < t->nparts; i++)
        for (int j = i + 1; j < t->nparts; j++)
            if (overlaps(t->parts[i].start, t->parts[i].count, t->parts[j].start, t->parts[j].count)) {
                other(t, "Partitions on the disk overlap.");
                return 0;
            }
    return 0;
}

int pt_read(const pt_io_t *io, pt_table_t *t) {
    memset(t, 0, sizeof *t);
    if (!io || io->sector_size < 512 || io->sector_size > 4096 || (io->sector_size & (io->sector_size - 1)) ||
        io->sectors < 128) return E_INVAL;
    t->ss = io->sector_size;
    t->sectors = io->sectors;
    u8 *s0 = kmalloc(io->sector_size), *buf = kmalloc(io->sector_size);
    int r = s0 && buf ? 0 : E_NOMEM;
    if (!r) r = io->read(io->ctx, 0, 1, s0);
    if (!r) {
        memcpy(t->mbr, s0, 512);
        t->disk_sig = rd32(s0 + 440);
        if (s0[510] != 0x55 || s0[511] != 0xAA) {
            int zero = 1;
            for (u32 i = 0; i < 512; i++) if (s0[i]) zero = 0;
            if (zero) t->kind = PT_NONE;
            else other(t, looks_like_bpb(s0) ? "A file system fills the whole disk (it has no partition table)."
                                             : "The disk holds data that is not a partition table.");
        } else {
            int gpt = 0;
            for (int i = 0; i < 4; i++) if (s0[446 + 16 * i + 4] == 0xEE) gpt = 1;
            r = gpt ? read_gpt(io, t, buf) : read_mbr(io, t, s0, buf);
        }
    }
    kfree(s0);
    kfree(buf);
    return r;
}

/* ---------------- free space ---------------- */

int pt_free_regions(const pt_table_t *t, u64 align, pt_region_t *out, int max) {
    u64 lo, hi;
    if (!align) align = 1;
    if (t->kind == PT_MBR) {
        if (!t->free_slots) return 0;
        lo = 1;
        hi = MIN(t->sectors, MBR_LIMIT);
    } else if (t->kind == PT_GPT) {
        if (!t->free_entries) return 0;
        lo = t->first_usable;
        hi = t->last_usable + 1;
    } else {
        return 0;
    }
    /* the used ranges, sorted by start (logical partitions lie inside their container) */
    int idx[PT_MAX_PARTS + 4], n = 0;
    for (int i = 0; i < t->nparts; i++) if (!t->parts[i].logical) idx[n++] = i;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && t->parts[idx[j - 1]].start > t->parts[idx[j]].start; j--) {
            int x = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = x;
        }
    int cnt = 0;
    u64 cur = lo;
    for (int k = 0; k <= n && cnt < max; k++) {
        u64 gend = k < n ? t->parts[idx[k]].start : hi;
        if (gend > hi) gend = hi;
        u64 s = ALIGN_UP(cur, align), e = ALIGN_DOWN(gend, align);
        if (e > s) { out[cnt].start = s; out[cnt].count = e - s; cnt++; }
        if (k < n) cur = MAX(cur, t->parts[idx[k]].start + t->parts[idx[k]].count);
    }
    for (int i = 1; i < cnt; i++)
        for (int j = i; j > 0 && out[j - 1].count < out[j].count; j--) {
            pt_region_t x = out[j]; out[j] = out[j - 1]; out[j - 1] = x;
        }
    return cnt;
}

/* ---------------- writing ---------------- */

static int check_new(const pt_new_t *p, int n, u64 lo, u64 hi) {
    for (int i = 0; i < n; i++) {
        if (!p[i].count || p[i].start < lo || p[i].start + p[i].count > hi || p[i].start + p[i].count < p[i].start)
            return E_INVAL;
        for (int j = 0; j < i; j++)
            if (overlaps(p[i].start, p[i].count, p[j].start, p[j].count)) return E_INVAL;
    }
    return 0;
}

int pt_write_mbr(const pt_io_t *io, const pt_new_t *p, int n, u32 disk_sig, const u8 *boot440) {
    if (n < 0 || n > 4) return E_INVAL;
    int r = check_new(p, n, 1, MIN(io->sectors, MBR_LIMIT));
    if (r) return r;
    int active = 0;
    for (int i = 0; i < n; i++) active += p[i].active ? 1 : 0;
    if (active > 1) return E_INVAL;
    u8 *s = kzalloc(io->sector_size);
    if (!s) return E_NOMEM;
    if (boot440) memcpy(s, boot440, 440);
    wr32(s + 440, disk_sig);
    for (int i = 0; i < n; i++) mbr_entry(s + 446 + 16 * i, p[i].start, p[i].count, p[i].mbr_type, p[i].active);
    s[510] = 0x55; s[511] = 0xAA;
    r = io->write(io->ctx, 0, 1, s);
    kfree(s);
    return r;
}

static void gpt_entry(u8 *e, const pt_new_t *p) {
    memset(e, 0, 128);
    memcpy(e, p->type_guid, 16);
    memcpy(e + 16, p->guid, 16);
    wr64(e + 32, p->start);
    wr64(e + 40, p->start + p->count - 1);
    name_to_utf16(p->name, e + 56, 36);
}

static void gpt_header(u8 *h, u64 my, u64 alt, u64 first, u64 last, const u8 *disk_guid, u64 ent, u32 num, u32 esz,
                       u32 ecrc) {
    memset(h, 0, GPT_HDR_SIZE);
    memcpy(h, "EFI PART", 8);
    wr32(h + 8, 0x00010000);
    wr32(h + 12, GPT_HDR_SIZE);
    wr64(h + 24, my);
    wr64(h + 32, alt);
    wr64(h + 40, first);
    wr64(h + 48, last);
    memcpy(h + 56, disk_guid, 16);
    wr64(h + 72, ent);
    wr32(h + 80, num);
    wr32(h + 84, esz);
    wr32(h + 88, ecrc);
    wr32(h + 16, pt_crc32(h, GPT_HDR_SIZE));
}

/* a header sector: the header, zeros after it */
static int write_header(const pt_io_t *io, u64 lba, const u8 *h) {
    u8 *s = kzalloc(io->sector_size);
    if (!s) return E_NOMEM;
    memcpy(s, h, GPT_HDR_SIZE);
    int r = io->write(io->ctx, lba, 1, s);
    kfree(s);
    return r;
}

int pt_write_gpt(const pt_io_t *io, const pt_new_t *p, int n, const u8 disk_guid[16], const u8 *boot440) {
    u32 ss = io->sector_size;
    u64 entsec = ((u64)PT_GPT_ENTRIES * 128 + ss - 1) / ss;
    if (io->sectors < 2 * entsec + 8 || n < 0 || n > PT_GPT_ENTRIES) return E_INVAL;
    u64 first = 2 + entsec, last = io->sectors - 2 - entsec, alt = io->sectors - 1;
    int r = check_new(p, n, first, last + 1);
    if (r) return r;
    u8 *ents = kzalloc((size_t)entsec * ss), *s = kzalloc(ss);
    if (!ents || !s) { kfree(ents); kfree(s); return E_NOMEM; }
    for (int i = 0; i < n; i++) gpt_entry(ents + 128 * i, &p[i]);
    u32 ecrc = pt_crc32(ents, PT_GPT_ENTRIES * 128);
    u8 h[GPT_HDR_SIZE], b[GPT_HDR_SIZE];
    gpt_header(h, 1, alt, first, last, disk_guid, 2, PT_GPT_ENTRIES, 128, ecrc);
    gpt_header(b, alt, 1, first, last, disk_guid, last + 1, PT_GPT_ENTRIES, 128, ecrc);
    r = io->write(io->ctx, 2, (u32)entsec, ents);
    if (!r) r = write_header(io, 1, h);
    if (!r) r = io->write(io->ctx, last + 1, (u32)entsec, ents);
    if (!r) r = write_header(io, alt, b);
    if (!r) {
        /* protective MBR: one type 0xEE entry over the whole disk */
        if (boot440) memcpy(s, boot440, 440);
        u8 *e = s + 446;
        e[1] = 0x00; e[2] = 0x02; e[3] = 0x00;
        e[4] = 0xEE;
        chs(e + 5, io->sectors - 1);
        wr32(e + 8, 1);
        wr32(e + 12, (u32)MIN(io->sectors - 1, 0xFFFFFFFFull));
        s[510] = 0x55; s[511] = 0xAA;
        r = io->write(io->ctx, 0, 1, s);
    }
    kfree(ents);
    kfree(s);
    return r;
}

int pt_add_mbr(const pt_io_t *io, const pt_table_t *t, const pt_new_t *p, int *index_out) {
    if (t->kind != PT_MBR) return E_INVAL;
    u8 *s = kmalloc(io->sector_size);
    if (!s) return E_NOMEM;
    int r = io->read(io->ctx, 0, 1, s);
    /* the table must be the one the plan was made from */
    if (!r && memcmp(s + 440, t->mbr + 440, 72)) r = E_BUSY;
    if (!r) r = check_new(p, 1, 1, MIN(io->sectors, MBR_LIMIT));
    for (int i = 0; !r && i < t->nparts; i++)
        if (!t->parts[i].logical && overlaps(p->start, p->count, t->parts[i].start, t->parts[i].count)) r = E_INVAL;
    int slot = -1;
    for (int i = 0; !r && i < 4; i++) {
        if (!s[446 + 16 * i + 4] && slot < 0) slot = i;
        if (p->active && s[446 + 16 * i] == 0x80) r = E_INVAL;     /* one active partition */
    }
    if (!r && slot < 0) r = E_NOSPC;
    if (!r) {
        mbr_entry(s + 446 + 16 * slot, p->start, p->count, p->mbr_type, p->active);
        r = io->write(io->ctx, 0, 1, s);
        if (!r && index_out) *index_out = slot + 1;
    }
    kfree(s);
    return r;
}

int pt_add_gpt(const pt_io_t *io, const pt_table_t *t, const pt_new_t *p, int n, int *index_out) {
    if (t->kind != PT_GPT || n < 1) return E_INVAL;
    u32 ss = io->sector_size;
    u8 *s = kmalloc(ss);
    if (!s) return E_NOMEM;
    int r = io->read(io->ctx, 1, 1, s);
    if (!r && !gpt_header_valid(io, s, 1)) r = E_IO;
    if (!r && rd32(s + 12) != GPT_HDR_SIZE) r = E_NOTSUP;    /* (headers are rewritten as 92 bytes) */
    u8 h[GPT_HDR_SIZE];
    memcpy(h, s, sizeof h);
    if (!r && (rd64(h + 72) != t->ent_lba || rd32(h + 80) != t->nent || rd32(h + 84) != t->esz ||
               rd64(h + 32) != t->alt_lba || rd64(h + 40) != t->first_usable || rd64(h + 48) != t->last_usable))
        r = E_BUSY;
    u32 bytes = 0;
    u8 *ents = r ? NULL : read_entries(io, h, &bytes);
    if (!r && !ents) r = E_IO;
    if (!r) r = check_new(p, n, t->first_usable, t->last_usable + 1);
    u32 esz = t->esz, num = t->nent;
    /* no overlap with anything in the array (all entries, not only those listed in t) */
    for (u32 i = 0; !r && i < num; i++) {
        const u8 *e = ents + (size_t)i * esz;
        if (pt_guid_zero(e)) continue;
        u64 es = rd64(e + 32), el = rd64(e + 40);
        for (int k = 0; k < n; k++) if (overlaps(p[k].start, p[k].count, es, el - es + 1)) r = E_INVAL;
    }
    for (int k = 0; !r && k < n; k++) {
        u32 i = 0;
        while (i < num && !pt_guid_zero(ents + (size_t)i * esz)) i++;
        if (i == num) { r = E_NOSPC; break; }
        u8 *e = ents + (size_t)i * esz;
        memset(e, 0, esz);
        gpt_entry(e, &p[k]);
        if (index_out) index_out[k] = (int)i + 1;
    }
    u64 entsec = ((u64)bytes + ss - 1) / ss;
    u64 alt = t->alt_lba, bent = 0;
    if (!r) {
        /* the backup array stays where the backup header says, if it is valid;
         * else right below the backup header (rewritten: a repair) */
        if (!io->read(io->ctx, alt, 1, s) && gpt_header_valid(io, s, alt) && rd32(s + 80) == num && rd32(s + 84) == esz)
            bent = rd64(s + 72);
        else
            bent = alt - entsec;
        if (bent <= t->last_usable || bent + entsec > alt) r = E_INVAL;
    }
    if (!r) {
        u32 ecrc = pt_crc32(ents, bytes);
        u8 b[GPT_HDR_SIZE];
        memcpy(b, h, sizeof b);
        /* the primary header keeps every field but the CRCs */
        wr32(h + 88, ecrc);
        wr32(h + 16, 0);
        wr32(h + 16, pt_crc32(h, GPT_HDR_SIZE));
        wr64(b + 24, alt);
        wr64(b + 32, 1);
        wr64(b + 72, bent);
        wr32(b + 88, ecrc);
        wr32(b + 16, 0);
        wr32(b + 16, pt_crc32(b, GPT_HDR_SIZE));
        r = io->write(io->ctx, rd64(h + 72), (u32)entsec, ents);
        if (!r) r = write_header(io, 1, h);
        if (!r) r = io->write(io->ctx, bent, (u32)entsec, ents);
        if (!r) r = write_header(io, alt, b);
    }
    kfree(ents);
    kfree(s);
    return r;
}

int pt_write_bootcode(const pt_io_t *io, const u8 *boot440) {
    u8 *s = kmalloc(io->sector_size);
    if (!s) return E_NOMEM;
    int r = io->read(io->ctx, 0, 1, s);
    if (!r && (s[510] != 0x55 || s[511] != 0xAA)) r = E_INVAL;
    if (!r) {
        memcpy(s, boot440, 440);
        r = io->write(io->ctx, 0, 1, s);
    }
    kfree(s);
    return r;
}
