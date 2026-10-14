// SPDX-License-Identifier: GPL-2.0-only
/*
 * DSH_SCX_PORT -- build wrapper for sched_ext.
 *
 * Upstream 6.6 feeds sched_ext's translation unit from build_policy.c: ext.c
 * itself carries no #include lines at all, because everything it needs is
 * pulled in by the wrapper before it is textually included.  5.10 has no
 * build_policy.c (rt.c / deadline.c / idle.c are compiled separately here), so
 * this file reproduces only the header prologue that ext.c depends on, and
 * includes ext.c the same way upstream does.
 *
 * Keep the include list in sync with 6.6's kernel/sched/build_policy.c.
 */

/* Headers, copied from 6.6's build_policy.c prologue: */
#include <linux/sched/clock.h>
#include <linux/sched/cputime.h>
#include <linux/sched/hotplug.h>
#include <linux/sched/posix-timers.h>
#include <linux/sched/rt.h>

#include <linux/cpuidle.h>
#include <linux/jiffies.h>
#include <linux/psi.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <linux/tsacct_kern.h>
#include <linux/vtime.h>
#include <linux/sysrq.h>
#include <linux/percpu-rwsem.h>

#include <uapi/linux/sched/types.h>

#ifndef DSH_INTERNAL_SCHED_INCLUDED
#define DSH_INTERNAL_SCHED_INCLUDED
#include "sched.h"
#endif
#include "smp.h"

/* #include "autogroup.h" removed: sched.h already includes it, no guard */
/* #include "stats.h" removed: sched.h already includes it, no guard */
/* #include "pelt.h" removed: sched.h already includes it, no guard */

/* ext.c needs these too, and 5.10 does not get them transitively: */
#include <linux/bpf.h>
#include <linux/btf.h>
#include <linux/bpf_verifier.h>
#include <linux/btf_ids.h>

/* ---- DSH_SCX_KFUNC_STUB ----
 *
 * sched_ext registers its BPF-callable kfuncs through BTF sets, an interface
 * that only exists from 6.x: 5.10 has neither BTF_ID_FLAGS() nor
 * register_btf_kfunc_id_set() nor the BTF_SET8 helpers.  Those kfuncs are what
 * a *BPF* scheduler needs; HMBIRD (fengchi) is a native sched_class and never
 * calls them, so the registration layer is stubbed.  The extensible scheduling
 * class itself, and HMBIRD on top of it, are unaffected.
 */
/* ---- DSH_DIAG_SHIM: the __diag_* pragma helpers are 6.x ---- */
#define DSH_DIAG_SHIM 1
#ifndef __diag_push
#define __diag_push()			do { } while (0)
#endif
#ifndef __diag_pop
#define __diag_pop()			do { } while (0)
#endif
#ifndef __diag_ignore_all
#define __diag_ignore_all(option, comment)	do { } while (0)
#endif
#ifndef __diag_ignore
#define __diag_ignore(compiler, version, option, comment)	do { } while (0)
#endif

#define DSH_SCX_KFUNC_STUB 1

/* 5.10 predates struct btf_kfunc_id_set; give the stubbed sets a home. */
struct btf_kfunc_id_set {
	struct module		*owner;
	const struct btf_id_set	*set;
};



#ifndef BTF_ID_FLAGS
#define BTF_ID_FLAGS(prefix, name, ...)		BTF_ID_UNUSED
#endif
#ifndef BTF_SET8_START
#define BTF_SET8_START(name)			BTF_SET_START(name)
#endif
#ifndef BTF_SET8_END
#define BTF_SET8_END(name)			BTF_SET_END(name)
#endif
#ifndef BTF_KFUNCS_START
#define BTF_KFUNCS_START(name)			BTF_SET8_START(name)
#endif
#ifndef BTF_KFUNCS_END
#define BTF_KFUNCS_END(name)			BTF_SET8_END(name)
#endif
#ifndef register_btf_kfunc_id_set
static inline int register_btf_kfunc_id_set(enum bpf_prog_type prog_type,
					    const void *kset)
{
	return 0;
}
#endif


#include <linux/cgroup.h>
#include <linux/cpumask.h>
#include <linux/kthread.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>
#include <linux/atomic.h>
#include <linux/workqueue.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/time.h>
#include <linux/ktime.h>
#include <linux/rcupdate.h>
#include <linux/percpu.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/sched/ext.h>

#include "hmbird_compat.h"	/* DSH: 6.x API shims for ext.c */

/*
 * DSH_EXT_LOCAL_HELPERS -- core.c keeps __setscheduler_prio() and
 * check_class_changed() static (as they are in 6.6), and HMBIRD defines its own
 * static copies, so sched_ext gets private equivalents here rather than
 * exporting the originals and colliding with HMBIRD.
 */
#define DSH_EXT_LOCAL_HELPERS 1
static void dsh_ext_setscheduler_prio(struct task_struct *p, int prio)
{
	p->prio = prio;
}
static void dsh_ext_check_class_changed(struct rq *rq, struct task_struct *p,
					const struct sched_class *prev_class, int prio)
{
	if (prev_class != p->sched_class) {
		if (prev_class->switched_from)
			prev_class->switched_from(rq, p);
		p->sched_class->switched_to(rq, p);
	} else if (prev_class->prio_changed) {
		prev_class->prio_changed(rq, p, prio);
	}
}
#define __setscheduler_prio(p, prio)		dsh_ext_setscheduler_prio((p), (prio))
#define check_class_changed(rq, p, pc, prio)	dsh_ext_check_class_changed((rq), (p), (pc), (prio))

#include "ext.h"

/* Source module, exactly as 6.6 does it: */
#ifdef CONFIG_SCHED_CLASS_EXT
# include "ext.c"
#endif
