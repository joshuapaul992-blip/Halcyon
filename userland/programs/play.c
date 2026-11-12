/* PLAY.EXE - play a WAV file from the Command Prompt, and a few sound
 * settings:
 *   PLAY file.wav [/V percent]   play it (stream volume), wait until it ends
 *   PLAY /E event                a system sound (startup, error, notify...)
 *   PLAY /VOLUME [percent|MUTE|UNMUTE]   show or set the master volume (saved)
 *   PLAY                         the sound cards and the master volume */
#include "hal.h"
#include "wav.h"

static const char *events[SNDEV_COUNT] = { "startup", "shutdown", "error", "warning", "info", "question", "notify",
                                           "devadd", "devremove", "default" };

static void usage(void) {
    printf("usage: PLAY file.wav [/V percent]    play a WAV file\n"
           "       PLAY /E event                 a system sound:");
    for (int i = 0; i < SNDEV_COUNT; i++) printf(" %s", events[i]);
    printf("\n       PLAY /VOLUME [percent|MUTE|UNMUTE]   the master volume\n");
}

static void devices(void) {
    hal_snddev_t d;
    int n = 0;
    for (int i = 0; snd_devinfo(i, &d) == 0; i++, n++)
        printf("%s: %s (%s), %s, %d Hz, %s%s\n", d.name, d.desc, d.driver, d.output[0] ? d.output : "output", d.rate,
               d.running ? "playing" : "idle", d.is_default ? ", default" : "");
    if (!n) printf("No sound card.\n");
    int m = snd_master(-1, -1, 0);
    if (m >= 0) printf("Master volume: %d%%%s\n", m & 0xFF, m & SND_MUTED ? " (muted)" : "");
}

static int play_file(const char *path, int volume) {
    char *data;
    long size;
    int r = read_file(path, &data, &size);
    if (r < 0) { printf("PLAY: %s: %s\n", path, hal_strerror(r)); return 1; }
    wav_t w;
    r = wav_open(&w, data, (size_t)size);
    if (r) {
        if (r == WAV_E_FORMAT) printf("PLAY: %s: %s compression cannot be played\n", path, wav_tag_name(w.tag));
        else printf("PLAY: %s: %s\n", path, wav_error(r));
        return 1;
    }
    int rep, dec, oc = wav_out_channels(&w);
    int rate = wav_stream_rate(&w, SND_RATE_MIN, SND_RATE_MAX, &rep, &dec);
    unsigned long long secs = w.frames / (unsigned long long)w.rate;
    printf("%s: %s %d-bit, %d Hz, %d channel%s, %llu:%02llu%s\n", path, wav_format_name(&w), w.bits, w.rate, w.channels,
           w.channels == 1 ? "" : "s", secs / 60, secs % 60, w.truncated ? " (the file is incomplete)" : "");
    if (!rate) { printf("PLAY: cannot play %d Hz\n", w.rate); return 1; }
    int h = snd_open(rate, oc, 16, 500, 0);
    if (h < 0) { printf("PLAY: %s\n", h == E_NODEV ? "there is no sound card" : hal_strerror(h)); return 1; }
    if (volume >= 0) snd_stream_volume(h, volume);
    snd_pause(h, 1);                         /* fill the buffer first: it starts full */
    int started = 0;
    static int16_t in[4096 * 2], out[4096 * 2];
    long chunk = rep > 1 ? (4096 / rep > 0 ? 4096 / rep : 1) : 4096;        /* frames of the file (in[] holds 4096) */
    for (uint64_t at = 0; at < w.frames;) {
        long n = wav_read(&w, at, in, chunk, oc);
        if (n <= 0) break;
        at += (uint64_t)n;
        long o = 0;
        for (long i = 0; i < n; i += dec) {          /* repeat (rep) or average (dec) frames */
            for (int c = 0; c < oc; c++) {
                long s = 0;
                int k = 0;
                for (; k < dec && i + k < n; k++) s += in[(i + k) * oc + c];
                int16_t v = (int16_t)(s / k);
                for (int j = 0; j < rep; j++) out[(o + j) * oc + c] = v;
            }
            o += rep;
        }
        hal_sndstatus_t st;
        if (!started && snd_status(h, &st) == 0 && st.space < (unsigned)o * oc * 2) { snd_pause(h, 0); started = 1; }
        long wr = snd_write(h, out, (size_t)o * oc * 2);
        if (wr < 0) { printf("PLAY: %s\n", hal_strerror((int)wr)); break; }
    }
    snd_pause(h, 0);
    snd_close(h, SNDCLOSE_WAIT);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); devices(); return 0; }
    if (!strcasecmp(argv[1], "/e") || !strcasecmp(argv[1], "-e")) {
        if (argc < 3) { usage(); return 1; }
        for (int i = 0; i < SNDEV_COUNT; i++)
            if (!strcasecmp(argv[2], events[i])) {
                int r = snd_event(i, SNDEVF_FORCE);
                if (r < 0) { printf("PLAY: %s\n", hal_strerror(r)); return 1; }
                if (!r) printf("PLAY: the %s sound is not loaded (see Sounds and Audio Devices)\n", events[i]);
                return 0;
            }
        printf("PLAY: no event called %s\n", argv[2]);
        return 1;
    }
    if (!strcasecmp(argv[1], "/volume") || !strcasecmp(argv[1], "-volume")) {
        int r;
        if (argc < 3) r = snd_master(-1, -1, 0);
        else if (!strcasecmp(argv[2], "mute")) r = snd_master(-1, 1, SNDVOL_SAVE);
        else if (!strcasecmp(argv[2], "unmute")) r = snd_master(-1, 0, SNDVOL_SAVE);
        else r = snd_master(atoi(argv[2]), -1, SNDVOL_SAVE);
        if (r < 0) { printf("PLAY: %s\n", hal_strerror(r)); return 1; }
        printf("Master volume: %d%%%s\n", r & 0xFF, r & SND_MUTED ? " (muted)" : "");
        return 0;
    }
    /* a file name (words joined: names with spaces need no quotes); /V percent at the end */
    int volume = -1, last = argc;
    if (argc >= 4 && (!strcasecmp(argv[argc - 2], "/v") || !strcasecmp(argv[argc - 2], "-v"))) {
        volume = atoi(argv[argc - 1]);
        if (volume < 0 || volume > 100) { usage(); return 1; }
        last = argc - 2;
    }
    char path[300] = "";
    for (int i = 1; i < last; i++) {
        if (i > 1) strlcat(path, " ", sizeof path);
        strlcat(path, argv[i], sizeof path);
    }
    return play_file(path, volume);
}
