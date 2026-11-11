/* TEMPORARY weak stubs for applications not written yet */
#include "gui/wm.h"
#define STUB(name, ...) __attribute__((weak)) void name(__VA_ARGS__) { msgbox(NULL, #name, "This application is not available yet.", ICON_INFO, MB_OK); }
STUB(app_explorer, const char *p)
STUB(app_notepad, const char *p)
STUB(app_terminal, void)
STUB(app_paint, const char *p)
STUB(app_calc, void)
STUB(app_sysinfo, void)
STUB(app_control, void)
STUB(app_about, void)
STUB(app_imageview, const char *p)
STUB(app_mines, void)
