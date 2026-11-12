/* Halcyon OS - Network Connections: each adapter's status (link, address,
 * DHCP lease, traffic) and its configuration: an address from DHCP or a
 * static one, and DNS servers from DHCP or typed in. Changes apply at once
 * and are saved in SETTINGS.INI (net.<if>.*, net.dns) through the stack's
 * configuration call (net/netcfg.h). Opened from Control Panel and the
 * network icon in the taskbar's tray. */
#include "gui/wm.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/settings.h"
#include "net/netcfg.h"

#define NW_W 440
#define NW_H 544
#define MAX_IFS 8

enum { ID_IFS = 10, ID_AUTO, ID_IP, ID_MASK, ID_GW, ID_DNSAUTO, ID_DNS1, ID_DNS2, ID_APPLY, ID_RENEW };

typedef struct netwin {
    int nifs, sel;
    hal_ifinfo_t info[MAX_IFS];        /* adapters only (no loopback) */
    u64 last_refresh;
    int dirty;                         /* the user changed a field */
} netwin_t;

static window_t *nw_win;

/* ---------------- helpers ---------------- */
static void ipfmt(u32 a, char *out, int n) {
    const u8 *b = (const u8 *)&a;
    snprintf(out, n, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

/* "a.b.c.d" (network byte order) -> 1, empty -> 0 with *out 0, else -1 */
static int ipparse(const char *s, u32 *out) {
    *out = 0;
    while (*s == ' ') s++;
    if (!*s) return 0;
    u8 b[4];
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9') return -1;
        int v = 0, digits = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); if (++digits > 3 || v > 255) return -1; }
        b[i] = (u8)v;
        if (i < 3) { if (*s != '.') return -1; s++; }
    }
    while (*s == ' ') s++;
    if (*s) return -1;
    memcpy(out, b, 4);
    return 1;
}

static void fmt_bytes(u64 v, char *out, int n) {
    if (v < 10 * 1024) snprintf(out, n, "%u bytes", (unsigned)v);
    else if (v < 10ull * 1024 * 1024) snprintf(out, n, "%u KB", (unsigned)(v >> 10));
    else snprintf(out, n, "%u MB", (unsigned)(v >> 20));
}

static void fmt_duration(i64 s, char *out, int n) {
    if (s < 0) { strlcpy(out, "never", n); return; }
    if (s >= 86400) snprintf(out, n, "%d days %d hours", (int)(s / 86400), (int)(s % 86400 / 3600));
    else if (s >= 3600) snprintf(out, n, "%d hours %d minutes", (int)(s / 3600), (int)(s % 3600 / 60));
    else if (s >= 60) snprintf(out, n, "%d minutes", (int)(s / 60));
    else snprintf(out, n, "%d seconds", (int)s);
}

/* the adapters' current state; returns how many */
static int read_ifs(hal_ifinfo_t *out, int max) {
    if (!net_if_info) return 0;
    int n = 0;
    hal_ifinfo_t t;
    for (int i = 0; n < max && net_if_info(i, &t) == 0; i++)
        if (!t.loopback) out[n++] = t;
    return n;
}

/* a one-line summary of an adapter's state, for the dialog and the tray */
void net_status_text(const hal_ifinfo_t *f, char *out, int n) {
    char a[16];
    if (!f->link_up) { strlcpy(out, "Network cable unplugged", n); return; }
    if (f->conflict) { strlcpy(out, "Address conflict: another computer uses this address", n); return; }
    if (!f->addr) {
        strlcpy(out, f->mode == NET_MODE_DHCP ? "Acquiring a network address..." : "No address", n);
        return;
    }
    ipfmt(f->addr, a, sizeof a);
    snprintf(out, n, "Connected, %s", a);
}

/* ---------------- painting ---------------- */
static void row(gc_t *g, int x, int *y, const char *label, const char *val) {
    gfx_text(g, &font_ui, x, *y, label, HEX(0x4A5060));
    gfx_text_box(g, &font_ui, mkrect(x + 116, *y, NW_W - x - 116 - 24, font_ui.height), val, HEX(0x1C2230), 0);
    *y += 17;
}

static void nw_paint(window_t *w, gc_t *g) {
    netwin_t *c = w->data;
    gfx_text(g, &font_ui, 14, 12, "Connect using:", HEX(0x1C2230));
    rect_t sg = mkrect(12, 82, NW_W - 24, 176);
    draw_group(g, sg, "Status");
    draw_group(g, mkrect(12, 268, NW_W - 24, 224), "Settings");
    if (!c->nifs) {
        gfx_icon(g, sg.x + 12, sg.y + 22, &icons32[ICON_NETWORK_OFF]);
        gfx_text_box(g, &font_ui, mkrect(sg.x + 56, sg.y + 22, sg.w - 66, 60),
                     net_if_info ? "No network adapter was found. docs/HARDWARE.md lists the adapters Halcyon supports."
                                 : "This version of Halcyon has no network support.", HEX(0x1C2230), 0);
        return;
    }
    hal_ifinfo_t *f = &c->info[c->sel];
    int ok = f->link_up && f->addr;
    gfx_icon(g, sg.x + 12, sg.y + 22, &icons32[ok ? ICON_NETWORK : ICON_NETWORK_OFF]);
    char st[96], v[96], a[16], b[16];
    net_status_text(f, st, sizeof st);
    gfx_text_box(g, &font_ui_bold, mkrect(sg.x + 56, sg.y + 22, sg.w - 66, font_ui.height), st, HEX(0x1C2230), 0);
    if (f->link_up && f->speed_mbps > 0) snprintf(v, sizeof v, "%d Mbps, %s duplex", f->speed_mbps, f->full_duplex ? "full" : "half");
    else if (f->link_up) strlcpy(v, "Link up (speed not reported)", sizeof v);
    else strlcpy(v, "-", sizeof v);
    gfx_text(g, &font_ui, sg.x + 56, sg.y + 40, v, HEX(0x4A5060));
    int y = sg.y + 64, x = sg.x + 14;
    if (f->addr) { ipfmt(f->addr, a, sizeof a); ipfmt(f->mask, b, sizeof b); snprintf(v, sizeof v, "%s, mask %s", a, b); }
    else strlcpy(v, "-", sizeof v);
    row(g, x, &y, "IP address:", v);
    if (f->gateway) ipfmt(f->gateway, v, sizeof v); else strlcpy(v, "-", sizeof v);
    row(g, x, &y, "Default gateway:", v);
    v[0] = 0;
    for (int i = 0; i < 3 && f->dns[i]; i++) {
        ipfmt(f->dns[i], a, sizeof a);
        size_t l = strlen(v);
        snprintf(v + l, sizeof v - l, "%s%s", i ? ", " : "", a);
    }
    row(g, x, &y, "DNS servers:", v[0] ? v : "-");
    if (f->mode == NET_MODE_DHCP) {
        if (f->dhcp_server && f->lease_s != 0) {
            char d[48];
            ipfmt(f->dhcp_server, a, sizeof a);
            fmt_duration(f->lease_left_s, d, sizeof d);
            snprintf(v, sizeof v, "from %s, ends in %s", a, d);
        } else strlcpy(v, "no lease yet", sizeof v);
    } else strlcpy(v, "not used (static address)", sizeof v);
    row(g, x, &y, "DHCP:", v);
    snprintf(v, sizeof v, "%02X-%02X-%02X-%02X-%02X-%02X", f->mac[0], f->mac[1], f->mac[2], f->mac[3], f->mac[4], f->mac[5]);
    row(g, x, &y, "Physical address:", v);
    char s1[24], s2[24];
    fmt_bytes(f->tx_bytes, s1, sizeof s1);
    fmt_bytes(f->rx_bytes, s2, sizeof s2);
    snprintf(v, sizeof v, "sent %s, received %s", s1, s2);
    row(g, x, &y, "Activity:", v);
}

/* ---------------- the settings fields ---------------- */
static void enable(window_t *w, int id, int on) {
    widget_t *wd = widget_find(w, id);
    if (!wd) return;
    if (on) wd->style &= ~WS_DISABLED; else wd->style |= WS_DISABLED;
}

static void update_fields(window_t *w) {
    netwin_t *c = w->data;
    int have = c->nifs > 0;
    widget_t *au = widget_find(w, ID_AUTO), *da = widget_find(w, ID_DNSAUTO);
    int st = have && au && !au->value;
    enable(w, ID_AUTO, have);
    enable(w, ID_IP, st); enable(w, ID_MASK, st); enable(w, ID_GW, st);
    /* static addresses need typed-in DNS servers: DHCP gives none */
    if (st && da && da->value) da->value = 0;
    enable(w, ID_DNSAUTO, have && !st);
    int dn = have && da && !da->value;
    enable(w, ID_DNS1, dn); enable(w, ID_DNS2, dn);
    enable(w, ID_APPLY, have && c->dirty);
    enable(w, ID_RENEW, have && c->info[c->sel].mode == NET_MODE_DHCP);
    wm_invalidate(w);
}

static void set_ip_text(window_t *w, int id, u32 a) {
    char t[16] = "";
    if (a) ipfmt(a, t, sizeof t);
    widget_set_text(w, widget_find(w, id), t);
}

/* fill the fields from an adapter's configuration */
static void load_fields(window_t *w) {
    netwin_t *c = w->data;
    if (!c->nifs) { update_fields(w); return; }
    hal_ifinfo_t *f = &c->info[c->sel];
    widget_find(w, ID_AUTO)->value = f->mode == NET_MODE_DHCP;
    set_ip_text(w, ID_IP, f->mode == NET_MODE_STATIC ? f->addr : 0);
    set_ip_text(w, ID_MASK, f->mode == NET_MODE_STATIC ? f->mask : 0);
    set_ip_text(w, ID_GW, f->mode == NET_MODE_STATIC ? f->gateway : 0);
    /* typed-in DNS servers are the ones in net.dns */
    const char *dns = settings_get("net.dns", "");
    int manual = dns && *dns;
    widget_find(w, ID_DNSAUTO)->value = !manual && f->mode == NET_MODE_DHCP;
    set_ip_text(w, ID_DNS1, manual || f->mode == NET_MODE_STATIC ? f->dns[0] : 0);
    set_ip_text(w, ID_DNS2, manual || f->mode == NET_MODE_STATIC ? f->dns[1] : 0);
    c->dirty = 0;
    update_fields(w);
}

static void refresh(window_t *w) {
    netwin_t *c = w->data;
    int old = c->nifs;
    c->nifs = read_ifs(c->info, MAX_IFS);
    if (c->sel >= c->nifs) c->sel = 0;
    c->last_refresh = time_ms();
    if (c->nifs != old) {
        char *items[MAX_IFS];
        char buf[MAX_IFS][96];
        for (int i = 0; i < c->nifs; i++) {
            snprintf(buf[i], sizeof buf[i], "%s: %s", c->info[i].name, c->info[i].desc);
            items[i] = buf[i];
        }
        widget_t *lb = widget_find(w, ID_IFS);
        widget_list_set(w, lb, items, c->nifs);
        lb->value = c->nifs ? c->sel : -1;
        load_fields(w);
    }
    wm_invalidate(w);
}

/* ---------------- behaviour ---------------- */
static int field(window_t *w, int id, const char *what, u32 *out, int required) {
    widget_t *wd = widget_find(w, id);
    int r = ipparse(wd->text, out);
    if (r < 0 || (required && r == 0)) {
        char msg[200];
        if (r < 0) snprintf(msg, sizeof msg, "\"%s\" is not a valid %s.\nType it as four numbers from 0 to 255 separated by dots, "
                                             "such as 192.168.1.20.", wd->text, what);
        else snprintf(msg, sizeof msg, "Type the %s: a static address needs one.", what);
        msgbox(w, "Network Connections", msg, ICON_WARNING, MB_OK);
        w->focus = wd;
        return 0;
    }
    return 1;
}

static int apply(window_t *w) {
    netwin_t *c = w->data;
    if (!c->nifs || !net_if_config) return 1;
    if (!c->dirty) return 1;
    hal_ifinfo_t *f = &c->info[c->sel];
    hal_ifconfig_t cfg;
    memset(&cfg, 0, sizeof cfg);
    strlcpy(cfg.name, f->name, sizeof cfg.name);
    cfg.flags = NETCFG_ADDR | NETCFG_DNS | NETCFG_SAVE;
    cfg.mode = widget_find(w, ID_AUTO)->value ? NET_MODE_DHCP : NET_MODE_STATIC;
    if (cfg.mode == NET_MODE_STATIC) {
        if (!field(w, ID_IP, "IP address", &cfg.addr, 1) || !field(w, ID_MASK, "subnet mask", &cfg.mask, 1) ||
            !field(w, ID_GW, "default gateway", &cfg.gateway, 0))
            return 0;
    }
    if (!widget_find(w, ID_DNSAUTO)->value) {
        if (!field(w, ID_DNS1, "preferred DNS server", &cfg.dns[0], 0) || !field(w, ID_DNS2, "alternate DNS server", &cfg.dns[1], 0))
            return 0;
        if (!cfg.dns[0]) { cfg.dns[0] = cfg.dns[1]; cfg.dns[1] = 0; }
    }
    int r = net_if_config(&cfg);
    if (r) {
        char msg[200];
        if (r == E_INVAL)
            snprintf(msg, sizeof msg, "These settings cannot be used. Check that the IP address and the gateway are ordinary addresses "
                                      "on the same network (the subnet mask decides which addresses that is).");
        else snprintf(msg, sizeof msg, "The settings could not be changed (%s).", strerror(r));
        msgbox(w, "Network Connections", msg, ICON_ERROR, MB_OK);
        return 0;
    }
    if (!wm_alive(w)) return 1;
    c->dirty = 0;
    c->nifs = 0;                      /* reload the list and the fields */
    refresh(w);
    return 1;
}

static void on_edit(window_t *w, widget_t *wd) {
    (void)wd;
    netwin_t *c = w->data;
    c->dirty = 1;
    update_fields(w);
}
static void on_select(window_t *w, widget_t *lb) {
    netwin_t *c = w->data;
    if (lb->value < 0 || lb->value >= c->nifs || lb->value == c->sel) return;
    c->sel = lb->value;
    load_fields(w);
}
static void on_renew(window_t *w, widget_t *b) {
    (void)b;
    netwin_t *c = w->data;
    if (!c->nifs || !net_if_ctl) return;
    int r = net_if_ctl(NETCTL_RENEW, c->info[c->sel].name);
    if (r) {
        char msg[120];
        snprintf(msg, sizeof msg, "The address could not be renewed (%s).", strerror(r));
        msgbox(w, "Network Connections", msg, ICON_ERROR, MB_OK);
    }
    if (wm_alive(w)) refresh(w);
}
static void on_ok(window_t *w, widget_t *b) { (void)b; if (apply(w) && wm_alive(w)) wm_close(w); }
static void on_cancel(window_t *w, widget_t *b) { (void)b; wm_close(w); }
static void on_apply(window_t *w, widget_t *b) { (void)b; apply(w); }

static void nw_tick(window_t *w) {
    netwin_t *c = w->data;
    if (time_ms() - c->last_refresh >= 1000) refresh(w);
}

static void nw_destroy(window_t *w) {
    kfree(w->data);
    w->data = NULL;
    if (nw_win == w) nw_win = NULL;
}

void app_network(void) {
    if (nw_win && wm_alive(nw_win)) { wm_show(nw_win); return; }
    netwin_t *c = kzalloc(sizeof *c);
    if (!c) return;
    window_t *w = wm_create("Network Connections", ICON_NETWORK, -1, -1, NW_W, NW_H, WF_NO_MAXIMIZE);
    if (w) w->help = "network";              /* F1 */
    if (!w) { kfree(c); return; }
    nw_win = w;
    w->data = c;
    w->on_paint = nw_paint;
    w->on_tick = nw_tick;
    w->on_destroy = nw_destroy;

    widget_t *lb = widget_add(w, W_LISTBOX, mkrect(12, 30, NW_W - 24, 2 * 18 + 6), "", ID_IFS);
    lb->on_change = on_select;
    int y = 290;
    widget_t *au = widget_add(w, W_CHECKBOX, mkrect(26, y, 380, 18), "Obtain an IP address automatically (DHCP)", ID_AUTO);
    au->on_change = on_edit;
    static const char *labels[3] = { "IP address:", "Subnet mask:", "Default gateway:" };
    for (int i = 0; i < 3; i++) {
        y += 27;
        widget_add(w, W_LABEL, mkrect(46, y + 3, 130, 16), labels[i], 0);
        widget_t *t = widget_add(w, W_TEXTBOX, mkrect(186, y, 150, 22), "", ID_IP + i);
        t->on_change = on_edit;
    }
    y += 34;
    widget_t *da = widget_add(w, W_CHECKBOX, mkrect(26, y, 380, 18), "Obtain DNS server addresses automatically", ID_DNSAUTO);
    da->on_change = on_edit;
    static const char *dl[2] = { "Preferred DNS server:", "Alternate DNS server:" };
    for (int i = 0; i < 2; i++) {
        y += 27;
        widget_add(w, W_LABEL, mkrect(46, y + 3, 136, 16), dl[i], 0);
        widget_t *t = widget_add(w, W_TEXTBOX, mkrect(186, y, 150, 22), "", ID_DNS1 + i);
        t->on_change = on_edit;
    }

    widget_t *rn = widget_add(w, W_BUTTON, mkrect(12, NW_H - 35, 90, 23), "Renew", ID_RENEW);
    rn->on_click = on_renew;
    strlcpy(rn->tip, "Ask the DHCP server for the address again", sizeof rn->tip);
    widget_t *ok = widget_add(w, W_BUTTON, mkrect(NW_W - 12 - 3 * 75 - 2 * 8, NW_H - 35, 75, 23), "OK", ID_OK);
    ok->style |= WS_DEFAULT; ok->on_click = on_ok;
    widget_t *cn = widget_add(w, W_BUTTON, mkrect(NW_W - 12 - 2 * 75 - 8, NW_H - 35, 75, 23), "Cancel", ID_CANCEL);
    cn->style |= WS_CANCEL; cn->on_click = on_cancel;
    widget_t *ap = widget_add(w, W_BUTTON, mkrect(NW_W - 12 - 75, NW_H - 35, 75, 23), "Apply", ID_APPLY);
    ap->on_click = on_apply;
    w->focus = lb;
    c->nifs = -1;                      /* forces the first fill */
    refresh(w);
}
