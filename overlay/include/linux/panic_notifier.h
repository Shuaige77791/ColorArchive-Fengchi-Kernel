/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DSH_SCX_PORT shim.
 *
 * 5.10 keeps the panic notifier API in <linux/kernel.h>; 6.x split it into
 * this header.  The ported sched_ext/HMBIRD sources include it, so provide the
 * split header rather than editing every include site.
 */
#ifndef _LINUX_PANIC_NOTIFIER_H
#define _LINUX_PANIC_NOTIFIER_H

#include <linux/kernel.h>
#include <linux/notifier.h>

#endif /* _LINUX_PANIC_NOTIFIER_H */
