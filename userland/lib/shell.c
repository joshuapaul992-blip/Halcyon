/* Halcyon user runtime: settings and help (see hal.h). */
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

/* HELP.EXE takes a page name ("notepad", "notepad.htm#undo") or nothing for the contents */
int help_open(const char *page) {
    char arg[96] = "";
    if (page) {
        int k = 0;
        for (const char *p = page; *p && k < (int)sizeof arg - 1; p++)
            if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '#')
                arg[k++] = *p;
        arg[k] = 0;
    }
    int pid = spawn("C:/PROGRAMS/HELP.EXE", arg[0] ? arg : NULL);
    if (pid < 0) msgbox(-1, "Help and Support", "Help and Support (C:\\PROGRAMS\\HELP.EXE) cannot be started.", MBX_ICON_ERROR, MBX_OK);
    return pid;
}
