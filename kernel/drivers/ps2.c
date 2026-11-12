/* Halcyon OS - PS/2 (i8042) keyboard and mouse, polled */
#include "input.h"
#include "core/io.h"
#include "core/lib.h"
#include "core/kernel.h"
#include "core/irq.h"

#define DATA 0x60
#define STAT 0x64
#define CMD  0x64

static int have_ctrl, have_kbd, have_mouse, mouse_wheel;
/* IRQ 1 / 12 only wake the main loop: ps2_poll() reads the bytes. The 8042
 * raises its (edge) IRQ again for the next byte once one has been read. */
static irq_handle_t kbd_irq = { .name = "PS/2 keyboard" }, aux_irq = { .name = "PS/2 mouse" };
static u8 mbuf[4];
static int mlen;
static int ext, pause_skip;
static u8 last_mouse_buttons;

int ps2_have_keyboard(void) { return have_kbd; }
int ps2_have_mouse(void) { return have_mouse; }

static int wait_write(void) {
    for (int i = 0; i < 100000; i++) { if (!(inb(STAT) & 2)) return 0; udelay(10); }
    return -1;
}
static int wait_read(int ms) {
    for (int i = 0; i < ms * 100; i++) { if (inb(STAT) & 1) return 0; udelay(10); }
    return -1;
}
static void cmd(u8 c) { wait_write(); outb(CMD, c); }
static void data(u8 d) { wait_write(); outb(DATA, d); }
static int read_data(int ms) { if (wait_read(ms)) return -1; return inb(DATA); }
static void flush(void) { for (int i = 0; i < 64 && (inb(STAT) & 1); i++) { inb(DATA); udelay(50); } }

static int dev_cmd(int aux, u8 c) {
    for (int tries = 0; tries < 3; tries++) {
        if (aux) cmd(0xD4);
        data(c);
        int r = read_data(50);
        if (r == 0xFA) return 0;
        if (r != 0xFE) return -1;
    }
    return -1;
}

/* Route IRQ 1 / 12 and check that they arrive: the controller's "write
 * output buffer" commands (D2 keyboard, D3 aux) raise them with a harmless
 * 00 byte. If a test fails the port stays polled until a real IRQ shows up. */
static void ps2_irqs(int cfg) {
    if (!have_kbd && !have_mouse) return;
    int k = have_kbd && irq_attach_isa(&kbd_irq, 1) != IRQ_NONE;
    int a = have_mouse && irq_attach_isa(&aux_irq, 12) != IRQ_NONE;
    if (!k && !a) return;
    cfg = (cfg & ~0x03) | (k ? 0x01 : 0) | (a ? 0x02 : 0);
    cmd(0x60); data((u8)cfg);
    for (int port = 0; port < 2; port++) {
        irq_handle_t *h = port ? &aux_irq : &kbd_irq;
        if (port ? !a : !k) continue;
        u32 before = h->events;
        u64 f = irq_save();                     /* keep the test byte away from ps2_poll */
        cmd(port ? 0xD3 : 0xD2); data(0x00);
        irq_restore(f);
        u64 end = time_ms() + 20;
        while (time_ms() < end && h->events == before) cpu_relax();
        flush();
    }
    kprintf("ps2: IRQ 1 %s, IRQ 12 %s\n", !k ? "unused" : kbd_irq.events ? "works" : "silent (polled)",
            !a ? "unused" : aux_irq.events ? "works" : "silent (polled)");
}

void ps2_init(void) {
    u8 st = inb(STAT);
    if (st == 0xFF) { kprintf("ps2: no controller\n"); return; }
    cmd(0xAD);            /* disable keyboard port */
    cmd(0xA7);            /* disable aux port */
    flush();
    cmd(0x20);
    int cfg = read_data(20);
    if (cfg < 0) { kprintf("ps2: controller not responding\n"); return; }
    have_ctrl = 1;
    cfg &= ~0x03;          /* no IRQs while initialising (replies are read by polling) */
    cfg |= 0x40;           /* scancode translation -> set 1 */
    cfg &= ~0x30;          /* clocks enabled */
    cmd(0x60); data((u8)cfg);

    cmd(0xAE);             /* enable keyboard */
    flush();
    if (dev_cmd(0, 0xF4) == 0) have_kbd = 1;   /* enable scanning */
    flush();

    cmd(0xA8);             /* enable aux */
    flush();
    if (dev_cmd(1, 0xF6) == 0) {               /* set defaults */
        /* IntelliMouse wheel detection: sample rates 200, 100, 80 */
        dev_cmd(1, 0xF3); dev_cmd(1, 200);
        dev_cmd(1, 0xF3); dev_cmd(1, 100);
        dev_cmd(1, 0xF3); dev_cmd(1, 80);
        if (dev_cmd(1, 0xF2) == 0) {
            int id = read_data(50);
            if (id == 3 || id == 4) mouse_wheel = 1;
        }
        dev_cmd(1, 0xF3); dev_cmd(1, 100);
        if (dev_cmd(1, 0xF4) == 0) have_mouse = 1;
    }
    flush();
    ps2_irqs(cfg);
    kprintf("ps2: keyboard %s, mouse %s%s\n", have_kbd ? "yes" : "no", have_mouse ? "yes" : "no",
            mouse_wheel ? " (wheel)" : "");
}

/* set 1 scancode -> HID usage. 0x2B is both "\|" and the ISO key left of
 * Enter (layouts treat usage 0x31 and 0x32 alike); 0x56 is the ISO key
 * between left Shift and Z; 0x73 and 0x7E are the Brazilian ABNT2 "/?" key
 * and keypad "."; 0x70, 0x79, 0x7B, 0x7D are the Japanese keys. */
static const u8 set1[0x80] = {
    0, 0x29, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2D, 0x2E, 0x2A, 0x2B,
    0x14, 0x1A, 0x08, 0x15, 0x17, 0x1C, 0x18, 0x0C, 0x12, 0x13, 0x2F, 0x30, 0x28, 0xE0, 0x04, 0x16,
    0x07, 0x09, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F, 0x33, 0x34, 0x35, 0xE1, 0x31, 0x1D, 0x1B, 0x06, 0x19,
    0x05, 0x11, 0x10, 0x36, 0x37, 0x38, 0xE5, 0x55, 0xE2, 0x2C, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E,
    0x3F, 0x40, 0x41, 0x42, 0x43, 0x53, 0x47, 0x5F, 0x60, 0x61, 0x56, 0x5C, 0x5D, 0x5E, 0x57, 0x59,
    0x5A, 0x5B, 0x62, 0x63, 0, 0, 0x64, 0x44, 0x45, 0x67,
    [0x70] = 0x88, [0x73] = 0x87, [0x79] = 0x8A, [0x7B] = 0x8B, [0x7D] = 0x89, [0x7E] = 0x85,
};

static u8 ext_key(u8 sc) {
    switch (sc) {
    case 0x1C: return KEY_KP_ENTER;
    case 0x1D: return KEY_RCTRL;
    case 0x35: return KEY_KP_SLASH;
    case 0x37: return KEY_PRINTSCREEN;
    case 0x38: return KEY_RALT;
    case 0x47: return KEY_HOME;
    case 0x48: return KEY_UP;
    case 0x49: return KEY_PAGEUP;
    case 0x4B: return KEY_LEFT;
    case 0x4D: return KEY_RIGHT;
    case 0x4F: return KEY_END;
    case 0x50: return KEY_DOWN;
    case 0x51: return KEY_PAGEDOWN;
    case 0x52: return KEY_INSERT;
    case 0x53: return KEY_DELETE;
    case 0x5B: return KEY_LGUI;
    case 0x5C: return KEY_RGUI;
    case 0x5D: return KEY_MENU;
    case 0x20: return 0x7F;          /* multimedia keys: Mute, Volume Down, Volume Up (HID 0x7F-0x81) */
    case 0x2E: return 0x81;
    case 0x30: return 0x80;
    }
    return 0;
}

static void kbd_byte(u8 b) {
    if (pause_skip) { pause_skip--; return; }
    if (b == 0xE0) { ext = 1; return; }
    if (b == 0xE1) { pause_skip = 5; input_key(KEY_PAUSE, 1); input_key(KEY_PAUSE, 0); return; }
    int released = b & 0x80;
    u8 sc = b & 0x7F;
    u8 key;
    if (ext) {
        ext = 0;
        if (sc == 0x2A || sc == 0x36) return;   /* fake shifts */
        key = ext_key(sc);
    } else {
        key = sc < sizeof set1 ? set1[sc] : 0;
    }
    if (key) input_key(key, !released);
}

static void mouse_byte(u8 b) {
    if (mlen == 0 && !(b & 0x08)) return;   /* resync: bit 3 always set in byte 0 */
    mbuf[mlen++] = b;
    int need = mouse_wheel ? 4 : 3;
    if (mlen < need) return;
    mlen = 0;
    if (mbuf[0] & 0xC0) return;             /* overflow */
    int dx = mbuf[1] - ((mbuf[0] & 0x10) ? 256 : 0);
    int dy = mbuf[2] - ((mbuf[0] & 0x20) ? 256 : 0);
    int dz = 0;
    if (mouse_wheel) dz = -(int)(i8)((mbuf[3] & 0x08) ? (mbuf[3] | 0xF0) : (mbuf[3] & 0x0F));
    u8 btn = 0;
    if (mbuf[0] & 1) btn |= MOUSE_LEFT;
    if (mbuf[0] & 2) btn |= MOUSE_RIGHT;
    if (mbuf[0] & 4) btn |= MOUSE_MIDDLE;
    last_mouse_buttons = btn;
    input_mouse_rel(dx, -dy, dz, btn);
}

void ps2_poll(void) {
    if (!have_ctrl) return;
    if ((have_kbd && !irq_live(&kbd_irq)) || (have_mouse && !irq_live(&aux_irq))) idle_wake_at(time_ms() + 10);
    for (int i = 0; i < 64; i++) {
        u8 st = inb(STAT);
        if (!(st & 1)) break;
        u8 b = inb(DATA);
        if (st & 0x20) { if (have_mouse) mouse_byte(b); }
        else kbd_byte(b);
    }
}
