// SPDX-License-Identifier: GPL-2.0-only
/*
 * DSH unfair scheduling 2.0 + power saving for the fengchi port
 * (OnePlus 10T / Ace Pro, SM8475, 5.10.236-ColorArchive).
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * OPLUS's "unfair scheduling 2.0" ultimately does one thing to the hardware:
 * it asks for more CPU performance for the tasks that matter, and less for
 * everything else.  In the vendor stack that request travels
 *
 *	hb_bpf_cpuperf_set()			(vendor/.../sched_ext/hmbird_kfunc.c)
 *	  -> rq->scx.cpuperf_target = perf
 *	  -> cpufreq_update_util(rq, flags)
 *	  -> the fengchi cpufreq governor turns perf into a frequency
 *
 * On this tree sched_ext cannot be enabled -- switching the class on hard-hangs
 * the SoC (PMIC PSHOLD reset, no software log) -- so nothing would ever write
 * that field.  This file produces the same request from hooks the tree already
 * has, so the effect is preserved without sched_ext:
 *
 *	android_vh_account_task_time()	every tick, the task that just ran
 *	android_vh_scheduler_tick()	every tick, every CPU
 *	  -> dsh_cpuperf_target[cpu]
 *	  -> cpufreq_update_util()
 *	  -> the same fengchi governor
 *
 * THE POLICY
 * ----------
 *   - a task is "critical" if it belongs to the foreground app (target_tgid),
 *     is one of the registered pids, or carries a boosted weight.  A critical
 *     task raises the CPU's performance request to dsh_boost_perf, which is the
 *     unfair part: it is served before background work.
 *   - every other task, and every idle CPU, is capped at dsh_idle_perf.  That
 *     cap is where the power saving comes from: the governor then computes a
 *     low frequency, and the soft_freq_max / target_loads knobs of the governor
 *     can push it lower still.
 *   - dsh_cpuperf_target is a per-CPU variable of our own.  Nothing is added to
 *     struct rq / task_struct / cpufreq_policy, so exported symbol CRCs are
 *     untouched.
 *
 * Everything here runs in the scheduler hot path: no locks, no sleeping, only
 * per-CPU and atomic accesses.
 */

#include "sched.h"

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/atomic.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/sched/cpufreq.h>
#include <trace/hooks/sched.h>

/*
 * The performance request for each CPU, in fengchi's 0..1024 domain.
 * Read by kernel/sched/dsh_fengchi_gov.c.
 */
DEFINE_PER_CPU(u32, dsh_cpuperf_target);

#define DSH_MAX_PERF		1024
#define DSH_MAX_PIDS		32

/* ---- tunables (userspace visible under /proc/dsh_unfair/) ------------------ */

static bool	dsh_enabled		= true;
static int	dsh_target_tgid		= -1;
static u32	dsh_boost_perf		= DSH_MAX_PERF;
static u32	dsh_idle_perf		= 192;
static u32	dsh_heavy_weight	= 3072;

/* registered critical pids; a plain array, scanned linearly: bounded and cheap */
static atomic_t	dsh_nr_pids = ATOMIC_INIT(0);
static pid_t	dsh_pids[DSH_MAX_PIDS];

/* stats so the effect can be verified from userspace */
static atomic64_t dsh_boost_hits = ATOMIC64_INIT(0);
static atomic64_t dsh_idle_hits = ATOMIC64_INIT(0);
static atomic64_t dsh_update_calls = ATOMIC64_INIT(0);

/*
 * Only feed a governor that actually consumes the request.  With walt (or any
 * other vendor governor) selected this whole file must do nothing at all --
 * otherwise we would be calling cpufreq_update_util() into a governor that has
 * no idea what we are asking for.  That interaction is a prime suspect for the
 * early-boot hang this port hit.
 */
static inline struct cpufreq_policy *dsh_gov_is_ours(unsigned int cpu)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get_raw(cpu);

	if (!policy || !policy->governor || !policy->governor->name)
		return NULL;
	if (strcmp(policy->governor->name, "scx") &&
	    strcmp(policy->governor->name, "sugov_ext"))
		return NULL;

	return policy;
}

static inline bool dsh_pid_registered(pid_t pid)
{
	int i, n = atomic_read(&dsh_nr_pids);

	for (i = 0; i < n && i < DSH_MAX_PIDS; i++)
		if (READ_ONCE(dsh_pids[i]) == pid)
			return true;
	return false;
}

/*
 * Is this task the one the unfair scheduler should favour?
 * Called from the tick path, so the tgid compare comes first: it is O(1) and it
 * is the case that actually matters (the app in the foreground).
 */
static inline bool dsh_task_is_critical(struct task_struct *p)
{
	if (!p)
		return false;

	if (READ_ONCE(dsh_target_tgid) >= 0 && p->tgid == READ_ONCE(dsh_target_tgid))
		return true;

	if (dsh_pid_registered(p->pid))
		return true;

	if (READ_ONCE(dsh_heavy_weight) &&
	    p->se.load.weight >= READ_ONCE(dsh_heavy_weight))
		return true;

	return false;
}

/* ---- the two observation hooks -------------------------------------------- */

static void dsh_account_task_time(void *data, struct task_struct *p,
				  struct rq *rq, int user_tick)
{
	unsigned int cpu;
	u32 perf;

	if (!READ_ONCE(dsh_enabled) || !rq)
		return;

	cpu = cpu_of(rq);

	if (dsh_task_is_critical(p)) {
		perf = READ_ONCE(dsh_boost_perf);
		atomic64_inc(&dsh_boost_hits);
	} else {
		/* not critical: hold the request down, this is the saving */
		perf = READ_ONCE(dsh_idle_perf);
		atomic64_inc(&dsh_idle_hits);
	}

	if (!dsh_gov_is_ours(cpu))
		return;

	if (READ_ONCE(per_cpu(dsh_cpuperf_target, cpu)) == perf)
		return;

	WRITE_ONCE(per_cpu(dsh_cpuperf_target, cpu), perf);

	atomic64_inc(&dsh_update_calls);
	rcu_read_lock_sched_notrace();
	cpufreq_update_util(rq, 0);
	rcu_read_unlock_sched_notrace();
}

/*
 * Runs on every CPU on every tick, including idle CPUs.  An idle or
 * fully-blocked CPU has nothing that needs frequency, so decay its request --
 * that is what keeps the low-power states cheap.
 */
static void dsh_scheduler_tick(void *data, struct rq *rq)
{
	unsigned int cpu;
	u32 cur, want;

	if (!READ_ONCE(dsh_enabled) || !rq)
		return;

	cpu = cpu_of(rq);
	cur = READ_ONCE(per_cpu(dsh_cpuperf_target, cpu));

	if (!rq->nr_running) {
		want = 0;			/* nothing runnable at all */
	} else if (rq->curr && dsh_task_is_critical(rq->curr)) {
		want = READ_ONCE(dsh_boost_perf);
	} else {
		want = READ_ONCE(dsh_idle_perf);
	}

	if (cur == want)
		return;

	WRITE_ONCE(per_cpu(dsh_cpuperf_target, cpu), want);
}

/* ---- /proc/dsh_unfair/ ----------------------------------------------------- */

static int dsh_stats_show(struct seq_file *m, void *v)
{
	int cpu;

	seq_printf(m, "enabled          : %d\n", READ_ONCE(dsh_enabled) ? 1 : 0);
	seq_printf(m, "target_tgid      : %d\n", READ_ONCE(dsh_target_tgid));
	seq_printf(m, "boost_perf       : %u\n", READ_ONCE(dsh_boost_perf));
	seq_printf(m, "idle_perf        : %u\n", READ_ONCE(dsh_idle_perf));
	seq_printf(m, "heavy_weight     : %u\n", READ_ONCE(dsh_heavy_weight));
	seq_printf(m, "registered_pids  : %d\n", atomic_read(&dsh_nr_pids));
	seq_printf(m, "boost_hits       : %lld\n", atomic64_read(&dsh_boost_hits));
	seq_printf(m, "idle_hits        : %lld\n", atomic64_read(&dsh_idle_hits));
	seq_printf(m, "update_calls     : %lld\n", atomic64_read(&dsh_update_calls));
	seq_puts(m, "per-cpu perf request:\n");
	for_each_possible_cpu(cpu)
		seq_printf(m, "  cpu%-2d : %u\n", cpu, READ_ONCE(per_cpu(dsh_cpuperf_target, cpu)));

	return 0;
}
static int dsh_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, dsh_stats_show, NULL);
}

static const struct proc_ops dsh_stats_pops = {
	.proc_open	= dsh_stats_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

#define DSH_U32_FILE(_name, _var)						\
static int dsh_##_name##_show(struct seq_file *m, void *v)			\
{										\
	seq_printf(m, "%u\n", READ_ONCE(_var));					\
	return 0;								\
}										\
static int dsh_##_name##_open(struct inode *i, struct file *f)			\
{										\
	return single_open(f, dsh_##_name##_show, NULL);				\
}										\
static ssize_t dsh_##_name##_write(struct file *f, const char __user *ubuf,	\
				   size_t cnt, loff_t *ppos)			\
{										\
	unsigned int val;							\
	char buf[32];								\
	if (cnt >= sizeof(buf))							\
		return -EINVAL;							\
	if (copy_from_user(buf, ubuf, cnt))					\
		return -EFAULT;							\
	buf[cnt] = '\0';							\
	if (kstrtouint(buf, 10, &val))						\
		return -EINVAL;							\
	WRITE_ONCE(_var, val);							\
	return cnt;								\
}										\
static int dsh_##_name##_release(struct inode *i, struct file *f)		\
{										\
	return single_release(i, f);						\
}										\
static const struct proc_ops dsh_##_name##_pops = {				\
	.proc_open	= dsh_##_name##_open,					\
	.proc_read	= seq_read,						\
	.proc_write	= dsh_##_name##_write,					\
	.proc_lseek	= seq_lseek,						\
	.proc_release	= dsh_##_name##_release,				\
}

#define DSH_I32_FILE(_name, _var)						\
static int dsh_##_name##_show(struct seq_file *m, void *v)			\
{										\
	seq_printf(m, "%d\n", READ_ONCE(_var));					\
	return 0;								\
}										\
static int dsh_##_name##_open(struct inode *i, struct file *f)			\
{										\
	return single_open(f, dsh_##_name##_show, NULL);				\
}										\
static ssize_t dsh_##_name##_write(struct file *f, const char __user *ubuf,	\
				   size_t cnt, loff_t *ppos)			\
{										\
	int val;								\
	char buf[32];								\
	if (cnt >= sizeof(buf))							\
		return -EINVAL;							\
	if (copy_from_user(buf, ubuf, cnt))					\
		return -EFAULT;							\
	buf[cnt] = '\0';							\
	if (kstrtoint(buf, 10, &val))						\
		return -EINVAL;							\
	WRITE_ONCE(_var, val);							\
	return cnt;								\
}										\
static int dsh_##_name##_release(struct inode *i, struct file *f)		\
{										\
	return single_release(i, f);						\
}										\
static const struct proc_ops dsh_##_name##_pops = {				\
	.proc_open	= dsh_##_name##_open,					\
	.proc_read	= seq_read,						\
	.proc_write	= dsh_##_name##_write,					\
	.proc_lseek	= seq_lseek,						\
	.proc_release	= dsh_##_name##_release,				\
}

DSH_U32_FILE(boost_perf, dsh_boost_perf);
DSH_U32_FILE(idle_perf, dsh_idle_perf);
DSH_U32_FILE(heavy_weight, dsh_heavy_weight);
DSH_I32_FILE(target_tgid, dsh_target_tgid);
DSH_I32_FILE(enabled, dsh_enabled);

/* add_pid: write a pid to make that task critical */
static ssize_t dsh_add_pid_write(struct file *f, const char __user *ubuf,
				 size_t cnt, loff_t *ppos)
{
	pid_t pid;
	int n;
	char buf[32];

	if (cnt >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, cnt))
		return -EFAULT;
	buf[cnt] = '\0';
	if (kstrtoint(buf, 10, &pid))
		return -EINVAL;

	n = atomic_read(&dsh_nr_pids);
	if (n >= DSH_MAX_PIDS)
		return -ENOSPC;
	WRITE_ONCE(dsh_pids[n], pid);
	atomic_set(&dsh_nr_pids, n + 1);

	return cnt;
}
static int dsh_wo_open(struct inode *i, struct file *f)
{
	return 0;
}
static int dsh_wo_release(struct inode *i, struct file *f)
{
	return 0;
}
static const struct proc_ops dsh_add_pid_pops = {
	.proc_open	= dsh_wo_open,
	.proc_write	= dsh_add_pid_write,
	.proc_release	= dsh_wo_release,
};

/* clear: drop all registered pids */
static ssize_t dsh_clear_write(struct file *f, const char __user *ubuf,
			       size_t cnt, loff_t *ppos)
{
	atomic_set(&dsh_nr_pids, 0);
	return cnt;
}
static const struct proc_ops dsh_clear_pops = {
	.proc_open	= dsh_wo_open,
	.proc_write	= dsh_clear_write,
	.proc_release	= dsh_wo_release,
};

static struct proc_dir_entry *dsh_proc_dir;

static int dsh_unfair_proc_init(void)
{
	struct proc_dir_entry *e;

	dsh_proc_dir = proc_mkdir("dsh_unfair", NULL);
	if (!dsh_proc_dir) {
		pr_err("dsh_unfair: cannot create /proc/dsh_unfair\n");
		return -ENOMEM;
	}

	e = proc_create("stats", 0444, dsh_proc_dir, &dsh_stats_pops);
	e = proc_create("enabled", 0644, dsh_proc_dir, &dsh_enabled_pops);
	e = proc_create("target_tgid", 0644, dsh_proc_dir, &dsh_target_tgid_pops);
	e = proc_create("boost_perf", 0644, dsh_proc_dir, &dsh_boost_perf_pops);
	e = proc_create("idle_perf", 0644, dsh_proc_dir, &dsh_idle_perf_pops);
	e = proc_create("heavy_weight", 0644, dsh_proc_dir, &dsh_heavy_weight_pops);
	e = proc_create("add_pid", 0200, dsh_proc_dir, &dsh_add_pid_pops);
	e = proc_create("clear", 0200, dsh_proc_dir, &dsh_clear_pops);

	(void)e;
	return 0;
}

/* ---- init ----------------------------------------------------------------- */

static void dsh_unfair_start_workfn(struct work_struct *w)
{
	int ret;

	ret = register_trace_android_vh_account_task_time(dsh_account_task_time, NULL);
	if (ret) {
		pr_err("dsh_unfair: cannot hook account_task_time (%d)\n", ret);
		return;
	}

	ret = register_trace_android_vh_scheduler_tick(dsh_scheduler_tick, NULL);
	if (ret) {
		unregister_trace_android_vh_account_task_time(dsh_account_task_time, NULL);
		pr_err("dsh_unfair: cannot hook scheduler_tick (%d)\n", ret);
		return;
	}

	pr_info("dsh_unfair: hooks armed (system is up)\n");
}

static DECLARE_DELAYED_WORK(dsh_unfair_start_work, dsh_unfair_start_workfn);

static int __init dsh_unfair_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		WRITE_ONCE(per_cpu(dsh_cpuperf_target, cpu), 0);

	dsh_unfair_proc_init();

	/*
	 * Nothing is armed during boot: the hooks are registered a few seconds
	 * after the system is up, so early boot cannot be affected at all.
	 */
	schedule_delayed_work(&dsh_unfair_start_work, msecs_to_jiffies(8000));

	pr_info("dsh_unfair: ready (hooks will arm 8s after init, boost=%u idle=%u)\n",
		dsh_boost_perf, dsh_idle_perf);
	return 0;
}

late_initcall(dsh_unfair_init);
