/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DSH_SCX_PORT shim: 5.10 declares the posix-timer bits inside
 * <linux/sched/signal.h> / <linux/posix-timers.h>; 6.x split this out.
 */
#ifndef _LINUX_SCHED_POSIX_TIMERS_H
#define _LINUX_SCHED_POSIX_TIMERS_H
#include <linux/posix-timers.h>
#endif
