/* Halcyon user runtime: settings (see hal.h). */
#include "hal.h"

int setting_get_int(const char *key, int def) {
    char b[32];
    if (setting_get(key, b, sizeof b) <= 0) return def;
    char *end;
    long v = strtol(b, &end, 0);
    return *end ? def : (int)v;
}

int setting_set_int(const char *key, int value) {
    char b[16];
    snprintf(b, sizeof b, "%d", value);
    return setting_set(key, b);
}
