// SPDX-License-Identifier: GPL-2.0-only
/*
 * DSH fengchi CPU governor -- a real, working frequency governor for the
 * OnePlus 10T / Ace Pro (SM8475, 5.10.236-ColorArchive) fengchi port.
 *
 * WHERE THIS COMES FROM
 * ---------------------
 * The OPLUS fengchi stack pairs its sched_ext scheduler with a dedicated cpufreq
 * governor.  In the vendor tree that governor is
 *	vendor/oplus/kernel/cpu/sched_ext/hmbird_II/hmbird_II_freqgov.c
 * (29,570 bytes), registered there as "hmbird".  Its core maths is:
 *
 *	cluster_tl = mtl_target_load(hg_policy, perf);
 *	perf_tl    = mult_frac(perf, 100, cluster_tl);
 *	next_f     = mult_frac(policy->cpuinfo.max_freq, perf_tl,
 *			       arch_scale_cpu_capacity(cpu));
 *
 * and it pushes that out with cpufreq_driver_fast_switch() when fast switching
 * is available, otherwise by queueing a kthread_work that calls
 * __cpufreq_driver_target().  Both of those are reproduced here.
 *
 * WHAT IS DIFFERENT, AND WHY
 * --------------------------
 * 1. Input.  The vendor governor reads `rq->scx.cpuperf_target`, a field only
 *    the vendor's BPF scheduler writes (through the kfunc in hmbird_kfunc.c).
 *    sched_ext cannot be enabled on this tree -- doing so hard-hangs the SoC --
 *    so that field would never change and a literal copy would govern nothing.
 *    The same `perf` quantity is therefore derived from the always-available
 *    signal: this CPU's CFS utilisation, run through schedutil_cpu_util() and
 *    mapped onto fengchi's 0..1024 performance domain.  Everything downstream
 *    of `perf` is the vendor algorithm unchanged.
 *
 * 2. Tuning path.  This platform sets CPUFREQ_HAVE_GOVERNOR_PER_POLICY
 *    (drivers/cpufreq/qcom-cpufreq-hw.c), so a governor's tunables kobject
 *    normally lands in /sys/devices/system/cpu/cpufreq/policy<N>/<name>/.  The
 *    app used to switch scheduling modes (Scene, com.omarea.vtools) instead
 *    reads /sys/devices/system/cpu/cpufreq/<name>/up_rate_limit_us,
 *    .../down_rate_limit_us and .../rate_limit_us.  Those files are created here
 *    directly, under the global cpu/cpufreq kobject, and the values live in one
 *    shared per-governor structure that every policy reads -- so one write from
 *    userspace affects all clusters immediately.
 *
 * 3. The rate limits are not decoration: the update hook refuses to ramp up or
 *    down faster than up_rate_limit_us / down_rate_limit_us, which is exactly
 *    what those knobs mean.
 *
 * 4. Boot safety.  Userspace restores its saved governor early in boot, so a
 *    governor that starts driving frequency immediately can hang the machine
 *    before it finishes coming up -- which is exactly what happened here.  The
 *    update hook is therefore only installed once system_state is
 *    SYSTEM_RUNNING.  If the governor is selected during boot, dsh_init() only
 *    allocates memory and the entire activation -- worker thread, tunables
 *    directory and update hook -- is deferred by 5 s.  Selecting it from
 *    userspace after boot activates it immediately.
 *
 * 5. Names.  Userspace sees "scx" and "sugov_ext".  There is deliberately no
 *    "hmbird" governor on this tree.
 *
 * Nothing here changes the layout of any shared structure, so exported symbol
 * CRCs are untouched.
 */

#include "sched.h"

#include <linux/cpufreq.h>
#include <linux/kobject.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/sched/cpufreq.h>

/* fengchi's performance domain: 0 .. 1024, same range as cpuperf_target */
#define DSH_MAX_PERF			1024

/*
 * The per-CPU performance request produced by kernel/sched/dsh_unfair.c -- the
 * replacement for the vendor's rq->scx.cpuperf_target.  Zero means "no request",
 * in which case the governor falls back to measured utilisation.
 */
DEFINE_PER_CPU(u32, dsh_cpuperf_target);
#define DSH_DEFAULT_TARGET_LOAD		90
#define DSH_MIN_TARGET_LOAD		1
#define DSH_MAX_TARGET_LOAD		100

#define DSH_GOV_NR			2
#define DSH_GOV_IDX_SCX			0
#define DSH_GOV_IDX_SUGOV_EXT		1

static const char * const dsh_gov_names[DSH_GOV_NR] = {
	[DSH_GOV_IDX_SCX]	= "scx",
	[DSH_GOV_IDX_SUGOV_EXT]	= "sugov_ext",
};

/*
 * The shared, per-governor tuning values.  live under
 * /sys/devices/system/cpu/cpufreq/<name>/ and are read by every policy.
 */
struct dsh_gov_params {
	unsigned int	rate_limit_us;
	unsigned int	up_rate_limit_us;
	unsigned int	down_rate_limit_us;
	unsigned int	target_loads;
	unsigned int	soft_freq_min;
	unsigned int	soft_freq_max;
	bool		apply_freq_immediately;
	/*
	 * scx consumes the per-CPU performance request produced by
	 * dsh_unfair.c (the unfair-scheduling 2.0 channel, equivalent to the
	 * vendor's rq->scx.cpuperf_target).  sugov_ext does not: it behaves
	 * like schedutil and follows measured utilisation alone.
	 */
	bool		prefer_perf_hint;
};

struct dsh_gov_view {
	struct kobject		kobj;
	struct dsh_gov_params	p;
};

static struct dsh_gov_view dsh_views[DSH_GOV_NR];

struct dsh_gov_policy {
	struct cpufreq_policy	*policy;
	struct dsh_gov_params	*params;

	struct kthread_worker	worker;
	struct task_struct	*thread;
	struct kthread_work	work;

	struct mutex		work_lock;
	raw_spinlock_t		update_lock;

	/*
	 * Installing the update hook is what makes this governor actually run.
	 * Doing that while the system is still booting is what made the port hang
	 * at the first screen: the kernel selects the governor early (userspace
	 * restores its saved choice), our tick hook starts firing immediately, and
	 * the machine never finishes coming up.  walt does not have that problem,
	 * which is why it could always boot.
	 *
	 * So the hook is only installed once the system is running.  Selecting the
	 * governor from userspace after boot still takes effect immediately.
	 */
	struct delayed_work	activate_work;
	bool			hooks_installed;
	bool			activated;

	u64			last_freq_update_time;
	unsigned int		next_freq;
	unsigned int		next_raw_freq;
	unsigned int		last_freq;
};

struct dsh_gov_cpu {
	struct update_util_data	update_util;
	struct dsh_gov_policy	*dg_policy;
	unsigned int		cpu;
};

static DEFINE_PER_CPU(struct dsh_gov_cpu, dsh_gov_cpu);

static int dsh_gov_idx(const struct cpufreq_governor *gov)
{
	int i;

	for (i = 0; i < DSH_GOV_NR; i++)
		if (gov && !strcmp(gov->name, dsh_gov_names[i]))
			return i;
	return -1;
}

static struct dsh_gov_params *dsh_params_of(const struct cpufreq_policy *policy)
{
	int i = dsh_gov_idx(policy->governor);

	return (i < 0) ? NULL : &dsh_views[i].p;
}

/*******************************************************************************
 * the fengchi algorithm
 ******************************************************************************/

/* mtl_target_load() from the vendor source */
static inline unsigned int dsh_target_load(struct dsh_gov_policy *dg)
{
	unsigned int tl = dg->params ? dg->params->target_loads
				     : DSH_DEFAULT_TARGET_LOAD;

	if (!tl)
		tl = DSH_DEFAULT_TARGET_LOAD;
	if (tl < DSH_MIN_TARGET_LOAD)
		tl = DSH_MIN_TARGET_LOAD;
	if (tl > DSH_MAX_TARGET_LOAD)
		tl = DSH_MAX_TARGET_LOAD;

	return tl;
}

/* get_next_freq() from the vendor source, unchanged */
static unsigned int dsh_get_next_freq(struct dsh_gov_policy *dg, u32 perf)
{
	struct cpufreq_policy *policy = dg->policy;
	unsigned int max_freq = policy->cpuinfo.max_freq;
	unsigned int cpu = cpumask_first(policy->cpus);
	unsigned long cap = arch_scale_cpu_capacity(cpu);
	unsigned int cluster_tl, perf_tl, next_f;

	if (perf > DSH_MAX_PERF)
		perf = DSH_MAX_PERF;

	cluster_tl = dsh_target_load(dg);
	perf_tl = mult_frac(perf, 100, cluster_tl);
	next_f = mult_frac(max_freq, perf_tl, cap ? cap : DSH_MAX_PERF);

	if (next_f < policy->min)
		next_f = policy->min;
	if (next_f > policy->max)
		next_f = policy->max;
	if (next_f > max_freq)
		next_f = max_freq;

	return next_f;
}

/* soft_freq_clamp() from the vendor source */
static unsigned int dsh_soft_freq_clamp(struct dsh_gov_policy *dg,
					unsigned int target_freq)
{
	struct dsh_gov_params *p = dg->params;

	if (!p)
		return target_freq;

	if (p->soft_freq_min && target_freq < p->soft_freq_min)
		target_freq = p->soft_freq_min;
	if (p->soft_freq_max && target_freq > p->soft_freq_max)
		target_freq = p->soft_freq_max;

	return target_freq;
}

/*
 * The real input: this CPU's utilisation in fengchi's 0..1024 perf domain.
 * This is what replaces rq->scx.cpuperf_target.
 */
static u32 dsh_perf_for_cpu(unsigned int cpu, bool prefer_hint)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned long util, max;
	u32 hint, perf;

	if (!cpu_online(cpu))
		return 0;

	/*
	 * For scx the unfair-scheduling request wins when present: it is an
	 * explicit statement about what this CPU should deliver, whereas
	 * utilisation is only an inference.  sugov_ext ignores it entirely.
	 */
	if (prefer_hint) {
		hint = READ_ONCE(per_cpu(dsh_cpuperf_target, cpu));
		if (hint)
			return hint;
	}

	max = arch_scale_cpu_capacity(cpu);
	if (!max)
		return 0;

	util = cpu_util_cfs(rq);
	util = schedutil_cpu_util(cpu, util, max, FREQUENCY_UTIL, NULL);
	if (util > max)
		util = max;

	perf = (u32)mult_frac(util, DSH_MAX_PERF, max);
	if (perf == 0)
		perf = 1;	/* never ask for zero performance */

	return perf;
}

/*******************************************************************************
 * pushing the frequency out
 ******************************************************************************/

static void dsh_gov_work(struct kthread_work *work)
{
	struct dsh_gov_policy *dg = container_of(work, struct dsh_gov_policy, work);
	unsigned long flags;
	unsigned int freq;

	raw_spin_lock_irqsave(&dg->update_lock, flags);
	freq = dg->next_freq;
	raw_spin_unlock_irqrestore(&dg->update_lock, flags);

	mutex_lock(&dg->work_lock);
	__cpufreq_driver_target(dg->policy, freq, CPUFREQ_RELATION_L);
	mutex_unlock(&dg->work_lock);
}

static void dsh_update_cpufreq(struct dsh_gov_policy *dg, u32 perf)
{
	struct cpufreq_policy *policy = dg->policy;
	unsigned long flags;
	unsigned int next_f;

	raw_spin_lock_irqsave(&dg->update_lock, flags);

	next_f = dsh_get_next_freq(dg, perf);
	dg->next_raw_freq = next_f;
	next_f = dsh_soft_freq_clamp(dg, next_f);
	next_f = cpufreq_driver_resolve_freq(policy, next_f);

	if (dg->next_freq == next_f)
		goto unlock;

	dg->next_freq = next_f;
	dg->last_freq = next_f;

	if (policy->fast_switch_enabled && dg->params &&
	    dg->params->apply_freq_immediately)
		cpufreq_driver_fast_switch(policy, next_f);
	else
		kthread_queue_work(&dg->worker, &dg->work);

unlock:
	raw_spin_unlock_irqrestore(&dg->update_lock, flags);
}

static void dsh_update_util(struct update_util_data *cb, u64 time,
			    unsigned int flags)
{
	struct dsh_gov_cpu *dg_cpu = container_of(cb, struct dsh_gov_cpu, update_util);
	struct dsh_gov_policy *dg = READ_ONCE(dg_cpu->dg_policy);
	struct dsh_gov_params *p;
	unsigned int next_f;
	u64 delta_ns;
	u32 perf;
	bool boost;

	if (!dg)
		return;
	p = dg->params;
	if (!p)
		return;

	perf = dsh_perf_for_cpu(dg_cpu->cpu, p->prefer_perf_hint);
	next_f = dsh_get_next_freq(dg, perf);

	/*
	 * On 5.10 the RT/DL wakeup hints are folded into SCHED_CPUFREQ_IOWAIT,
	 * so that single bit marks "something wants performance right now".
	 */
	boost = flags & SCHED_CPUFREQ_IOWAIT;
	if (boost || p->apply_freq_immediately)
		goto update;

	if (dg->last_freq) {
		delta_ns = time - dg->last_freq_update_time;

		if (next_f > dg->last_freq) {
			if (delta_ns < (u64)p->up_rate_limit_us * NSEC_PER_USEC)
				return;
		} else if (next_f < dg->last_freq) {
			if (delta_ns < (u64)p->down_rate_limit_us * NSEC_PER_USEC)
				return;
		} else {
			return;		/* already at the right frequency */
		}
	}

update:
	dg->last_freq_update_time = time;
	dsh_update_cpufreq(dg, perf);
}

/*******************************************************************************
 * sysfs: /sys/devices/system/cpu/cpufreq/<name>/<attr>
 ******************************************************************************/

static ssize_t dsh_view_show(struct kobject *kobj, struct attribute *attr,
			     char *buf);
static ssize_t dsh_view_store(struct kobject *kobj, struct attribute *attr,
			      const char *buf, size_t count);

#define DSH_VIEW_ATTR(_name, _mode)					\
	static struct attribute dsh_attr_##_name = {			\
		.name = __stringify(_name),				\
		.mode = (_mode),					\
	}

DSH_VIEW_ATTR(rate_limit_us,		0644);
DSH_VIEW_ATTR(up_rate_limit_us,		0644);
DSH_VIEW_ATTR(down_rate_limit_us,	0644);
DSH_VIEW_ATTR(target_loads,		0644);
DSH_VIEW_ATTR(soft_freq_min,		0644);
DSH_VIEW_ATTR(soft_freq_max,		0644);
DSH_VIEW_ATTR(apply_freq_immediately,	0644);
DSH_VIEW_ATTR(prefer_perf_hint,		0644);

static struct attribute *dsh_view_attrs[] = {
	&dsh_attr_rate_limit_us,
	&dsh_attr_up_rate_limit_us,
	&dsh_attr_down_rate_limit_us,
	&dsh_attr_target_loads,
	&dsh_attr_soft_freq_min,
	&dsh_attr_soft_freq_max,
	&dsh_attr_apply_freq_immediately,
	&dsh_attr_prefer_perf_hint,
	NULL
};
ATTRIBUTE_GROUPS(dsh_view);

static const struct sysfs_ops dsh_view_sysfs_ops = {
	.show	= dsh_view_show,
	.store	= dsh_view_store,
};

static void dsh_view_release(struct kobject *kobj) { }

static struct kobj_type dsh_view_ktype = {
	.sysfs_ops	= &dsh_view_sysfs_ops,
	.default_groups	= dsh_view_groups,
	.release	= dsh_view_release,
};

static struct kobject *dsh_view_kobj[DSH_GOV_NR];
static struct kobject *dsh_cpufreq_kobj;

static ssize_t dsh_view_show(struct kobject *kobj, struct attribute *attr,
			     char *buf)
{
	struct dsh_gov_view *v = container_of(kobj, struct dsh_gov_view, kobj);
	struct dsh_gov_params *p = &v->p;
	const char *n = attr->name;

	if (!strcmp(n, "rate_limit_us"))
		return sprintf(buf, "%u\n", p->rate_limit_us);
	if (!strcmp(n, "up_rate_limit_us"))
		return sprintf(buf, "%u\n", p->up_rate_limit_us);
	if (!strcmp(n, "down_rate_limit_us"))
		return sprintf(buf, "%u\n", p->down_rate_limit_us);
	if (!strcmp(n, "target_loads"))
		return sprintf(buf, "%u\n", p->target_loads);
	if (!strcmp(n, "soft_freq_min"))
		return sprintf(buf, "%u\n", p->soft_freq_min);
	if (!strcmp(n, "soft_freq_max"))
		return sprintf(buf, "%u\n", p->soft_freq_max);
	if (!strcmp(n, "apply_freq_immediately"))
		return sprintf(buf, "%u\n", p->apply_freq_immediately ? 1 : 0);
	if (!strcmp(n, "prefer_perf_hint"))
		return sprintf(buf, "%u\n", p->prefer_perf_hint ? 1 : 0);

	return -EINVAL;
}

static ssize_t dsh_view_store(struct kobject *kobj, struct attribute *attr,
			      const char *buf, size_t count)
{
	struct dsh_gov_view *v = container_of(kobj, struct dsh_gov_view, kobj);
	struct dsh_gov_params *p = &v->p;
	const char *n = attr->name;
	unsigned int val;
	bool bval;

	if (!strcmp(n, "apply_freq_immediately")) {
		if (kstrtobool(buf, &bval))
			return -EINVAL;
		p->apply_freq_immediately = bval;
		return count;
	}
	if (!strcmp(n, "prefer_perf_hint")) {
		if (kstrtobool(buf, &bval))
			return -EINVAL;
		p->prefer_perf_hint = bval;
		return count;
	}

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	if (!strcmp(n, "rate_limit_us")) {
		p->rate_limit_us = val;
		p->up_rate_limit_us = val;
		p->down_rate_limit_us = val;
	} else if (!strcmp(n, "up_rate_limit_us")) {
		p->up_rate_limit_us = val;
		p->rate_limit_us = val;
	} else if (!strcmp(n, "down_rate_limit_us")) {
		p->down_rate_limit_us = val;
		p->rate_limit_us = val;
	} else if (!strcmp(n, "target_loads")) {
		p->target_loads = val;
	} else if (!strcmp(n, "soft_freq_min")) {
		p->soft_freq_min = val;
	} else if (!strcmp(n, "soft_freq_max")) {
		p->soft_freq_max = val;
	} else {
		return -EINVAL;
	}

	return count;
}

/*******************************************************************************
 * governor plumbing
 ******************************************************************************/

static int dsh_view_create(int idx);
static void dsh_activate_workfn(struct work_struct *work);

static struct dsh_gov_policy *dsh_policy_alloc(struct cpufreq_policy *policy)
{
	struct dsh_gov_policy *dg;

	dg = kzalloc(sizeof(*dg), GFP_KERNEL);
	if (!dg)
		return NULL;

	dg->policy = policy;
	dg->params = dsh_params_of(policy);
	raw_spin_lock_init(&dg->update_lock);
	mutex_init(&dg->work_lock);

	return dg;
}

static void dsh_policy_free(struct dsh_gov_policy *dg)
{
	mutex_destroy(&dg->work_lock);
	kfree(dg);
}

static void dsh_cpu_reset(struct dsh_gov_policy *dg)
{
	unsigned int cpu;

	for_each_cpu(cpu, dg->policy->cpus)
		per_cpu(dsh_gov_cpu, cpu).dg_policy = NULL;
}

static int dsh_kthread_create(struct dsh_gov_policy *dg)
{
	struct cpufreq_policy *policy = dg->policy;
	struct sched_attr attr = {
		.size		= sizeof(struct sched_attr),
		.sched_policy	= SCHED_DEADLINE,
		.sched_flags	= SCHED_FLAG_SUGOV,
		.sched_nice	= 0,
		.sched_priority	= 0,
		.sched_runtime	= 1000000,
		.sched_deadline	= 10000000,
		.sched_period	= 10000000,
	};
	struct task_struct *thread;
	char wname[32];
	int ret;

	snprintf(wname, sizeof(wname), "dsh_gov/%u", policy->cpu);

	kthread_init_worker(&dg->worker);
	thread = kthread_create(kthread_worker_fn, &dg->worker, "%s", wname);
	if (IS_ERR(thread)) {
		pr_err("dsh_gov: cannot create worker for cpu%u (%ld)\n",
		       policy->cpu, PTR_ERR(thread));
		return PTR_ERR(thread);
	}

	ret = sched_setattr_nocheck(thread, &attr);
	if (ret) {
		kthread_stop(thread);
		pr_warn("dsh_gov: SCHED_DEADLINE refused on cpu%u (%d)\n",
			policy->cpu, ret);
		return ret;
	}

	kthread_init_work(&dg->work, dsh_gov_work);
	dg->thread = thread;

	return 0;
}

static void dsh_kthread_stop(struct dsh_gov_policy *dg)
{
	if (dg->thread) {
		kthread_flush_worker(&dg->worker);
		kthread_stop(dg->thread);
		dg->thread = NULL;
	}
}

static int dsh_init(struct cpufreq_policy *policy)
{
	struct dsh_gov_policy *dg;

	if (!policy->freq_table)
		return -ENODEV;
	if (dsh_gov_idx(policy->governor) < 0)
		return -EINVAL;

	/*
	 * The global cpu/cpufreq kobject is static to cpufreq.c, but every
	 * policy kobject is created directly beneath it, so policy->kobj.parent
	 * is exactly what /sys/devices/system/cpu/cpufreq/<name>/ needs.
	 */
	/*
	 * Deliberately minimal: allocate and record.  No kthread, no sysfs
	 * directory, no update hook.  Those are created by
	 * dsh_activate_workfn(), which is only reached once the system is up.
	 */
	if (!dsh_cpufreq_kobj)
		dsh_cpufreq_kobj = policy->kobj.parent;

	dg = dsh_policy_alloc(policy);
	if (!dg)
		return -ENOMEM;

	INIT_DELAYED_WORK(&dg->activate_work, dsh_activate_workfn);
	policy->governor_data = dg;

	return 0;
}

static void dsh_exit(struct cpufreq_policy *policy)
{
	struct dsh_gov_policy *dg = policy->governor_data;

	if (!dg)
		return;

	policy->governor_data = NULL;
	cancel_delayed_work_sync(&dg->activate_work);
	dg->hooks_installed = false;
	dg->activated = false;
	dsh_cpu_reset(dg);
	if (dg->thread)
		dsh_kthread_stop(dg);
	dsh_policy_free(dg);
	cpufreq_disable_fast_switch(policy);
}

static void dsh_install_hooks(struct dsh_gov_policy *dg)
{
	unsigned int cpu;

	if (dg->hooks_installed)
		return;

	for_each_cpu(cpu, dg->policy->cpus) {
		struct dsh_gov_cpu *dg_cpu = &per_cpu(dsh_gov_cpu, cpu);

		dg_cpu->cpu = cpu;
		dg_cpu->dg_policy = dg;
	}

	for_each_cpu(cpu, dg->policy->cpus)
		cpufreq_add_update_util_hook(cpu,
					     &per_cpu(dsh_gov_cpu, cpu).update_util,
					     dsh_update_util);

	dg->hooks_installed = true;
	dg->last_freq_update_time = 0;
	dg->last_freq = 0;

	pr_info("dsh_gov: hooks installed on cpu%u (governor now driving frequency)\n",
		dg->policy->cpu);
}

/*
 * Everything that can touch shared state -- creating the SCHED_DEADLINE worker,
 * publishing the tunables directory, installing the update hook -- happens here
 * and nowhere else.  During boot the governor is selected but completely inert:
 * dsh_init() only allocates memory.  That is what makes selecting scx from a
 * saved setting as safe as selecting walt.
 */
static void dsh_activate_workfn(struct work_struct *work)
{
	struct dsh_gov_policy *dg = container_of(to_delayed_work(work),
						 struct dsh_gov_policy, activate_work);
	int ret;

	if (dg->activated)
		return;

	pr_info("dsh_gov: activating governor for cpu%u (system is up)\n",
		dg->policy->cpu);

	ret = dsh_kthread_create(dg);
	if (ret) {
		pr_err("dsh_gov: cpu%u cannot create worker (%d), leaving governor inert\n",
		       dg->policy->cpu, ret);
		return;
	}

	dsh_view_create(dsh_gov_idx(dg->policy->governor));
	dsh_install_hooks(dg);

	dg->activated = true;
}

static int dsh_start(struct cpufreq_policy *policy)
{
	struct dsh_gov_policy *dg = policy->governor_data;

	if (!dg)
		return -EINVAL;

	/*
	 * While the system is still booting, stay hands-off: record the policy
	 * and do nothing else.  The work item is queued so this cannot be
	 * forgotten if the governor is selected during early boot and nobody
	 * ever re-selects it.
	 */
	if (system_state == SYSTEM_RUNNING) {
		dsh_activate_workfn(&dg->activate_work.work);
	} else {
		pr_info("dsh_gov: cpu%u selected during boot -- governor stays inert for 5s\n",
			policy->cpu);
		schedule_delayed_work(&dg->activate_work, msecs_to_jiffies(5000));
	}

	return 0;
}

static void dsh_stop(struct cpufreq_policy *policy)
{
	struct dsh_gov_policy *dg = policy->governor_data;
	unsigned int cpu;

	cancel_delayed_work_sync(&dg->activate_work);

	if (dg->hooks_installed) {
		for_each_cpu(cpu, policy->cpus) {
			struct dsh_gov_cpu *dg_cpu = &per_cpu(dsh_gov_cpu, cpu);

			dg_cpu->dg_policy = NULL;
		}
		synchronize_rcu();
		for_each_cpu(cpu, policy->cpus)
			cpufreq_remove_update_util_hook(cpu);
		dg->hooks_installed = false;
	}

	if (dg->thread)
		kthread_flush_worker(&dg->worker);
}

static void dsh_limits(struct cpufreq_policy *policy)
{
	struct dsh_gov_policy *dg = policy->governor_data;
	u32 perf;

	if (!dg)
		return;

	mutex_lock(&dg->work_lock);
	cpufreq_policy_apply_limits(policy);
	mutex_unlock(&dg->work_lock);

	perf = dsh_perf_for_cpu(policy->cpu,
			       dg->params ? dg->params->prefer_perf_hint : false);
	dsh_update_cpufreq(dg, perf);
}

static struct cpufreq_governor dsh_scx_gov = {
	.name	= "scx",
	.owner	= THIS_MODULE,
	.flags	= CPUFREQ_GOV_DYNAMIC_SWITCHING,
	.init	= dsh_init,
	.exit	= dsh_exit,
	.start	= dsh_start,
	.stop	= dsh_stop,
	.limits	= dsh_limits,
};

static struct cpufreq_governor dsh_sugov_ext_gov = {
	.name	= "sugov_ext",
	.owner	= THIS_MODULE,
	.flags	= CPUFREQ_GOV_DYNAMIC_SWITCHING,
	.init	= dsh_init,
	.exit	= dsh_exit,
	.start	= dsh_start,
	.stop	= dsh_stop,
	.limits	= dsh_limits,
};

/* create /sys/devices/system/cpu/cpufreq/<name>/ with the tuning attributes */
static int dsh_view_create(int idx)
{
	struct dsh_gov_view *v = &dsh_views[idx];

	if (dsh_view_kobj[idx])
		return 0;

	v->p.rate_limit_us = 1000;
	v->p.up_rate_limit_us = 1000;
	v->p.down_rate_limit_us = 1000;
	v->p.target_loads = DSH_DEFAULT_TARGET_LOAD;
	v->p.apply_freq_immediately = false;
	/*
	 * The two names are deliberately different governors:
	 *   scx       -- fengchi: obeys the per-CPU performance request
	 *   sugov_ext -- schedutil semantics: utilisation only
	 */
	v->p.prefer_perf_hint = (idx == DSH_GOV_IDX_SCX);

	if (kobject_init_and_add(&v->kobj, &dsh_view_ktype,
				 dsh_cpufreq_kobj, "%s",
				 dsh_gov_names[idx])) {
		pr_err("dsh_gov: cannot create tunables dir for '%s'\n",
		       dsh_gov_names[idx]);
		return -ENOMEM;
	}
	kobject_uevent(&v->kobj, KOBJ_ADD);
	dsh_view_kobj[idx] = &v->kobj;

	return 0;
}

static int __init dsh_gov_init(void)
{
	int ret;

	ret = cpufreq_register_governor(&dsh_scx_gov);
	if (ret) {
		pr_err("dsh_gov: register 'scx' failed (%d)\n", ret);
		return ret;
	}
	pr_info("dsh_gov: governor 'scx' registered\n");

	ret = cpufreq_register_governor(&dsh_sugov_ext_gov);
	if (ret)
		pr_err("dsh_gov: register 'sugov_ext' failed (%d)\n", ret);
	else
		pr_info("dsh_gov: governor 'sugov_ext' registered\n");

	return 0;
}
late_initcall(dsh_gov_init);
