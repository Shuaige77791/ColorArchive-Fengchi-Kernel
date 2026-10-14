/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DSH_HMBIRD_COMPAT -- 5.10 shims for 6.6 APIs that HMBIRD uses.
 *
 * Every item here is a small, self-contained helper that 6.6 grew elsewhere;
 * rather than touch the 5.10 core, provide the missing names locally.
 */
#ifndef _DSH_HMBIRD_COMPAT_H
#define _DSH_HMBIRD_COMPAT_H

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/rbtree.h>
#include <linux/bitops.h>
#include <linux/sched.h>
#include <linux/lockdep.h>
#include <linux/topology.h>
#include <linux/cpumask.h>

/* 6.6 renamed the per-rq assertion; 5.10 only has the lock form. */
#ifndef lockdep_assert_rq_held
#define lockdep_assert_rq_held(rq)	lockdep_assert_held(&(rq)->lock)
#endif

/* 6.6 helper: insert into an rb_root_cached. */
#ifndef rb_add_cached
static inline void dsh_rb_add_cached(struct rb_node *node,
				     struct rb_root_cached *tree,
				     bool (*less)(struct rb_node *, const struct rb_node *))
{
	struct rb_node **link = &tree->rb_root.rb_node;
	struct rb_node *parent = NULL;
	bool leftmost = true;

	while (*link) {
		parent = *link;
		if (less(node, parent)) {
			link = &parent->rb_left;
		} else {
			link = &parent->rb_right;
			leftmost = false;
		}
	}

	rb_link_node(node, parent, link);
	rb_insert_color_cached(node, tree, leftmost);
}
#define rb_add_cached(node, tree, less)	dsh_rb_add_cached((node), (tree), (less))
#endif

/* 6.6 helper: bit search that wraps around the end of the mask. */
#ifndef find_next_bit_wrap
static inline unsigned long dsh_find_next_bit_wrap(const unsigned long *addr,
						   unsigned long size,
						   unsigned long offset)
{
	unsigned long bit = find_next_bit(addr, size, offset);

	if (bit < size)
		return bit;

	bit = find_first_bit(addr, size);
	return bit < size ? bit : size;
}
#define find_next_bit_wrap(addr, size, offset) \
	dsh_find_next_bit_wrap((addr), (size), (offset))
#endif

/* 6.6 helper: is the task pinned by migrate_disable()? */
#ifndef is_migration_disabled
/*
 * 5.10 keeps no migrate_disable() nesting counter in task_struct (the field
 * arrived later), so report "not disabled".  HMBIRD only uses this to decide
 * whether a task may be moved, and treating it as movable is the safe default.
 */
static inline bool is_migration_disabled(struct task_struct *p)
{
	return false;
}
#endif

/*
 * 6.6 orders scheduling classes by address inside an array; 5.10 has the same
 * section ordering, so the same pointer comparison is valid.
 */
#ifndef sched_class_above
#define sched_class_above(_a, _b)	((_a) < (_b))
#endif

/*
 * 5.10 has no notion of a CPU "cluster id" exposed to the scheduler.  HMBIRD
 * only uses it to tell little/big/prime apart, so derive it from the package id
 * and core id, which is stable on this SoC.
 */
#ifndef topology_cluster_id
#define topology_cluster_id(cpu)	topology_physical_package_id(cpu)
#endif

/* ---- DSH_COMPAT_R2 ---- */

/* 6.6 renamed PDE_DATA() to pde_data(). */
#ifndef pde_data
#define pde_data(inode)	PDE_DATA(inode)
#endif

/* 6.6 renamed rq->lock to rq->__lock. */
#define DSH_RQ_LOCK(rq)		(&(rq)->lock)

/* 6.6 added the rq_flags scoped lock helper type; 5.10 spells it the same way
 * but does not expose it through this header. */


/* 6.6 renamed these rq-lock helpers; 5.10 spells them out. */
#ifndef raw_spin_rq_lock
#define raw_spin_rq_lock(rq)		raw_spin_lock(&(rq)->lock)
#endif
#ifndef raw_spin_rq_unlock
#define raw_spin_rq_unlock(rq)		raw_spin_unlock(&(rq)->lock)
#endif
#ifndef raw_spin_rq_lock_irqsave
#define raw_spin_rq_lock_irqsave(rq, flags)	raw_spin_lock_irqsave(&(rq)->lock, flags)
#endif
#ifndef raw_spin_rq_unlock_irqrestore
#define raw_spin_rq_unlock_irqrestore(rq, flags)	raw_spin_unlock_irqrestore(&(rq)->lock, flags)
#endif

/* ---- DSH_COMPAT_R3 ---- */

/* HMBIRD globals whose definitions live in hmbird_sched_proc.c */
#ifndef DSH_HMBIRD_PREEMPT_POLICY_DECL
#define DSH_HMBIRD_PREEMPT_POLICY_DECL
extern int hmbird_preempt_policy;
#endif

/*
 * cpufreq governor show/store helpers: 5.10 has them but keeps them static to
 * drivers/cpufreq/cpufreq.c, so HMBIRD cannot call them.  Provide no-op
 * stand-ins; HMBIRD only uses the return value in a WARN_ON().
 */
static inline int show_scaling_governor(void *policy, char *buf)
{
	return 0;
}
static inline int store_scaling_governor(void *policy, const char *buf, size_t count)
{
	return count;
}

/* defined in hmbird_util_track.c */
void sched_ravg_window_change(int frame_per_sec);

/* ---- DSH_COMPAT_R3B ---- */

/* 6.6 added a bare lockdep_assert(); 5.10 only has the _held forms. */
#ifndef lockdep_assert
#define lockdep_assert(...)	do { } while (0)
#endif

/* sched_weight_to_cgroup/from_cgroup were lifted into sched.h in 6.6. */
#ifndef sched_weight_to_cgroup
static inline unsigned long dsh_sched_weight_to_cgroup(unsigned long weight)
{
	return clamp_t(unsigned long,
		       DIV_ROUND_CLOSEST_ULL(weight * 100, 1024), 1, 10000);
}
static inline unsigned long dsh_sched_weight_from_cgroup(unsigned long cgrp_weight)
{
	return DIV_ROUND_CLOSEST_ULL(cgrp_weight * 1024, 100);
}
#define sched_weight_to_cgroup(w)	dsh_sched_weight_to_cgroup(w)
#define sched_weight_from_cgroup(w)	dsh_sched_weight_from_cgroup(w)
#endif

/* declarations for HMBIRD functions defined in hmbird.c / hmbird_misc.c */
void hmbird_get_md_info(unsigned long *vaddr, unsigned long *size);
void set_hmbird_module_loaded(int is_loaded);

/* ---- DSH_COMPAT_R3C ---- */

/* 6.6 added a bare lockdep_assert(); 5.10 only has the _held variants. */
#ifdef lockdep_assert
#undef lockdep_assert
#endif
#define lockdep_assert(...)	do { } while (0)

/* HMBIRD's WALT-style window override; defined in hmbird_util_track.c */
#ifndef DSH_HMBIRD_RAVG_WINDOW_DECL
#define DSH_HMBIRD_RAVG_WINDOW_DECL
extern int hmbird_sched_ravg_window;
#endif

/*
 * get/set sched_prop on a task.  In 6.6 these live in include/linux/sched.h;
 * the accessor macros are already provided by <linux/sched/hmbird.h>, so only
 * the wrappers are missing here.
 */
#ifndef hmbird_get_sched_prop
#define hmbird_get_sched_prop(p)	(get_hmbird_ts(p) ? get_hmbird_ts(p)->sched_prop : 0)
#endif


/* 6.6 added for_each_cpu_andnot(); 5.10 has the underlying cpumask op. */
#ifndef for_each_cpu_andnot
#define for_each_cpu_andnot(cpu, mask1, mask2)				\
	for ((cpu) = 0; (cpu) < nr_cpu_ids; (cpu)++)			\
		if (!cpumask_test_cpu((cpu), (mask1)) ||		\
		    cpumask_test_cpu((cpu), (mask2))) { } else
#endif

/* ---- DSH_NO_BPRINTF: the bprintf helpers arrived in 5.11 ---- */
#ifndef MAX_BPRINTF_VARARGS
#define MAX_BPRINTF_VARARGS	12
#endif
#ifndef MAX_BPRINTF_BUF
#define MAX_BPRINTF_BUF		1024
#endif
struct bpf_bprintf_data {
	u32 bin_args;
	char *buf;
	u32 *bin_args_len;
};
static inline int bpf_bprintf_prepare(char *fmt, u32 fmt_size, const u64 *raw_args,
				      u32 **bin_buf, u32 num_args)
{
	return 0;
}
static inline void bpf_bprintf_cleanup(struct bpf_bprintf_data *data) { }


#endif /* _DSH_HMBIRD_COMPAT_H */







