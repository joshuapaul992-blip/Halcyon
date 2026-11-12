/* Halcyon OS - EDID (monitor identification) parsing */
#ifndef EDID_H
#define EDID_H
#include "core/types.h"

typedef struct edid_info {
    int valid;
    char vendor[4];          /* PNP id, e.g. "DEL" */
    u16 product;
    char name[14];           /* monitor name descriptor, or "" */
    int native_w, native_h;  /* preferred (first detailed) timing; 0 if none */
    int native_hz;
    int width_mm, height_mm; /* physical size of the preferred timing, if given */
    int year;                /* year of manufacture (or model year) */
} edid_info_t;

int  edid_parse(const u8 *e, int size, edid_info_t *out);   /* 1 if valid */
/* "DEL U2412M" / "Plug and Play Monitor" */
void edid_describe(const edid_info_t *i, char *out, int n);

/* Every detailed timing of an EDID: the base block's, the CEA-861
 * extensions' and DisplayID's (type I and type VII timings), in the order
 * they appear (the first one is the preferred mode). size: all the bytes
 * read (128 per block). */
typedef struct edid_timing {
    int w, h;                /* active pixels (h: frame height, also when interlaced) */
    int hz;                  /* rounded refresh rate */
    u32 clock_khz;
    int hblank, hfront, hsync, vblank, vfront, vsync;
    int interlaced;
    int block;               /* 0 base, 1.. extension block */
    int preferred;           /* DisplayID's preferred flag, or the base block's first timing */
} edid_timing_t;
int  edid_timings(const u8 *e, int size, edid_timing_t *out, int max);
int  edid_block_count(const u8 *e, int size);       /* blocks present (1 + extensions read), 0 if no EDID */
const char *edid_ext_name(u8 tag);                  /* "CEA-861", "DisplayID", "block map" ... */
/* "1920x1080 at 60 Hz (148.50 MHz)" */
void edid_timing_str(const edid_timing_t *t, char *out, int n);

#endif
