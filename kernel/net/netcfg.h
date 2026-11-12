/* Halcyon OS - the internet stack's information and configuration calls for
 * kernel code (the GUI's Network Connections dialog and tray icon). The
 * same as SYS_NET_IFINFO / IFCONFIG / CTL (net/sockabi.h), with kernel
 * pointers. Weak, so callers check for NULL (a kernel built without the
 * stack). */
#ifndef NETCFG_H
#define NETCFG_H
#include "core/types.h"
#include "net/sockabi.h"

/* interface index (0.., the loopback is last): 0, E_NOENT past the end,
 * E_NODEV while the stack is not running */
int net_if_info(int index, hal_ifinfo_t *out) __attribute__((weak));
/* change (and with NETCFG_SAVE keep) an interface's configuration */
int net_if_config(const hal_ifconfig_t *cfg) __attribute__((weak));
/* NETCTL_* on an interface (name NULL = the first adapter, or all for ARP_FLUSH) */
int net_if_ctl(int op, const char *name) __attribute__((weak));

#endif
