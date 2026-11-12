/* BLANK.SCR - a black screen. Draws once; the timer frames change nothing. */
#define SAVER_MS 1000
#include "saver.h"

SAVER_NAME("Blank");

static void saver_start(saver_t *s) { sv_clear(s, 0x000000); }
static int saver_frame(saver_t *s, int dt) { (void)s; (void)dt; return 0; }
static void saver_config(void) { sv_no_options("Blank"); }
