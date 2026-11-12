/* MPLAYER.EXE - Halcyon Media Player: plays WAV files (PCM 8/16/24/32-bit,
 * IEEE float, IMA ADPCM, mu-law, A-law; any rate; more than two channels
 * mixed down) through a sound stream, with play / pause / stop, a seek bar,
 * position and length, a level meter and its own volume. Explorer and the
 * desktop open .wav files with it.
 *
 * One thread: poll() waits for the program's window events and for room in
 * the sound stream (POLLOUT) at once; the file is decoded (userland/lib/wav.c)
 * a few thousand frames at a time into the non-blocking stream. The
 * position is the stream's played frames since the last seek. */
#include "hal.h"
#include "wav.h"

#define W 460
#define H 198
#define STATUS_H 22
#define PATHN 300

enum { ID_PLAY = 10, ID_STOP, ID_OPEN, C_STATUS,
       M_OPEN = 100, M_CLOSE, M_EXIT, M_PLAY, M_STOP, M_BACK, M_AHEAD, M_VOLUP, M_VOLDOWN, M_ABOUT };
enum { ST_NONE, ST_STOPPED, ST_PLAYING, ST_PAUSED };

static const hal_menu_item_t bar[] = {
    { "&File", 0, 0, MENU_TITLE, 0, 0 },
    { "&Open...", "Ctrl+O", M_OPEN, 0, HICON_FOLDER_OPEN, 0 },
    { "&Close", 0, M_CLOSE, 0, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "E&xit", 0, M_EXIT, 0, 0, 0 },
    { "&Play", 0, 0, MENU_TITLE, 0, 0 },
    { "&Play/Pause", "Space", M_PLAY, MENU_BOLD, 0, 0 },
    { "&Stop", 0, M_STOP, 0, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "Skip &back 5 seconds", "Left", M_BACK, 0, 0, 0 },
    { "Skip &ahead 5 seconds", "Right", M_AHEAD, 0, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "Volume &up", "Up", M_VOLUP, 0, 0, 0 },
    { "Volume &down", "Down", M_VOLDOWN, 0, 0, 0 },
    { "&Help", 0, 0, MENU_TITLE, 0, 0 },
    { "&About Media Player", 0, M_ABOUT, 0, HICON_MEDIA, 0 },
};
#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))

static int win;
static hal_theme_t th;
static int state = ST_NONE;
static char path[PATHN], fname[PATHN];
static char *filedata;
static wav_t wv;
static int oc, rate, rep = 1, dec = 1;
static int snd = -1;                       /* the stream (opened per file) */
static uint64_t dpos;                      /* next file frame to decode */
static uint64_t seek_base, played_base;    /* the file frame and the stream's played count at the last seek */
static uint64_t stop_pos;
static int eof;
static int volume = 100;
static int seek_drag = -1, vol_drag;       /* seek_drag: the file frame shown while dragging */
static int level[2];                       /* meter, 0..1000 */
static char status_text[96];

/* ---------------- layout ---------------- */
static const hal_rect_t R_DISPLAY = { 8, 8, W - 16, 78 };
static const hal_rect_t R_SEEK = { 12, 94, W - 24, 22 };
static const hal_rect_t R_VOL = { W - 12 - 104, 128, 104, 20 };

static int in_rect(hal_rect_t r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

static void fmt_time(uint64_t frames, char *out, int n) {
    unsigned long long s = wv.rate ? frames / (uint64_t)wv.rate : 0;
    if (s >= 3600) snprintf(out, (size_t)n, "%llu:%02llu:%02llu", s / 3600, s / 60 % 60, s % 60);
    else snprintf(out, (size_t)n, "%llu:%02llu", s / 60, s % 60);
}

static unsigned isqrt(unsigned v) {
    unsigned r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
    return r;
}

/* ---------------- the stream ---------------- */
static uint64_t position(void) {
    if (seek_drag >= 0) return (uint64_t)seek_drag;
    if (state == ST_STOPPED || state == ST_NONE || snd < 0) return stop_pos;
    hal_sndstatus_t st;
    if (snd_status(snd, &st)) return seek_base;
    uint64_t p = seek_base + (st.played - played_base) * (uint64_t)dec / (uint64_t)rep;
    return p < wv.frames ? p : wv.frames;
}

static void set_status(const char *s) {
    if (!strcmp(s, status_text)) return;
    strlcpy(status_text, s, sizeof status_text);
    char t[128];
    snprintf(t, sizeof t, "%s\tVolume %d%%", s, volume);
    ctl_set_text(win, C_STATUS, t);
}

/* decode into the stream while it has room (non-blocking) */
static void fill(void) {
    static int16_t in[4096 * 2], out[4096 * 2];
    if (snd < 0 || state != ST_PLAYING) return;
    for (int round = 0; round < 16 && !eof; round++) {
        hal_sndstatus_t st;
        if (snd_status(snd, &st)) return;
        long room = (long)st.space / (oc * 2);              /* stream frames */
        long want = rep > 1 ? room / rep : room * dec;      /* file frames */
        long cap = rep > 1 ? 4096 / rep : 4096;              /* (in[] holds 4096 frames) */
        if (want > cap) want = cap;
        if (want < 1 || (room < 256 && wv.frames - dpos > (uint64_t)want)) return;
        long n = wav_read(&wv, dpos, in, want, oc);
        if (n <= 0) { eof = 1; return; }
        dpos += (uint64_t)n;
        long o = 0;
        for (long i = 0; i < n; i += dec) {
            for (int c = 0; c < oc; c++) {
                long s = 0;
                int k = 0;
                for (; k < dec && i + k < n; k++) s += in[(i + k) * oc + c];
                for (int j = 0; j < rep; j++) out[(o + j) * oc + c] = (int16_t)(s / k);
            }
            o += rep;
        }
        long r = snd_write_nb(snd, out, (size_t)o * oc * 2);
        if (r < 0 && r != E_AGAIN) { set_status("The sound card stopped taking data"); eof = 1; return; }
        if (dpos >= wv.frames) eof = 1;
    }
}

static void restart_at(uint64_t f) {
    hal_sndstatus_t st;
    snd_discard(snd);
    snd_status(snd, &st);
    played_base = st.played;
    seek_base = dpos = f;
    eof = f >= wv.frames;
}

/* fill the stream before it starts (or goes on after a seek), so that it
 * does not run dry while the first pieces are decoded */
static void prime(void) {
    snd_pause(snd, 1);
    fill();
    if (state == ST_PLAYING) snd_pause(snd, 0);
}

/* ---------------- drawing ---------------- */
static void paint_display(void) {
    hal_rect_t r = R_DISPLAY;
    gfx_fill(win, r.x - 8, 0, W, r.y + r.h + 2, th.face);
    gfx_shade(win, r.x, r.y, r.w, r.h, RGB(0x1C, 0x2C, 0x4E), RGB(0x08, 0x10, 0x22), SHADE_ROUNDED, 6);
    gfx_rounded_outline(win, r.x, r.y, r.w, r.h, 6, RGB(0x04, 0x08, 0x14));
    char t[160], a[32], b[32];
    if (state == ST_NONE) {
        gfx_icon(win, r.x + 12, r.y + 12, HICON_MEDIA | 32 << 16);
        gfx_text(win, r.x + 54, r.y + 14, "No file open", RGB(0xFF, 0xFF, 0xFF), FONT_UI_BOLD);
        gfx_text(win, r.x + 54, r.y + 32, "File > Open, or open a .wav file in Explorer.", RGB(0x9F, 0xB4, 0xD8), FONT_UI);
        win_invalidate(win);
        return;
    }
    gfx_text_box(win, r.x + 12, r.y + 8, r.w - 150, 18, fname, RGB(0xFF, 0xFF, 0xFF), FONT_UI_BOLD, 0);
    const char *chs = wv.channels == 1 ? "mono" : wv.channels == 2 ? "stereo" : "";
    char chn[24];
    if (!*chs) { snprintf(chn, sizeof chn, "%d channels", wv.channels); chs = chn; }
    if (wv.format == WAV_IMA_ADPCM || wv.format == WAV_MULAW || wv.format == WAV_ALAW)
        snprintf(t, sizeof t, "%s, %d Hz, %s%s", wav_format_name(&wv), wv.rate, chs, wv.truncated ? " (incomplete file)" : "");
    else snprintf(t, sizeof t, "%s %d-bit, %d Hz, %s%s", wav_format_name(&wv), wv.bits, wv.rate, chs, wv.truncated ? " (incomplete file)" : "");
    gfx_text_box(win, r.x + 12, r.y + 27, r.w - 150, 16, t, RGB(0x9F, 0xB4, 0xD8), FONT_UI, 0);
    fmt_time(position(), a, sizeof a);
    fmt_time(wv.frames, b, sizeof b);
    snprintf(t, sizeof t, "%s / %s", a, b);
    gfx_text_box(win, r.x + r.w - 140, r.y + 8, 128, 26, t, RGB(0xE8, 0xF0, 0xFF), FONT_LARGE, 2);
    /* the level meter: two bars, green to yellow to red */
    for (int c = 0; c < 2; c++) {
        int bx = r.x + 12, by = r.y + 52 + c * 10, bw = r.w - 24, bh = 6;
        gfx_fill(win, bx, by, bw, bh, RGB(0x10, 0x1A, 0x30));
        int lit = bw * level[oc == 1 ? 0 : c] / 1000;
        for (int x = 0; x < lit; x += 4) {
            int f = x * 100 / bw;
            unsigned col = f < 65 ? RGB(0x40, 0xD0, 0x60) : f < 85 ? RGB(0xF0, 0xD0, 0x40) : RGB(0xF0, 0x50, 0x40);
            gfx_fill(win, bx + x, by, x + 3 < lit ? 3 : lit - x, bh, col);
        }
    }
    win_invalidate(win);
}

static void paint_seek(void) {
    hal_rect_t r = R_SEEK;
    gfx_fill(win, 0, r.y - 2, W, r.h + 4, th.face);
    int cy = r.y + r.h / 2;
    gfx_shade(win, r.x, cy - 3, r.w, 6, RGB(0xF4, 0xF3, 0xEE), 0, SHADE_SUNKEN, 0);
    if (state != ST_NONE && wv.frames) {
        int x = r.x + (int)((uint64_t)(r.w - 12) * position() / wv.frames);
        gfx_fill(win, r.x + 1, cy - 2, x + 6 - r.x - 1, 4, th.select);
        gfx_rounded(win, x, r.y + 2, 12, r.h - 4, 3, RGB(0xFF, 0xFF, 0xFF), RGB(0xE3, 0xE1, 0xD8));
        gfx_rounded_outline(win, x, r.y + 2, 12, r.h - 4, 3, th.button_border);
        gfx_line(win, x + 4, cy - 3, x + 4, cy + 3, RGB(0x9A, 0xA4, 0xB8));
        gfx_line(win, x + 7, cy - 3, x + 7, cy + 3, RGB(0x9A, 0xA4, 0xB8));
    }
    win_invalidate(win);
}

static void paint_volume(void) {
    hal_rect_t r = R_VOL;
    gfx_fill(win, r.x - 26, r.y - 2, r.w + 30, r.h + 4, th.face);
    gfx_icon(win, r.x - 24, r.y + 2, volume ? HICON_SPEAKER : HICON_SPEAKER_MUTED);
    int cy = r.y + r.h / 2;
    gfx_shade(win, r.x + 4, cy - 2, r.w - 8, 4, RGB(0xF4, 0xF3, 0xEE), 0, SHADE_SUNKEN, 0);
    int x = r.x + (r.w - 10) * volume / 100;
    gfx_fill(win, r.x + 5, cy - 1, x - r.x, 2, th.select);
    gfx_rounded(win, x, r.y + 2, 10, r.h - 4, 2, RGB(0xFF, 0xFF, 0xFF), RGB(0xE3, 0xE1, 0xD8));
    gfx_rounded_outline(win, x, r.y + 2, 10, r.h - 4, 2, th.button_border);
    win_invalidate(win);
}

static void paint_all(void) {
    gfx_theme(&th);
    gfx_fill(win, 0, 0, W, H - STATUS_H, th.face);
    paint_display();
    paint_seek();
    paint_volume();
}

static void update_controls(void) {
    ctl_set_text(win, ID_PLAY, state == ST_PLAYING ? "Pause" : "Play");
    ctl_set_style(win, ID_PLAY, CTL_DEFAULT | (state == ST_NONE ? CTL_DISABLED : 0));
    ctl_set_style(win, ID_STOP, state == ST_PLAYING || state == ST_PAUSED ? 0 : CTL_DISABLED);
    menu_enable(win, M_PLAY, state != ST_NONE);
    menu_enable(win, M_STOP, state == ST_PLAYING || state == ST_PAUSED);
    menu_enable(win, M_CLOSE, state != ST_NONE);
    menu_enable(win, M_BACK, state != ST_NONE);
    menu_enable(win, M_AHEAD, state != ST_NONE);
    set_status(state == ST_NONE ? "Ready" : state == ST_PLAYING ? "Playing" : state == ST_PAUSED ? "Paused" : "Stopped");
}

/* the meter follows the samples at the playing position */
static void meter(void) {
    int target[2] = { 0, 0 };
    if (state == ST_PLAYING) {
        int16_t buf[1024 * 2];
        long n = wav_read(&wv, position(), buf, 1024, oc);
        for (long i = 0; i < n * oc; i++) {
            int c = (int)(i % oc), v = buf[i] < 0 ? -buf[i] : buf[i];
            if (v > target[c]) target[c] = v;
        }
        for (int c = 0; c < 2; c++) target[c] = (int)(isqrt((unsigned)target[c] * 30) * 1000 / 991);   /* sqrt(32768 * 30) = 991 */
    }
    for (int c = 0; c < 2; c++) level[c] = target[c] >= level[c] ? target[c] : level[c] - (level[c] - target[c]) / 3 - 10;
    for (int c = 0; c < 2; c++) if (level[c] < 0) level[c] = 0; else if (level[c] > 1000) level[c] = 1000;
}

/* ---------------- files ---------------- */
static void close_file(void) {
    if (snd >= 0) snd_close(snd, SNDCLOSE_NOW);
    snd = -1;
    free(filedata);
    filedata = NULL;
    memset(&wv, 0, sizeof wv);
    state = ST_NONE;
    stop_pos = 0;
    level[0] = level[1] = 0;
    win_set_title(win, "Media Player");
    update_controls();
    paint_all();
}

static void open_file(const char *p, int autoplay) {
    char *data;
    long size;
    char msg[PATHN + 300];
    int r = read_file(p, &data, &size);
    if (r < 0) {
        snprintf(msg, sizeof msg, "Media Player cannot open \"%s\":\n%s.", p, hal_strerror(r));
        msgbox(win, "Media Player", msg, MBX_ICON_ERROR, MBX_OK);
        return;
    }
    wav_t w;
    r = wav_open(&w, data, (size_t)size);
    if (r == WAV_E_FORMAT) {
        snprintf(msg, sizeof msg, "\"%s\" uses %s compression, which Media Player cannot play.\n\n"
                 "Media Player plays WAV files with PCM (8, 16, 24 or 32-bit), IEEE float, IMA ADPCM, mu-law or A-law sound.",
                 p, wav_tag_name(w.tag));
    } else if (r) {
        snprintf(msg, sizeof msg, "Media Player cannot play \"%s\": %s.", p, wav_error(r));
    }
    int sr = 0;
    if (!r) {
        sr = wav_stream_rate(&w, SND_RATE_MIN, SND_RATE_MAX, &rep, &dec);
        if (!sr) { snprintf(msg, sizeof msg, "\"%s\" has a sample rate of %d Hz, which cannot be played.", p, w.rate); r = -1; }
    }
    if (r) {
        free(data);
        msgbox(win, "Media Player", msg, r == WAV_E_FORMAT ? MBX_ICON_WARNING : MBX_ICON_ERROR, MBX_OK);
        return;
    }
    close_file();
    filedata = data;
    wv = w;
    rate = sr;
    oc = wav_out_channels(&wv);
    strlcpy(path, p, sizeof path);
    const char *base = strrchr(p, '/');
    const char *b2 = strrchr(p, '\\');
    if (b2 && (!base || b2 > base)) base = b2;
    strlcpy(fname, base ? base + 1 : p, sizeof fname);
    char title[PATHN + 20];
    snprintf(title, sizeof title, "%s - Media Player", fname);
    win_set_title(win, title);
    snd = snd_open(rate, oc, 16, 300, SND_NONBLOCK);
    if (snd >= 0) snd_stream_volume(snd, volume);
    state = ST_STOPPED;
    stop_pos = 0;
    update_controls();
    if (snd < 0) {
        snprintf(msg, sizeof msg, "%s", snd == E_NODEV ? "There is no sound card: Media Player can show the file but not play it."
                                                       : "The sound card cannot play it now.");
        set_status(msg);
    }
    paint_all();
    if (autoplay && snd >= 0) {
        restart_at(0);
        state = ST_PLAYING;
        prime();
        update_controls();
    }
}

static void open_dialog(void) {
    char p[PATHN], dir[PATHN] = "C:/Documents";
    if (path[0]) {
        strlcpy(dir, path, sizeof dir);
        char *s = strrchr(dir, '/');
        if (s) *s = 0;
    }
    if (file_dialog(win, 0, "Open", dir, "wav", p, sizeof p)) open_file(p, 1);
}

/* ---------------- commands ---------------- */
static void play_pause(void) {
    if (state == ST_NONE) { open_dialog(); return; }
    if (snd < 0) { msgbox(win, "Media Player", "There is no sound card to play it on.", MBX_ICON_WARNING, MBX_OK); return; }
    if (state == ST_PLAYING) { snd_pause(snd, 1); state = ST_PAUSED; }
    else if (state == ST_PAUSED) { snd_pause(snd, 0); state = ST_PLAYING; fill(); }
    else {
        restart_at(stop_pos >= wv.frames ? 0 : stop_pos);
        state = ST_PLAYING;
        prime();
    }
    update_controls();
    paint_display();
}

static void stop(void) {
    if (state != ST_PLAYING && state != ST_PAUSED) return;
    snd_discard(snd);
    snd_pause(snd, 0);
    state = ST_STOPPED;
    stop_pos = 0;
    update_controls();
    paint_display();
    paint_seek();
}

static void seek_to(uint64_t f) {
    if (state == ST_NONE) return;
    if (f > wv.frames) f = wv.frames;
    if (state == ST_STOPPED || snd < 0) stop_pos = f;
    else {
        restart_at(f);
        if (state == ST_PLAYING) prime();
    }
    paint_display();
    paint_seek();
}

static void skip(int seconds) {
    if (state == ST_NONE) return;
    int64_t f = (int64_t)position() + (int64_t)seconds * wv.rate;
    seek_to(f < 0 ? 0 : (uint64_t)f);
}

static void set_volume(int v) {
    volume = v < 0 ? 0 : v > 100 ? 100 : v;
    if (snd >= 0) snd_stream_volume(snd, volume);
    status_text[0] = 0;
    update_controls();
    paint_volume();
}

static void command(int id) {
    switch (id) {
    case M_OPEN: case ID_OPEN: open_dialog(); break;
    case M_CLOSE: close_file(); break;
    case M_EXIT: exit(0);
    case M_PLAY: case ID_PLAY: play_pause(); break;
    case M_STOP: case ID_STOP: stop(); break;
    case M_BACK: skip(-5); break;
    case M_AHEAD: skip(5); break;
    case M_VOLUP: set_volume(volume + 5); break;
    case M_VOLDOWN: set_volume(volume - 5); break;
    case M_ABOUT:
        msgbox(win, "About Media Player", "Halcyon Media Player\n\nPlays WAV files: PCM 8, 16, 24 and 32-bit, IEEE float, "
               "IMA ADPCM, mu-law and A-law, at any rate, mono, stereo or more channels.", MBX_ICON_INFO, MBX_OK);
        break;
    }
}

static uint64_t seek_frame_at(int x) {
    if (!wv.frames) return 0;
    long p = x - R_SEEK.x - 6;
    if (p < 0) p = 0;
    if (p > R_SEEK.w - 12) p = R_SEEK.w - 12;
    return (uint64_t)p * wv.frames / (uint64_t)(R_SEEK.w - 12);
}

static void handle(hal_event_t *ev) {
    switch (ev->type) {
    case EVT_CLOSE: exit(0);
    case EVT_PAINT: paint_all(); break;
    case EVT_MENU: command(ev->id); break;
    case EVT_COMMAND: command(ev->id); ctl_focus(win, 0); break;
    case EVT_DRAG: {                            /* a .wav file dropped on the window plays */
        char p[PATHN];
        if (dnd_open_target(ev, "wav;wave", p, sizeof p)) open_file(p, 1);
        break;
    }
    case EVT_MOUSE_DOWN:
        if (ev->button != MOUSE_LEFT) break;
        if (state != ST_NONE && in_rect(R_SEEK, ev->x, ev->y)) { seek_drag = (int)seek_frame_at(ev->x); paint_seek(); paint_display(); }
        else if (in_rect((hal_rect_t){ R_VOL.x - 4, R_VOL.y - 4, R_VOL.w + 8, R_VOL.h + 8 }, ev->x, ev->y)) {
            vol_drag = 1;
            set_volume((ev->x - R_VOL.x - 5) * 100 / (R_VOL.w - 10));
        }
        break;
    case EVT_MOUSE_MOVE:
        if (seek_drag >= 0) { seek_drag = (int)seek_frame_at(ev->x); paint_seek(); paint_display(); }
        if (vol_drag) set_volume((ev->x - R_VOL.x - 5) * 100 / (R_VOL.w - 10));
        break;
    case EVT_MOUSE_UP:
        if (seek_drag >= 0) { uint64_t f = (uint64_t)seek_drag; seek_drag = -1; seek_to(f); }
        vol_drag = 0;
        break;
    case EVT_WHEEL:
        set_volume(volume + (ev->dz > 0 ? 5 : -5));
        break;
    case EVT_KEY:
        if (!ev->pressed) break;
        if (key_shortcut(ev, 'O')) { open_dialog(); break; }
        switch (ev->id) {
        case KEY_SPACE: play_pause(); break;
        case KEY_LEFT: skip(-5); break;
        case KEY_RIGHT: skip(5); break;
        case KEY_UP: set_volume(volume + 5); break;
        case KEY_DOWN: set_volume(volume - 5); break;
        case KEY_HOME: seek_to(0); break;
        }
        break;
    }
}

int main(int argc, char **argv) {
    win = win_create("Media Player", W, H, WIN_NO_MAX);
    if (win < 0) { printf("Media Player needs the desktop; PLAY file.wav plays from the Command Prompt.\n"); return 1; }
    win_set_icon(win, HICON_MEDIA);
    menu_set(win, bar, N(bar));
    int by = 124;
    ctl_create(win, CTL_BUTTON, 12, by, 72, 26, "Play", ID_PLAY, CTL_DEFAULT);
    ctl_create(win, CTL_BUTTON, 90, by, 72, 26, "Stop", ID_STOP, 0);
    ctl_create(win, CTL_BUTTON, 168, by, 80, 26, "Open...", ID_OPEN, 0);
    ctl_create(win, CTL_STATUSBAR, 0, 0, 0, STATUS_H, "", C_STATUS, 0);
    ctl_set_value(win, C_STATUS, 110);
    ctl_set_tool(win, ID_STOP, -1, "Stop and go back to the start");
    update_controls();
    paint_all();
    ctl_focus(win, 0);
    if (argc > 1) {
        char p[PATHN] = "";
        for (int i = 1; i < argc; i++) {
            if (i > 1) strlcat(p, " ", sizeof p);
            strlcat(p, argv[i], sizeof p);
        }
        if (p[0]) open_file(p, 1);
    }
    uint64_t last_ui = 0;
    for (;;) {
        struct pollfd pf[2];
        int n = 0;
        pf[n++] = (struct pollfd){ FD_WINDOWS, POLLIN, 0 };
        int feeding = state == ST_PLAYING && !eof && snd >= 0;
        if (feeding) pf[n++] = (struct pollfd){ snd, POLLOUT, 0 };
        poll(pf, (unsigned)n, state == ST_PLAYING ? 100 : -1);
        hal_event_t ev;
        for (int k = 0; k < 64 && win_get_event(&ev, 0); k++) handle(&ev);
        if (state == ST_PLAYING) {
            fill();
            if (eof) {
                hal_sndstatus_t st;
                if (snd_status(snd, &st) == 0 && st.queued <= 1) {      /* played to the end */
                    snd_discard(snd);
                    state = ST_STOPPED;
                    stop_pos = 0;
                    level[0] = level[1] = 0;
                    update_controls();
                    paint_display();
                    paint_seek();
                }
            }
            if (time_ms() - last_ui >= 100) {
                last_ui = time_ms();
                meter();
                paint_display();
                paint_seek();
            }
        }
    }
}
