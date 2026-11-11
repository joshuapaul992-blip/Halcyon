/* Halcyon OS - NVMe (NVM Express) disk driver
 *
 * NVMe 1.4 driver for the NVM command set. Each controller gets the admin
 * queue pair and one I/O submission/completion queue pair. The kernel is
 * single threaded and keeps one command in flight per controller, so a queue
 * never fills and completions arrive in order.
 *
 * Data is described with PRPs (Physical Region Page entries, 4 KiB memory
 * pages): PRP1 may start anywhere in a page as long as it is dword aligned,
 * every further entry is a whole page, either directly in PRP2 (transfer
 * touches two pages) or in a PRP list page that PRP2 points to. Everything is
 * identity mapped and physically contiguous, so a caller's buffer is used
 * directly when it is dword aligned; otherwise the request goes through a
 * per-controller bounce buffer. Requests are split by the controller's MDTS
 * and NVME_MAX_XFER.
 *
 * Interrupts (core/irq.h): MSI-X or MSI vector 0 serves both the admin and the
 * I/O completion queue. An interrupt only wakes the wait loop, which reads the
 * completion queue itself (phase tag), so the driver works the same fully
 * polled (BOOTF_NO_IRQ, or before an INTx route is learned).
 * INTx: the controller keeps its pin asserted while a completion queue with
 * interrupts enabled holds entries the host has not consumed (CQ head
 * doorbell). The acknowledge callback runs from the main loop, possibly in the
 * middle of a command, so it must not consume completions (only the wait loop
 * does that, in order); instead it masks vector 0 with INTMS, which deasserts
 * the pin at once. The wait loop unmasks it with INTMC after it has consumed
 * the entry and written the CQ head doorbell: the queue is empty then and the
 * pin stays deasserted until the next completion. (INTMS/INTMC must not be
 * used with MSI-X; the acknowledge callback only runs for INTx.)
 *
 * Write caching: like the AHCI driver, a write that succeeds is followed by
 * a Flush when the controller has a volatile write cache (Identify VWC), so
 * data is on media when write() returns. nvme_shutdown() (called by reboot()
 * and shutdown()) sends the normal shutdown notification.
 */
#include "drivers/nvme.h"
#include "drivers/pci.h"
#include "fs/block.h"
#include "core/kernel.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/io.h"
#include "core/irq.h"

/* ---------------- controller registers (BAR0) ---------------- */
#define NVME_CAP     0x00        /* 64-bit */
#define NVME_VS      0x08
#define NVME_INTMS   0x0C        /* interrupt mask set (INTx / MSI only) */
#define NVME_INTMC   0x10        /* interrupt mask clear */
#define NVME_CC      0x14
#define NVME_CSTS    0x1C
#define NVME_AQA     0x24
#define NVME_ASQ     0x28        /* 64-bit */
#define NVME_ACQ     0x30        /* 64-bit */
#define NVME_DBS     0x1000      /* doorbells: SQ y tail at 2y, CQ y head at 2y+1 (x stride) */

#define CAP_MQES(c)    ((u32)((c) & 0xFFFF) + 1)          /* max I/O queue entries (0's based) */
#define CAP_TO(c)      ((u32)(((c) >> 24) & 0xFF))         /* ready timeout, 500 ms units */
#define CAP_DSTRD(c)   ((u32)(((c) >> 32) & 0xF))          /* doorbell stride 2^(2+n) */
#define CAP_CSS_NVM    (1ull << 37)
#define CAP_MPSMIN(c)  ((u32)(((c) >> 48) & 0xF))          /* memory page size 2^(12+n) */
#define CAP_MPSMAX(c)  ((u32)(((c) >> 52) & 0xF))

#define CC_EN          (1u << 0)
#define CC_CSS_NVM     (0u << 4)
#define CC_MPS(n)      ((u32)(n) << 7)                     /* 2^(12+n): 0 = 4 KiB */
#define CC_AMS_RR      (0u << 11)                          /* round robin arbitration */
#define CC_SHN_MASK    (3u << 14)
#define CC_SHN_NORMAL  (1u << 14)
#define CC_IOSQES(n)   ((u32)(n) << 16)                    /* 2^n bytes: 6 = 64 */
#define CC_IOCQES(n)   ((u32)(n) << 20)                    /* 2^n bytes: 4 = 16 */

#define CSTS_RDY       (1u << 0)
#define CSTS_CFS       (1u << 1)                           /* controller fatal status */
#define CSTS_SHST_MASK (3u << 2)
#define CSTS_SHST_DONE (2u << 2)

/* ---------------- commands ---------------- */
#define ADM_DELETE_SQ    0x00
#define ADM_CREATE_SQ    0x01
#define ADM_DELETE_CQ    0x04
#define ADM_CREATE_CQ    0x05
#define ADM_IDENTIFY     0x06
#define ADM_SET_FEATURES 0x09
#define NVM_FLUSH        0x00
#define NVM_WRITE        0x01
#define NVM_READ         0x02

#define CNS_NAMESPACE    0x00
#define CNS_CONTROLLER   0x01
#define CNS_NS_LIST      0x02    /* active namespace IDs (NVMe 1.1+) */
#define FEAT_NUM_QUEUES  0x07

/* completion status field (CQE dword 3 bits 31:16; bit 0 here is the phase tag) */
#define ST_PHASE         0x0001
#define ST_SC(s)         (((s) >> 1) & 0xFF)
#define ST_SCT(s)        (((s) >> 9) & 7)
#define ST_DNR           0x8000  /* do not retry */

/* ---------------- tunables ---------------- */
#define NVME_PAGE          4096u
#define NVME_ADMIN_QSIZE   32
#define NVME_IO_QSIZE      64
#define NVME_MAX_XFER      (128u * 1024)       /* bytes per command (and MDTS) */
#define NVME_BOUNCE_SIZE   (128u * 1024)
#define NVME_MAX_NS        16                  /* namespaces registered per controller */
#define NVME_SCAN_NS       64                  /* NSIDs tried without an active list */
#define NVME_ADMIN_TIMEOUT 5000                /* ms */
#define NVME_CMD_TIMEOUT   5000
#define NVME_FLUSH_TIMEOUT 30000
#define NVME_RETRIES       2                   /* for errors without DNR */

/* a 128 KiB transfer that starts mid-page touches 33 pages: PRP1 + 32 list
 * entries, well inside one 512-entry list page (no list chaining needed) */
_Static_assert(NVME_MAX_XFER / NVME_PAGE + 1 <= NVME_PAGE / 8, "PRP list must fit one page");

/* q_exec() result for a controller fatal status (CSTS.CFS) */
#define E_FATAL E_STALL

/* ---------------- data structures ---------------- */
typedef struct {
    u32 cdw0;          /* opcode [7:0], FUSE [9:8], PSDT [15:14] (0: PRPs), CID [31:16] */
    u32 nsid;
    u32 rsvd[2];
    u64 mptr;
    u64 prp1, prp2;
    u32 cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} PACKED nvme_sqe_t;

typedef struct {
    u32 dw0;           /* command specific result */
    u32 dw1;
    u16 sqhd, sqid;
    u16 cid;
    u16 status;        /* P [0], SC [8:1], SCT [11:9], CRD [13:12], M [14], DNR [15] */
} PACKED nvme_cqe_t;

_Static_assert(sizeof(nvme_sqe_t) == 64 && sizeof(nvme_cqe_t) == 16, "NVMe queue entry sizes");

typedef struct {
    nvme_sqe_t *sq;
    volatile nvme_cqe_t *cq;
    volatile u8 *sq_db, *cq_db;
    u16 qid, size;
    u16 sq_tail, cq_head;
    u16 cid;
    u8 phase;          /* phase tag value of new entries */
} nvme_queue_t;

struct nvme_ns;
typedef struct nvme_ctrl {
    pci_dev_t *pci;
    volatile u8 *regs;
    char name[16];               /* PCI location, for logs */
    char model[48];
    u64 cap;
    u32 vs;
    u32 dstrd;                   /* doorbell stride, bytes */
    u32 rdy_timeout;             /* CAP.TO, ms */
    u32 max_xfer;                /* bytes per command */
    u32 rtd3e;                   /* RTD3 entry latency, us (0: not reported) */
    u8 vwc;                      /* volatile write cache present */
    u8 busy, dead, shut, io_ok, intx_masked;
    nvme_queue_t aq, ioq;
    u64 *prp_list;               /* one page */
    u8 *bounce;                  /* NVME_BOUNCE_SIZE, also Identify data */
    irq_handle_t irq;
    char irq_name[24];
    int nns;
    struct nvme_ns *ns[NVME_MAX_NS];
    struct nvme_ctrl *next;
} nvme_ctrl_t;

typedef struct nvme_ns {
    nvme_ctrl_t *c;
    u32 nsid;
    u32 lba_size;
    u64 blocks;
    blockdev_t bd;
} nvme_ns_t;

/* weak: the driver also links (and logs) without a block layer */
int blockdev_register(blockdev_t *bd) __attribute__((weak));

static nvme_ctrl_t *g_ctrls;
static int g_disk_count;

int nvme_disk_count(void) { return g_disk_count; }

/* ---------------- helpers ---------------- */
static inline u32 reg_rd(nvme_ctrl_t *c, u32 off) { return mmio_read32(c->regs + off); }
static inline void reg_wr(nvme_ctrl_t *c, u32 off, u32 v) { mmio_write32(c->regs + off, v); }

static void idle_poll(u64 *last) {
    if (!g_idle_hook) return;
    u64 now = time_ms();
    if (now != *last) { *last = now; g_idle_hook(); }
}

static void fmt_size(u64 bytes, char *buf, size_t n) {
    if (bytes >= (10ull << 30)) snprintf(buf, n, "%lu GiB", bytes >> 30);
    else if (bytes >= (10ull << 20)) snprintf(buf, n, "%lu MiB", bytes >> 20);
    else snprintf(buf, n, "%lu KiB", bytes >> 10);
}

/* Identify strings are ASCII, space padded (not byte swapped as in ATA) */
static void id_string(const u8 *src, int len, char *out, size_t outsz) {
    size_t n = 0;
    for (int i = 0; i < len && n + 1 < outsz; i++) out[n++] = src[i] ? (char)src[i] : ' ';
    out[n] = 0;
    while (n && out[n - 1] == ' ') out[--n] = 0;
    size_t s = 0;
    while (out[s] == ' ') s++;
    if (s) memmove(out, out + s, n - s + 1);
    for (size_t i = 0; out[i]; i++)
        if (!isprint((u8)out[i])) out[i] = '?';
}

static const char *status_text(u16 st) {
    u32 sc = ST_SC(st);
    switch (ST_SCT(st)) {
    case 0:                                     /* generic command status */
        switch (sc) {
        case 0x00: return "success";
        case 0x01: return "invalid command opcode";
        case 0x02: return "invalid field in command";
        case 0x03: return "command ID conflict";
        case 0x04: return "data transfer error";
        case 0x05: return "aborted due to power loss notification";
        case 0x06: return "internal error";
        case 0x07: return "command abort requested";
        case 0x08: return "aborted due to SQ deletion";
        case 0x0B: return "invalid namespace or format";
        case 0x0C: return "command sequence error";
        case 0x13: return "PRP offset invalid";
        case 0x15: return "operation denied";
        case 0x1D: return "sanitize in progress";
        case 0x20: return "namespace is write protected";
        case 0x21: return "command interrupted";
        case 0x80: return "LBA out of range";
        case 0x81: return "capacity exceeded";
        case 0x82: return "namespace not ready";
        case 0x83: return "reservation conflict";
        case 0x84: return "format in progress";
        }
        return "generic error";
    case 1:                                     /* command specific status */
        switch (sc) {
        case 0x00: return "completion queue invalid";
        case 0x01: return "invalid queue identifier";
        case 0x02: return "invalid queue size";
        case 0x08: return "invalid interrupt vector";
        case 0x0B: return "invalid format";
        case 0x80: return "conflicting attributes";
        case 0x81: return "invalid protection information";
        case 0x82: return "attempted write to read only range";
        }
        return "command specific error";
    case 2:                                     /* media and data integrity errors */
        switch (sc) {
        case 0x80: return "write fault";
        case 0x81: return "unrecovered read error";
        case 0x82: return "end-to-end guard check error";
        case 0x83: return "end-to-end application tag check error";
        case 0x84: return "end-to-end reference tag check error";
        case 0x85: return "compare failure";
        case 0x86: return "access denied";
        case 0x87: return "deallocated or unwritten logical block";
        }
        return "media error";
    case 3: return "path related error";
    case 7: return "vendor specific error";
    }
    return "unknown status type";
}

static void cmd_desc(const nvme_queue_t *q, const nvme_sqe_t *cmd, char *buf, size_t n) {
    u8 op = (u8)cmd->cdw0;
    if (q->qid == 0) {
        const char *nm = op == ADM_IDENTIFY ? "IDENTIFY" : op == ADM_CREATE_CQ ? "CREATE I/O CQ" :
                         op == ADM_CREATE_SQ ? "CREATE I/O SQ" : op == ADM_DELETE_CQ ? "DELETE I/O CQ" :
                         op == ADM_DELETE_SQ ? "DELETE I/O SQ" : op == ADM_SET_FEATURES ? "SET FEATURES" : "admin";
        snprintf(buf, n, "%s (opcode %02x, nsid %u, cdw10 %08x)", nm, op, cmd->nsid, cmd->cdw10);
    } else if (op == NVM_FLUSH) {
        snprintf(buf, n, "FLUSH nsid %u", cmd->nsid);
    } else {
        snprintf(buf, n, "%s nsid %u lba %lu count %u", op == NVM_READ ? "READ" : op == NVM_WRITE ? "WRITE" : "I/O",
                 cmd->nsid, (u64)cmd->cdw10 | ((u64)cmd->cdw11 << 32), (cmd->cdw12 & 0xFFFF) + 1);
    }
}

/* ---------------- queues ---------------- */
static void queue_reset(nvme_ctrl_t *c, nvme_queue_t *q) {
    memset(q->sq, 0, (size_t)q->size * sizeof(nvme_sqe_t));
    memset((void *)q->cq, 0, (size_t)q->size * sizeof(nvme_cqe_t));
    q->sq_tail = q->cq_head = 0;
    q->phase = 1;                               /* zeroed entries carry phase 0 */
    q->sq_db = c->regs + NVME_DBS + (2u * q->qid) * c->dstrd;
    q->cq_db = c->regs + NVME_DBS + (2u * q->qid + 1) * c->dstrd;
}

static int queue_alloc(nvme_ctrl_t *c, nvme_queue_t *q, u16 qid, u16 size) {
    q->qid = qid;
    q->size = size;
    /* physically contiguous (CC/PC = 1) and page aligned */
    q->sq = dma_alloc((size_t)size * sizeof(nvme_sqe_t), NVME_PAGE);
    q->cq = dma_alloc((size_t)size * sizeof(nvme_cqe_t), NVME_PAGE);
    if (!q->sq || !q->cq) return E_NOMEM;
    queue_reset(c, q);
    return 0;
}

/* the completion at the head of q if the controller has posted one */
static volatile nvme_cqe_t *cq_peek(const nvme_queue_t *q) {
    volatile nvme_cqe_t *e = &q->cq[q->cq_head];
    return (e->status & ST_PHASE) == q->phase ? e : NULL;
}

/* INTx acknowledge (see the top of the file): mask vector 0 if the controller
 * is asserting its interrupt; the wait loop unmasks it once the completion is
 * consumed. Touches registers only. */
static int nvme_ack(void *ctx) {
    nvme_ctrl_t *c = ctx;
    if (c->intx_masked) return 0;               /* masked: cannot be asserting */
    int pending = (c->aq.cq && cq_peek(&c->aq)) || (c->ioq.cq && cq_peek(&c->ioq));
    if (!pending && !(pci_read16(c->pci, 0x06) & (1u << 3))) return 0;
    reg_wr(c, NVME_INTMS, 1);
    (void)reg_rd(c, NVME_INTMS);                /* flush the posted write: deasserted on return */
    c->intx_masked = 1;
    return 1;
}

/* Submit one command on q and wait for its completion. Returns 0, E_IO (error
 * status, *status set), E_TIMEOUT, E_FATAL (CSTS.CFS) or E_NODEV (controller
 * gone). After E_TIMEOUT / E_FATAL the queues are out of step with the
 * controller: the caller resets it (ctrl_restart). */
static int q_exec(nvme_ctrl_t *c, nvme_queue_t *q, nvme_sqe_t *cmd, u32 timeout_ms, u32 *result, u16 *status) {
    char d[80];
    if (status) *status = 0;
    if (c->dead) return E_NODEV;
    u16 cid = ++q->cid;
    cmd->cdw0 = (cmd->cdw0 & 0xFFu) | ((u32)cid << 16);    /* FUSE 0, PSDT 0 (PRPs) */
    memcpy(&q->sq[q->sq_tail], cmd, sizeof *cmd);
    if (++q->sq_tail == q->size) q->sq_tail = 0;
    wmb();
    u32 gen0 = irq_generation();
    mmio_write32(q->sq_db, q->sq_tail);

    u64 deadline = time_ms() + timeout_ms, last = 0;
    for (;;) {
        volatile nvme_cqe_t *e;
        int fail = 0;
        u32 csts = 0;
        for (;;) {
            u32 gen = irq_generation();
            if ((e = cq_peek(q))) break;
            csts = reg_rd(c, NVME_CSTS);
            if (csts == 0xFFFFFFFF) { fail = E_NODEV; break; }
            if (csts & CSTS_CFS) { fail = E_FATAL; break; }
            if (time_ms() > deadline) {
                if ((e = cq_peek(q))) break;
                fail = E_TIMEOUT;
                break;
            }
            idle_poll(&last);
            irq_wait(&c->irq, gen, deadline);
        }
        if (fail) {
            cmd_desc(q, cmd, d, sizeof d);
            if (fail == E_NODEV) {
                kprintf("nvme: %s: %s: controller not responding (registers read all ones), disabled\n", c->name, d);
                c->dead = 1;
            } else if (fail == E_FATAL) {
                kprintf("nvme: %s: %s: controller fatal status (CSTS %08x)\n", c->name, d, csts);
            } else {
                kprintf("nvme: %s: %s timed out after %u ms (CSTS %08x)\n", c->name, d, timeout_ms,
                        reg_rd(c, NVME_CSTS));
            }
            return fail;
        }
        rmb();
        /* INTx: let irq_service() see (and learn) the interrupt while the
         * completion that drives it is still unconsumed */
        if (c->irq.kind == IRQ_INTX) {
            if (c->irq.line < 0)
                for (u64 end = time_us() + 500; irq_generation() == gen0 && time_us() < end;) cpu_relax();
            irq_service();
        }
        u32 dw0 = e->dw0;
        u16 st = e->status, ecid = e->cid;
        /* consume: advance the head (the phase flips on wrap) and tell the
         * controller, which frees the slot and drops the interrupt */
        if (++q->cq_head == q->size) { q->cq_head = 0; q->phase ^= 1; }
        mmio_write32(q->cq_db, q->cq_head);
        if (c->intx_masked) {
            c->intx_masked = 0;
            reg_wr(c, NVME_INTMC, 1);
        }
        irq_note_work(&c->irq);
        if (ecid != cid) {
            /* a late completion of a command given up on earlier */
            kprintf("nvme: %s: queue %u: stray completion (CID %u, expected %u), ignored\n", c->name, q->qid, ecid, cid);
            continue;
        }
        if (ST_SC(st) || ST_SCT(st)) {
            if (status) *status = st | 1;       /* nonzero even for SC 0 */
            cmd_desc(q, cmd, d, sizeof d);
            kprintf("nvme: %s: %s failed: status %04x (SCT %u SC %02x: %s)%s\n", c->name, d, st >> 1,
                    ST_SCT(st), ST_SC(st), status_text(st), (st & ST_DNR) ? ", do not retry" : "");
            return E_IO;
        }
        if (result) *result = dw0;
        return 0;
    }
}

static int admin(nvme_ctrl_t *c, nvme_sqe_t *cmd, u32 *result) {
    return q_exec(c, &c->aq, cmd, NVME_ADMIN_TIMEOUT, result, NULL);
}

static int identify(nvme_ctrl_t *c, u32 cns, u32 nsid, void *buf) {
    nvme_sqe_t cmd = { 0 };
    cmd.cdw0 = ADM_IDENTIFY;
    cmd.nsid = nsid;
    cmd.prp1 = virt_to_phys(buf);               /* 4 KiB, page aligned: PRP1 only */
    cmd.cdw10 = cns;
    return admin(c, &cmd, NULL);
}

/* ---------------- controller state ---------------- */
/* wait for (CSTS & mask) == val; E_FATAL on CFS when stop_on_cfs */
static int wait_csts(nvme_ctrl_t *c, u32 mask, u32 val, u32 timeout_ms, int stop_on_cfs, u32 *out) {
    u64 deadline = time_ms() + timeout_ms;
    u32 csts;
    for (;;) {
        csts = reg_rd(c, NVME_CSTS);
        if (out) *out = csts;
        if (csts == 0xFFFFFFFF) return E_NODEV;
        if ((csts & mask) == val) return 0;
        if (stop_on_cfs && (csts & CSTS_CFS)) return E_FATAL;
        if (time_ms() > deadline) return E_TIMEOUT;
        udelay(100);
    }
}

/* CC.EN = 0 and wait for CSTS.RDY = 0: resets the controller, all queues
 * and outstanding commands (NVMe 1.4 section 7.3.2) */
static int ctrl_disable(nvme_ctrl_t *c) {
    u32 cc = reg_rd(c, NVME_CC), csts = reg_rd(c, NVME_CSTS);
    if (csts == 0xFFFFFFFF) return E_NODEV;
    if (cc & CC_EN) {
        /* EN must not be cleared while the controller is still becoming
         * ready after it was set (RDY 0): let that finish first */
        if (!(csts & (CSTS_RDY | CSTS_CFS))) wait_csts(c, CSTS_RDY, CSTS_RDY, c->rdy_timeout, 1, NULL);
        reg_wr(c, NVME_CC, cc & ~CC_EN);
    }
    return wait_csts(c, CSTS_RDY, 0, c->rdy_timeout, 0, &csts);
}

/* reset the controller, program the admin queue and enable it
 * (NVMe 1.4 section 7.6.1, controller initialization) */
static int ctrl_start(nvme_ctrl_t *c) {
    int rc = ctrl_disable(c);
    if (rc) {
        kprintf("nvme: %s: controller did not reset (%s, CSTS %08x)\n", c->name,
                rc == E_NODEV ? "not responding" : "CSTS.RDY stays set", reg_rd(c, NVME_CSTS));
        return rc;
    }
    c->intx_masked = 0;                         /* reset clears INTMS */
    c->io_ok = 0;
    queue_reset(c, &c->aq);
    reg_wr(c, NVME_AQA, ((u32)(c->aq.size - 1) << 16) | (u32)(c->aq.size - 1));
    mmio_write64(c->regs + NVME_ASQ, virt_to_phys(c->aq.sq));
    mmio_write64(c->regs + NVME_ACQ, virt_to_phys((void *)c->aq.cq));
    /* NVM command set, 4 KiB pages, round robin, 64-byte SQ and 16-byte CQ entries */
    u32 cc = CC_CSS_NVM | CC_MPS(0) | CC_AMS_RR | CC_IOSQES(6) | CC_IOCQES(4);
    reg_wr(c, NVME_CC, cc);
    reg_wr(c, NVME_CC, cc | CC_EN);
    u32 csts = 0;
    rc = wait_csts(c, CSTS_RDY, CSTS_RDY, c->rdy_timeout, 1, &csts);
    if (rc) {
        kprintf("nvme: %s: controller did not become ready (%s, CSTS %08x)\n", c->name,
                rc == E_FATAL ? "fatal status" : rc == E_NODEV ? "not responding" : "timed out", csts);
        return rc;
    }
    return 0;
}

/* create the I/O queue pair (qid 1), completion queue first */
static int ctrl_create_io(nvme_ctrl_t *c) {
    nvme_sqe_t cmd = { 0 };
    u32 res = 0;
    /* Number of Queues must be set before any I/O queue is created; one
     * pair is all we use (values are 0's based) */
    cmd.cdw0 = ADM_SET_FEATURES;
    cmd.cdw10 = FEAT_NUM_QUEUES;
    cmd.cdw11 = 0;
    int rc = admin(c, &cmd, &res);
    if (rc == E_TIMEOUT || rc == E_FATAL || rc == E_NODEV) return rc;
    if (rc) kprintf("nvme: %s: Set Features (Number of Queues) failed, continuing\n", c->name);

    queue_reset(c, &c->ioq);
    memset(&cmd, 0, sizeof cmd);
    cmd.cdw0 = ADM_CREATE_CQ;
    cmd.prp1 = virt_to_phys((void *)c->ioq.cq);
    cmd.cdw10 = ((u32)(c->ioq.size - 1) << 16) | c->ioq.qid;
    cmd.cdw11 = (0u << 16) | (1u << 1) | 1u;    /* IV 0, IEN, PC (contiguous) */
    if ((rc = admin(c, &cmd, NULL))) return rc;

    memset(&cmd, 0, sizeof cmd);
    cmd.cdw0 = ADM_CREATE_SQ;
    cmd.prp1 = virt_to_phys(c->ioq.sq);
    cmd.cdw10 = ((u32)(c->ioq.size - 1) << 16) | c->ioq.qid;
    cmd.cdw11 = ((u32)c->ioq.qid << 16) | 1u;   /* CQID 1, PC */
    if ((rc = admin(c, &cmd, NULL))) return rc;
    c->io_ok = 1;
    return 0;
}

/* after a timeout or a fatal status: reset and bring the queues back */
static int ctrl_restart(nvme_ctrl_t *c, const char *why) {
    if (c->dead) return E_NODEV;
    kprintf("nvme: %s: %s, resetting the controller\n", c->name, why);
    int rc = ctrl_start(c);
    if (!rc) rc = ctrl_create_io(c);
    if (rc) {
        c->dead = 1;
        ctrl_disable(c);
        kprintf("nvme: %s: controller reset failed, its disks are offline\n", c->name);
        return rc;
    }
    kprintf("nvme: %s: controller reset done\n", c->name);
    return 0;
}

/* ---------------- block device interface ---------------- */
static void build_prp(nvme_ctrl_t *c, u64 addr, u32 bytes, u64 *prp1, u64 *prp2) {
    *prp1 = addr;
    *prp2 = 0;
    u32 first = NVME_PAGE - (u32)(addr & (NVME_PAGE - 1));
    if (bytes <= first) return;
    u64 next = addr + first;                    /* page aligned from here on */
    u32 rest = bytes - first;
    if (rest <= NVME_PAGE) { *prp2 = next; return; }
    /* more than two pages: PRP2 points to a list of the remaining pages */
    u32 n = (rest + NVME_PAGE - 1) / NVME_PAGE;
    for (u32 i = 0; i < n; i++) c->prp_list[i] = next + (u64)i * NVME_PAGE;
    wmb();
    *prp2 = virt_to_phys(c->prp_list);
}

static int io_cmd(nvme_ns_t *ns, u8 op, u64 lba, u32 count, void *dma, u32 bytes, u32 timeout_ms) {
    nvme_ctrl_t *c = ns->c;
    nvme_sqe_t cmd = { 0 };
    cmd.cdw0 = op;
    cmd.nsid = ns->nsid;
    if (bytes) build_prp(c, virt_to_phys(dma), bytes, &cmd.prp1, &cmd.prp2);
    if (op != NVM_FLUSH) {
        cmd.cdw10 = (u32)lba;
        cmd.cdw11 = (u32)(lba >> 32);
        cmd.cdw12 = count - 1;                  /* NLB, 0's based */
    }
    for (int attempt = 0;; attempt++) {
        u16 st = 0;
        int rc = q_exec(c, &c->ioq, &cmd, timeout_ms, NULL, &st);
        if (rc == 0 || rc == E_NODEV) return rc;
        if (rc == E_IO) {
            if ((st & ST_DNR) || attempt >= NVME_RETRIES) return E_IO;
            mdelay(10u * (attempt + 1));        /* transient (e.g. namespace not ready): retry */
            continue;
        }
        /* timed out or fatal: reset, then try the command once more */
        if (ctrl_restart(c, rc == E_TIMEOUT ? "command timed out" : "controller fatal status") || attempt >= 1)
            return rc;
    }
}

static int nvme_rw(blockdev_t *bd, u64 lba, u32 count, void *buf, int write) {
    nvme_ns_t *ns = bd ? (nvme_ns_t *)bd->priv : NULL;
    if (!ns || !buf || count == 0) return E_INVAL;
    if (!bd->present) return E_NODEV;
    nvme_ctrl_t *c = ns->c;
    if (c->dead || c->shut || !c->io_ok) return E_IO;
    if (lba >= ns->blocks || count > ns->blocks - lba) return E_INVAL;
    if (c->busy) return E_BUSY;                 /* re-entered from the idle hook */
    c->busy = 1;

    u32 ss = ns->lba_size;
    int bounce = ((uptr)buf & 3) != 0;          /* PRP entries must be dword aligned */
    u32 max_sec = (bounce ? MIN(c->max_xfer, NVME_BOUNCE_SIZE) : c->max_xfer) / ss;
    if (max_sec > 65536) max_sec = 65536;       /* NLB is 16 bits */
    if (max_sec == 0) max_sec = 1;

    u8 op = write ? NVM_WRITE : NVM_READ;
    u8 *b = buf;
    int rc = 0;
    while (count) {
        u32 n = MIN(count, max_sec);
        u32 bytes = n * ss;
        void *dma = bounce ? (void *)c->bounce : (void *)b;
        if (write && bounce) memcpy(c->bounce, b, bytes);
        rc = io_cmd(ns, op, lba, n, dma, bytes, NVME_CMD_TIMEOUT);
        if (rc) break;
        if (!write && bounce) memcpy(b, c->bounce, bytes);
        b += bytes; lba += n; count -= n;
    }
    /* volatile write cache: make the data durable before returning */
    if (write && rc == 0 && c->vwc) rc = io_cmd(ns, NVM_FLUSH, 0, 0, NULL, 0, NVME_FLUSH_TIMEOUT);
    c->busy = 0;
    return rc == 0 ? 0 : rc == E_NODEV ? E_NODEV : E_IO;
}

static int nvme_read(blockdev_t *bd, u64 lba, u32 count, void *buf) {
    return nvme_rw(bd, lba, count, buf, 0);
}

static int nvme_write(blockdev_t *bd, u64 lba, u32 count, const void *buf) {
    return nvme_rw(bd, lba, count, (void *)buf, 1);
}

/* ---------------- namespaces ---------------- */
static void ns_probe(nvme_ctrl_t *c, u32 nsid, u8 *id, int quiet_inactive) {
    char sz[24];
    if (identify(c, CNS_NAMESPACE, nsid, id)) {
        kprintf("nvme: %s: namespace %u: IDENTIFY NAMESPACE failed\n", c->name, nsid);
        return;
    }
    u64 nsze = *(u64 *)(id + 0);                /* size in logical blocks */
    if (!nsze) {
        if (!quiet_inactive) kprintf("nvme: %s: namespace %u: inactive (size 0), skipped\n", c->name, nsid);
        return;
    }
    u32 nlbaf = (u32)id[25] + 1;                /* number of LBA formats (0's based) */
    u8 flbas = id[26];
    u32 fmt = flbas & 0xF;                      /* LBA format in use */
    if (nlbaf > 16) fmt |= (u32)((flbas >> 5) & 3) << 4;   /* NVMe 2.0: up to 64 formats */
    if (fmt >= nlbaf || fmt >= 64) {
        kprintf("nvme: %s: namespace %u: LBA format %u out of range (%u formats), skipped\n", c->name, nsid, fmt, nlbaf);
        return;
    }
    u32 lbaf = *(u32 *)(id + 128 + fmt * 4);
    u32 ms = lbaf & 0xFFFF, lbads = (lbaf >> 16) & 0xFF;
    if (lbads < 9 || lbads > 12) {
        kprintf("nvme: %s: namespace %u: block size 2^%u bytes unsupported, skipped\n", c->name, nsid, lbads);
        return;
    }
    if (ms) {
        /* metadata would be transferred with (extended LBA) or next to the
         * data (MPTR); neither is set up */
        kprintf("nvme: %s: namespace %u: %u bytes of metadata per block (%s) unsupported, skipped\n", c->name,
                nsid, ms, (flbas & 0x10) ? "extended LBA" : "separate buffer");
        return;
    }
    if (c->nns >= NVME_MAX_NS) {
        kprintf("nvme: %s: namespace %u: more than %d namespaces, skipped\n", c->name, nsid, NVME_MAX_NS);
        return;
    }
    u32 lba_size = 1u << lbads;
    int wp = (id[99] & 1) != 0;                 /* NSATTR: write protected */

    nvme_ns_t *ns = kzalloc(sizeof *ns);
    if (!ns) return;
    ns->c = c;
    ns->nsid = nsid;
    ns->lba_size = lba_size;
    ns->blocks = nsze;
    c->ns[c->nns++] = ns;

    fmt_size(nsze * lba_size, sz, sizeof sz);
    blockdev_t *bd = &ns->bd;
    snprintf(bd->name, sizeof bd->name, "nvme%d", g_disk_count);
    strlcpy(bd->model, c->model, sizeof bd->model);
    bd->type = BDEV_DISK_NVME;
    bd->sector_size = lba_size;
    bd->sector_count = nsze;
    bd->removable = 0;
    bd->readonly = (u8)wp;
    bd->present = 1;
    bd->read = nvme_read;
    bd->write = nvme_write;
    bd->priv = ns;
    g_disk_count++;
    kprintf("nvme: %s: namespace %u: %lu blocks x %u bytes (%s), LBA format %u of %u%s: disk %s\n", c->name,
            nsid, nsze, lba_size, sz, fmt, nlbaf, wp ? ", write protected" : "", bd->name);
    if (blockdev_register) {
        int rc = blockdev_register(bd);
        if (rc) kprintf("nvme: %s: blockdev_register failed (%d)\n", bd->name, rc);
    } else {
        kprintf("nvme: %s: no block layer linked\n", bd->name);
    }
}

static void ns_scan(nvme_ctrl_t *c, u32 nn) {
    u32 *list = (u32 *)c->bounce;               /* 1024 NSIDs */
    u8 *id = c->bounce + NVME_PAGE;
    /* active namespace list (NVMe 1.1+): IDs greater than NSID 0, ascending */
    if (c->vs >= 0x00010100 && identify(c, CNS_NS_LIST, 0, list) == 0) {
        u32 ids[NVME_MAX_NS + 1];
        int n = 0;
        for (int i = 0; i < 1024 && list[i] && n <= NVME_MAX_NS; i++) ids[n++] = list[i];
        if (n == 0) kprintf("nvme: %s: no active namespaces\n", c->name);
        for (int i = 0; i < n; i++) ns_probe(c, ids[i], id, 0);
        return;
    }
    /* older controllers: try NSIDs 1..NN, inactive ones report size 0 */
    u32 last = MIN(nn, (u32)NVME_SCAN_NS);
    for (u32 nsid = 1; nsid <= last; nsid++) ns_probe(c, nsid, id, 1);
    if (!c->nns) kprintf("nvme: %s: no usable namespaces among NSID 1..%u\n", c->name, last);
}

/* ---------------- controller bring-up ---------------- */
static void ctrl_irq_setup(nvme_ctrl_t *c) {
    snprintf(c->irq_name, sizeof c->irq_name, "NVMe %s", c->name);
    c->irq.name = c->irq_name;
    c->irq.pci = c->pci;
    c->irq.ack = nvme_ack;
    c->irq.ctx = c;
    irq_attach(&c->irq);
}

static void ctrl_free(nvme_ctrl_t *c) {
    ctrl_disable(c);                            /* no more DMA into the buffers */
    irq_detach(&c->irq);
    dma_free(c->aq.sq); dma_free((void *)c->aq.cq);
    dma_free(c->ioq.sq); dma_free((void *)c->ioq.cq);
    dma_free(c->prp_list); dma_free(c->bounce);
    c->pci->driver = NULL;
    kfree(c);
}

static void ctrl_init(pci_dev_t *d) {
    char loc[16];
    snprintf(loc, sizeof loc, "%02x:%02x.%x", d->bus, d->dev, d->func);
    if (d->bar[0].is_io || !d->bar[0].addr) {
        kprintf("nvme: %s: no memory BAR0, skipped\n", loc);
        return;
    }
    pci_enable(d);
    volatile u8 *regs = pci_map_bar(d, 0);
    if (!regs) {
        kprintf("nvme: %s: cannot map BAR0, skipped\n", loc);
        return;
    }
    nvme_ctrl_t *c = kzalloc(sizeof *c);
    if (!c) return;
    c->pci = d;
    c->regs = regs;
    strlcpy(c->name, loc, sizeof c->name);
    d->driver = "nvme";

    c->cap = mmio_read64(regs + NVME_CAP);
    c->vs = reg_rd(c, NVME_VS);
    if (c->cap == ~0ull) {
        kprintf("nvme: %s: registers read all ones, skipped\n", loc);
        d->driver = NULL;
        kfree(c);
        return;
    }
    c->dstrd = 4u << CAP_DSTRD(c->cap);
    c->rdy_timeout = MAX(CAP_TO(c->cap), 1u) * 500;
    u32 mqes = CAP_MQES(c->cap);
    char ver[16];
    if (c->vs & 0xFF) snprintf(ver, sizeof ver, "%u.%u.%u", c->vs >> 16, (c->vs >> 8) & 0xFF, c->vs & 0xFF);
    else snprintf(ver, sizeof ver, "%u.%u", c->vs >> 16, (c->vs >> 8) & 0xFF);
    kprintf("nvme: controller %s [%04x:%04x] %s, NVMe %s at %lx, max queue %u, doorbell stride %u, "
            "ready timeout %u ms, pages %u-%u KiB\n",
            loc, d->vendor, d->device, pci_vendor_name(d->vendor), ver, d->bar[0].addr, mqes, c->dstrd,
            c->rdy_timeout, 4u << CAP_MPSMIN(c->cap), 4u << CAP_MPSMAX(c->cap));

    if (!(c->cap & CAP_CSS_NVM)) {
        kprintf("nvme: %s: NVM command set not supported, skipped\n", loc);
        d->driver = NULL;
        kfree(c);
        return;
    }
    if (CAP_MPSMIN(c->cap) != 0) {
        kprintf("nvme: %s: minimum memory page size %u KiB unsupported (need 4 KiB), skipped\n", loc,
                4u << CAP_MPSMIN(c->cap));
        d->driver = NULL;
        kfree(c);
        return;
    }
    if (d->bar[0].size && d->bar[0].size < NVME_DBS + 4ull * c->dstrd) {
        kprintf("nvme: %s: BAR0 too small for the doorbells, skipped\n", loc);
        d->driver = NULL;
        kfree(c);
        return;
    }

    u16 iosz = (u16)MIN((u32)NVME_IO_QSIZE, mqes);
    u16 asz = (u16)MIN((u32)NVME_ADMIN_QSIZE, mqes);
    c->prp_list = dma_alloc(NVME_PAGE, NVME_PAGE);
    c->bounce = dma_alloc(NVME_BOUNCE_SIZE, NVME_PAGE);
    if (!c->prp_list || !c->bounce || queue_alloc(c, &c->aq, 0, asz) || queue_alloc(c, &c->ioq, 1, iosz)) {
        kprintf("nvme: %s: out of memory\n", loc);
        ctrl_free(c);
        return;
    }

    if (ctrl_start(c)) {
        ctrl_free(c);
        return;
    }

    /* Identify Controller */
    u8 *id = c->bounce;
    if (identify(c, CNS_CONTROLLER, 0, id)) {
        kprintf("nvme: %s: IDENTIFY CONTROLLER failed, skipped\n", loc);
        ctrl_free(c);
        return;
    }
    char serial[24], fw[12];
    id_string(id + 24, 40, c->model, sizeof c->model);
    id_string(id + 4, 20, serial, sizeof serial);
    id_string(id + 64, 8, fw, sizeof fw);
    u8 mdts = id[77];                           /* max transfer 2^n minimum pages, 0 = no limit */
    c->rtd3e = *(u32 *)(id + 88);
    u8 sqes = id[512], cqes = id[513];
    u32 nn = *(u32 *)(id + 516);
    c->vwc = id[525] & 1;
    c->max_xfer = NVME_MAX_XFER;
    if (mdts && mdts < 20) c->max_xfer = MIN(NVME_MAX_XFER, NVME_PAGE << mdts);
    if (!c->model[0]) strlcpy(c->model, "NVMe disk", sizeof c->model);
    char mx[24];
    if (mdts) fmt_size((u64)NVME_PAGE << MIN(mdts, 40), mx, sizeof mx);
    else strlcpy(mx, "no limit", sizeof mx);
    kprintf("nvme: %s: \"%s\" serial \"%s\" fw \"%s\"\n", loc, c->model, serial, fw);
    kprintf("nvme: %s: %u namespace(s), MDTS %s, write cache %s, queues %u/%u entries\n", loc, nn, mx,
            c->vwc ? "volatile (flushed after writes)" : "none", asz, iosz);
    if ((sqes & 0xF) > 6 || (sqes >> 4) < 6 || (cqes & 0xF) > 4 || (cqes >> 4) < 4) {
        kprintf("nvme: %s: queue entry sizes SQES %02x CQES %02x exclude 64/16 bytes, skipped\n", loc, sqes, cqes);
        ctrl_free(c);
        return;
    }

    /* interrupts, then the I/O queues: their admin commands complete with
     * the first interrupt, which proves MSI(-X) or teaches the INTx route */
    ctrl_irq_setup(c);
    int rc = ctrl_create_io(c);
    if (rc) {
        kprintf("nvme: %s: cannot create the I/O queues (%d), skipped\n", loc, rc);
        ctrl_free(c);
        return;
    }
    if (c->irq.kind != IRQ_NONE) {
        u64 end = time_ms() + 20;
        while (!irq_live(&c->irq) && time_ms() < end) irq_service();
        if (!irq_live(&c->irq)) kprintf("nvme: %s: no interrupt seen yet, polling\n", loc);
    }

    c->next = g_ctrls;
    g_ctrls = c;
    ns_scan(c, nn);
}

void nvme_init(void) {
    int found = 0;
    u64 t0 = time_ms();
    for (int idx = 0;; idx++) {
        pci_dev_t *d = pci_find_class(0x01, 0x08, -1, idx);
        if (!d) break;
        if (d->prog_if != 0x02) {
            kprintf("nvme: %02x:%02x.%x: non-volatile memory controller with prog-if %02x is not NVMe, skipped\n",
                    d->bus, d->dev, d->func, d->prog_if);
            continue;
        }
        found++;
        ctrl_init(d);
    }
    kprintf("nvme: %d controller(s), %d disk(s), init took %lu ms\n", found, g_disk_count, time_ms() - t0);
}

/* ---------------- shutdown ---------------- */
/* NVMe 1.4 section 7.6.2: stop I/O, delete the I/O submission queue, then
 * its completion queue, then set CC.SHN to normal shutdown and wait for
 * CSTS.SHST = shutdown processing complete. */
void nvme_shutdown(void) {
    for (nvme_ctrl_t *c = g_ctrls; c; c = c->next) {
        if (c->dead || c->shut || c->busy) continue;
        c->busy = 1;
        u64 t0 = time_ms();
        if (c->io_ok) {
            if (c->vwc)
                for (int i = 0; i < c->nns; i++)
                    io_cmd(c->ns[i], NVM_FLUSH, 0, 0, NULL, 0, NVME_FLUSH_TIMEOUT);
            nvme_sqe_t cmd = { 0 };
            cmd.cdw0 = ADM_DELETE_SQ;
            cmd.cdw10 = c->ioq.qid;
            int rc = admin(c, &cmd, NULL);
            if (!rc) {
                memset(&cmd, 0, sizeof cmd);
                cmd.cdw0 = ADM_DELETE_CQ;
                cmd.cdw10 = c->ioq.qid;
                admin(c, &cmd, NULL);
            }
            c->io_ok = 0;
        }
        if (c->dead) { c->busy = 0; continue; }
        reg_wr(c, NVME_CC, (reg_rd(c, NVME_CC) & ~CC_SHN_MASK) | CC_SHN_NORMAL);
        /* RTD3 entry latency is the controller's own estimate; bounded */
        u32 tmo = c->rtd3e ? c->rtd3e / 1000 + 1000 : 5000;
        tmo = MAX(tmo, 2000u);
        tmo = MIN(tmo, 15000u);
        u32 csts = 0;
        int rc = wait_csts(c, CSTS_SHST_MASK, CSTS_SHST_DONE, tmo, 0, &csts);
        c->shut = 1;
        c->busy = 0;
        if (rc) kprintf("nvme: %s: shutdown not confirmed within %u ms (CSTS %08x)\n", c->name, tmo, csts);
        else kprintf("nvme: %s: shutdown complete (%lu ms)\n", c->name, time_ms() - t0);
    }
}
