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

#endif
