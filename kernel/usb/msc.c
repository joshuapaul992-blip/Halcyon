/* Halcyon OS - USB mass storage: Bulk-Only Transport + SCSI transparent */
#include "usb.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "fs/block.h"

#define BOT_RESET   0xFF
#define BOT_MAX_LUN 0xFE
#define CBW_SIG 0x43425355
#define CSW_SIG 0x53425355
#define MAX_LUNS 4
#define XFER_MAX (64 * 1024)

typedef struct PACKED cbw {
    u32 sig, tag, len;
    u8 flags, lun, cblen;
    u8 cb[16];
} cbw_t;
typedef struct PACKED csw { u32 sig, tag, residue; u8 status; } csw_t;

typedef struct msc msc_t;
typedef struct msc_lun {
    blockdev_t bd;
    msc_t *m;
    u8 lun;
    u8 registered;
} msc_lun_t;

struct msc {
    usb_device_t *dev;
    usb_endpoint_t *in, *out;
    u8 intf;
    u32 tag;
    int nluns;
    int dead;
    cbw_t *cbw;
    csw_t *csw;
    u8 *sense;
    msc_lun_t luns[MAX_LUNS];
};

static int disk_counter;

static int reset_recovery(msc_t *m) {
    kprintf("usb-msc: reset recovery\n");
    usb_control(m->dev, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE, BOT_RESET, 0, m->intf, NULL, 0);
    usb_clear_halt(m->dev, m->in);
    usb_clear_halt(m->dev, m->out);
    return 0;
}

/* one SCSI command; dir_in: data phase direction. Returns 0, 1 (check
 * condition), or a negative error. */
static int scsi_cmd(msc_t *m, u8 lun, const u8 *cb, int cblen, void *data, u32 len, int dir_in, int timeout) {
    if (m->dead || !m->dev->alive) return E_NODEV;
    cbw_t *c = m->cbw;
    memset(c, 0, sizeof *c);
    c->sig = CBW_SIG;
    c->tag = ++m->tag;
    c->len = len;
    c->flags = dir_in ? 0x80 : 0x00;
    c->lun = lun;
    c->cblen = (u8)cblen;
    memcpy(c->cb, cb, cblen);
    int r = usb_bulk(m->dev, m->out, c, 31, 5000);
    if (r == E_STALL) { reset_recovery(m); return E_IO; }
    if (r != 31) { if (r == E_NODEV) m->dead = 1; return r < 0 ? r : E_IO; }

    if (len) {
        r = usb_bulk(m->dev, dir_in ? m->in : m->out, data, (int)len, timeout);
        if (r == E_STALL) usb_clear_halt(m->dev, dir_in ? m->in : m->out);
        else if (r < 0) { if (r == E_NODEV) m->dead = 1; else reset_recovery(m); return r; }
    }
    /* status */
    r = usb_bulk(m->dev, m->in, m->csw, 13, 5000);
    if (r == E_STALL) {
        usb_clear_halt(m->dev, m->in);
        r = usb_bulk(m->dev, m->in, m->csw, 13, 5000);
    }
    if (r != 13 || m->csw->sig != CSW_SIG || m->csw->tag != c->tag) {
        if (r == E_NODEV) { m->dead = 1; return r; }
        reset_recovery(m);
        return E_IO;
    }
    if (m->csw->status == 2) { reset_recovery(m); return E_IO; }
    return m->csw->status == 1 ? 1 : 0;
}

static int request_sense(msc_t *m, u8 lun, u8 *key, u8 *asc) {
    u8 cb[6] = { 0x03, 0, 0, 0, 18, 0 };
    memset(m->sense, 0, 18);
    int r = scsi_cmd(m, lun, cb, 6, m->sense, 18, 1, 2000);
    if (r != 0) return E_IO;
    *key = m->sense[2] & 0x0F;
    *asc = m->sense[12];
    return 0;
}

static int rw(blockdev_t *bd, u64 lba, u32 count, void *buf, int write) {
    msc_lun_t *l = bd->priv;
    msc_t *m = l->m;
    u32 ss = bd->sector_size;
    u32 maxsec = XFER_MAX / ss;
    if (!maxsec) maxsec = 1;
    u8 *p = buf;
    while (count) {
        u32 n = MIN(count, maxsec);
        u8 cb[16] = { 0 };
        int cblen;
        if (lba + n > 0xFFFFFFFFull) {
            cb[0] = write ? 0x8A : 0x88;
            for (int i = 0; i < 8; i++) cb[2 + i] = (u8)(lba >> (56 - 8 * i));
            cb[10] = n >> 24; cb[11] = n >> 16; cb[12] = n >> 8; cb[13] = (u8)n;
            cblen = 16;
        } else {
            cb[0] = write ? 0x2A : 0x28;
            cb[2] = lba >> 24; cb[3] = lba >> 16; cb[4] = lba >> 8; cb[5] = (u8)lba;
            cb[7] = n >> 8; cb[8] = (u8)n;
            cblen = 10;
        }
        int r = E_IO;
        for (int tries = 0; tries < 3; tries++) {
            r = scsi_cmd(m, l->lun, cb, cblen, p, n * ss, !write, 10000);
            if (r == 0) break;
            if (r == E_NODEV) return r;
            u8 key, asc;
            if (r == 1) request_sense(m, l->lun, &key, &asc);
            mdelay(50);
        }
        if (r != 0) {
            kprintf("usb-msc: %s failed at LBA %lu (+%u)\n", write ? "write" : "read", lba, n);
            return E_IO;
        }
        p += n * ss; lba += n; count -= n;
    }
    if (write) {
        u8 cb[10] = { 0x35 };   /* SYNCHRONIZE CACHE(10): ignore failures */
        scsi_cmd(m, l->lun, cb, 10, NULL, 0, 0, 10000);
    }
    return 0;
}

static int msc_read(blockdev_t *bd, u64 lba, u32 count, void *buf) { return rw(bd, lba, count, buf, 0); }
static int msc_write(blockdev_t *bd, u64 lba, u32 count, const void *buf) { return rw(bd, lba, count, (void *)buf, 1); }

static int lun_init(msc_t *m, int lun) {
    msc_lun_t *l = &m->luns[lun];
    l->m = m; l->lun = (u8)lun;
    u8 *buf = kzalloc(64);
    char vendor[9] = { 0 }, product[17] = { 0 };
    u8 inq[6] = { 0x12, 0, 0, 0, 36, 0 };
    int r = scsi_cmd(m, lun, inq, 6, buf, 36, 1, 5000);
    int removable = 0;
    if (r == 0) {
        if ((buf[0] & 0x1F) != 0x00 && (buf[0] & 0x1F) != 0x0E) {
            kprintf("usb-msc: LUN %d is peripheral type %#x, skipped\n", lun, buf[0] & 0x1F);
            kfree(buf); return -1;
        }
        removable = (buf[1] & 0x80) != 0;
        memcpy(vendor, buf + 8, 8); memcpy(product, buf + 16, 16);
        for (int i = 7; i >= 0 && vendor[i] == ' '; i--) vendor[i] = 0;
        for (int i = 15; i >= 0 && product[i] == ' '; i--) product[i] = 0;
    }
    /* wait for the unit to become ready (up to ~6 s) */
    int ready = 0;
    for (int i = 0; i < 30; i++) {
        u8 tur[6] = { 0 };
        r = scsi_cmd(m, lun, tur, 6, NULL, 0, 0, 2000);
        if (r == 0) { ready = 1; break; }
        if (r < 0 && r != E_IO) break;
        u8 key = 0, asc = 0;
        request_sense(m, lun, &key, &asc);
        if (key == 0x02 && asc == 0x3A) break;   /* medium not present */
        mdelay(200);
    }
    if (!ready) { kprintf("usb-msc: LUN %d not ready (no medium?)\n", lun); kfree(buf); return -1; }

    u64 blocks = 0; u32 bsize = 0;
    for (int tries = 0; tries < 3 && !bsize; tries++) {
        u8 rc[10] = { 0x25 };
        memset(buf, 0, 8);
        r = scsi_cmd(m, lun, rc, 10, buf, 8, 1, 5000);
        if (r == 0) {
            u32 last = ((u32)buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
            bsize = ((u32)buf[4] << 24) | (buf[5] << 16) | (buf[6] << 8) | buf[7];
            blocks = (u64)last + 1;
            if (last == 0xFFFFFFFF) {
                u8 rc16[16] = { 0x9E, 0x10 };
                rc16[13] = 32;
                memset(buf, 0, 32);
                if (scsi_cmd(m, lun, rc16, 16, buf, 32, 1, 5000) == 0) {
                    u64 l64 = 0;
                    for (int i = 0; i < 8; i++) l64 = (l64 << 8) | buf[i];
                    blocks = l64 + 1;
                    bsize = ((u32)buf[8] << 24) | (buf[9] << 16) | (buf[10] << 8) | buf[11];
                }
            }
        } else {
            u8 key, asc; request_sense(m, lun, &key, &asc);
            mdelay(100);
        }
    }
    if (!bsize || bsize > 4096 || (bsize & (bsize - 1))) {
        kprintf("usb-msc: LUN %d: READ CAPACITY failed\n", lun);
        kfree(buf); return -1;
    }
    /* write protect: MODE SENSE(6), device-specific parameter bit 7 */
    int ro = 0;
    u8 ms[6] = { 0x1A, 0, 0x3F, 0, 192, 0 };
    u8 *mbuf = kzalloc(192);
    if (scsi_cmd(m, lun, ms, 6, mbuf, 192, 1, 2000) == 0) ro = (mbuf[2] & 0x80) != 0;
    kfree(mbuf);

    blockdev_t *bd = &l->bd;
    snprintf(bd->name, sizeof bd->name, "usb%d", disk_counter++);
    if (vendor[0] || product[0]) snprintf(bd->model, sizeof bd->model, "%s %s", vendor, product);
    else snprintf(bd->model, sizeof bd->model, "%s", m->dev->product);
    bd->type = BDEV_DISK_USB;
    bd->sector_size = bsize;
    bd->sector_count = blocks;
    bd->removable = 1;
    (void)removable;
    bd->readonly = (u8)ro;
    bd->read = msc_read;
    bd->write = msc_write;
    bd->priv = l;
    kprintf("usb-msc: %s: \"%s\" %lu x %u bytes (%lu MiB)%s\n", bd->name, bd->model, blocks, bsize,
            (blocks * bsize) >> 20, ro ? " write-protected" : "");
    kfree(buf);
    if (blockdev_register(bd) == 0) l->registered = 1;
    return 0;
}

static int msc_probe(usb_device_t *dev, const usb_interface_desc_t *intf, usb_binding_t *b) {
    if (intf->bInterfaceClass != USB_CLASS_MSC || intf->bInterfaceProtocol != 0x50) return E_NOTSUP;
    if (intf->bInterfaceSubClass != 0x06 && intf->bInterfaceSubClass != 0x05 && intf->bInterfaceSubClass != 0x02)
        return E_NOTSUP;
    msc_t *m = kzalloc(sizeof *m);
    m->dev = dev;
    m->intf = intf->bInterfaceNumber;
    m->in = usb_find_endpoint(dev, m->intf, USB_EP_BULK, USB_DIR_IN);
    m->out = usb_find_endpoint(dev, m->intf, USB_EP_BULK, USB_DIR_OUT);
    if (!m->in || !m->out) { kfree(m); return E_NOTSUP; }
    m->cbw = dma_alloc(64, 64);
    m->csw = dma_alloc(64, 64);
    m->sense = dma_alloc(64, 64);
    b->priv = m;

    u8 maxlun = 0;
    if (usb_control(dev, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE, BOT_MAX_LUN, 0, m->intf, &maxlun, 1) < 1)
        maxlun = 0;
    m->nluns = MIN(maxlun + 1, MAX_LUNS);
    snprintf(dev->description, sizeof dev->description, "USB mass storage device");
    kprintf("usb-msc: %s, %d LUN(s)\n", dev->product, m->nluns);
    for (int i = 0; i < m->nluns; i++) lun_init(m, i);
    return 0;
}

static void msc_disconnect(usb_device_t *dev, usb_binding_t *b) {
    msc_t *m = b->priv;
    if (!m) return;
    m->dead = 1;
    for (int i = 0; i < m->nluns; i++)
        if (m->luns[i].registered) blockdev_unregister(&m->luns[i].bd);
    dma_free(m->cbw); dma_free(m->csw); dma_free(m->sense);
    kfree(m);
    b->priv = NULL;
}

const usb_driver_t usb_msc_driver = { "mass-storage", msc_probe, NULL, msc_disconnect };
