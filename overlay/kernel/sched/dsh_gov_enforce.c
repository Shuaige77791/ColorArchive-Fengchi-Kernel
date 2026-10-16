// SPDX-License-Identifier: GPL-2.0-only
/*
 * DSH governor enforcement -- keep "scx" selected on every cpufreq policy.
 *
 * WHY
 * ---
 * The fengchi governor ("scx") is the point of this port, but selecting a
 * cpufreq governor is a userspace decision, and on this device vendor init
 * rewrites scaling_governor during boot (the stock image boots with "walt" even
 * though the kernel command line asks for "performance").  Setting a default in
 * the command line is therefore not enough.
 *
 * This file makes "scx" the effective global default and keeps it there.  It
 * uses cpufreq_set_governor_by_name(), which the port adds to
 * drivers/cpufreq/cpufreq.c and which performs exactly the same locked operation
 * as writing scaling_governor from userspace.
 *
 * BEHAVIOUR
 * ---------
 *   - shortly after boot, and then periodically, every distinct cpufreq policy
 *     is checked; any policy not running dsh_default_gov is switched to it.
 *   - /proc/dsh_gov/enforce=0 stops the enforcement (writes 0/n), so a user who
 *     prefers walt or schedutil can say so and be left alone.
 *   - /proc/dsh_gov/default changes the target name.
 *   - /proc/dsh_gov/stats reports the current governor of every policy, the
 *     switch count and the last error, so the effect is observable.
 *
 * Everything runs from a delayed workqueue in process context; no structure
 * layout is touched, so exported symbol CRCs are unaffected.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/workqueue.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/delay.h>

/*
 * The OPLUS fengchi knob.  It lives in kernel/sched/hmbird/hmbird_sched_proc.c
 * and is exposed as /proc/hmbird_sched/scx_gov_ctrl, but nothing in this tree
 * reads it -- it is a dead switch.  Wiring it here gives the hmbird-named
 * interface real authority over the real fengchi governor:
 *
 *	scx_gov_ctrl == 1  -> keep "scx" selected on every policy
 *	scx_gov_ctrl == 0  -> stop enforcing and leave the choice to userspace
 */
extern int scx_gov_ctrl;

#define DSH_GOV_NAME_LEN	CPUFREQ_NAME_LEN
#define DSH_FIRST_DELAY_MS	45000	/* escape window: userspace can write enforce=0 */
#define DSH_BOOT_RETRIES	4
#define DSH_STEADY_DELAY_MS	30000
#define DSH_MAX_ATTEMPTS	3

static char dsh_default_gov[DSH_GOV_NAME_LEN] = "scx";
static bool dsh_enforce = true;
static unsigned long dsh_switch_count;
static unsigned long dsh_fail_count;
static int dsh_last_err;
static int dsh_rounds;
static struct delayed_work dsh_gov_work;
static bool dsh_running;

/*
 * Per-policy health state.  The governor this port ships has not been validated
 * on hardware at the time of writing, and a governor that misbehaves would take
 * the whole device with it.  So every switch is treated as an experiment:
 *
 *   - the governor found before the first attempt is remembered;
 *   - if, after DSH_MAX_ATTEMPTS rounds, the policy still is not running the
 *     requested governor, that is taken as "this does not work here", the
 *     original governor is restored and enforcement for that policy stops for
 *     good (until reboot).
 *
 * The result is that an unsupported/unstable governor degrades to the vendor
 * default within a couple of minutes instead of producing a boot loop.
 */
struct dsh_policy_state {
	char	prev[DSH_GOV_NAME_LEN];
	int	attempts;
	bool	give_up;
};

static struct dsh_policy_state dsh_states[NR_CPUS];

static void dsh_gov_check(void)
{
	unsigned int cpu;
	int switched = 0, failed = 0, reverted = 0;

	for_each_possible_cpu(cpu) {
		struct cpufreq_policy *policy;
		struct dsh_policy_state *st = &dsh_states[cpu];
		char cur[DSH_GOV_NAME_LEN] = "";
		const char *want;
		int ret;

		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;

		/* one entry per policy: skip the non-lead CPUs */
		if (policy->cpu != cpu) {
			cpufreq_cpu_put(policy);
			continue;
		}

		down_read(&policy->rwsem);
		if (policy->governor)
			strscpy(cur, policy->governor->name, sizeof(cur));
		up_read(&policy->rwsem);
		cpufreq_cpu_put(policy);

		want = dsh_default_gov;

		if (!strcmp(cur, want)) {
			st->attempts = 0;	/* healthy */
			continue;
		}

		if (st->give_up)
			continue;

		if (!st->attempts)
			strscpy(st->prev, cur, sizeof(st->prev));

		if (st->attempts >= DSH_MAX_ATTEMPTS) {
			/*
			 * It never took.  Put back what was there and stop
			 * touching this policy -- the safe outcome.
			 */
			if (st->prev[0]) {
				ret = cpufreq_set_governor_by_name(cpu, st->prev);
				pr_err("dsh_gov: cpu%u refused '%s' %d times, restored '%s' (ret %d)\n",
				       cpu, want, st->attempts, st->prev, ret);
				reverted++;
			}
			st->give_up = true;
			continue;
		}

		ret = cpufreq_set_governor_by_name(cpu, want);
		if (ret) {
			failed++;
			dsh_last_err = ret;
			st->attempts++;
		} else {
			switched++;
			st->attempts++;
			pr_info("dsh_gov: cpu%u governor %s -> %s (attempt %d)\n",
				cpu, cur[0] ? cur : "?", want, st->attempts);
		}
	}

	if (switched)
		dsh_switch_count += switched;
	if (failed)
		dsh_fail_count += failed;
	if (reverted)
		pr_warn("dsh_gov: %d policy(ies) reverted; enforcement stopped for them\n",
			reverted);
}

static void dsh_gov_workfn(struct work_struct *work)
{
	unsigned long delay;

	if (!READ_ONCE(dsh_enforce))
		goto rearm;

	/*
	 * hmbird's own knob wins when it asks for enforcement to stop, so
	 * OPLUS userspace can hand control back to walt/schedutil.
	 */
	if (!READ_ONCE(scx_gov_ctrl))
		goto rearm;

	dsh_gov_check();
	dsh_rounds++;

rearm:
	/*
	 * Re-check a few times while vendor init is still settling, then settle
	 * into a slow steady poll so a stray userspace write cannot silently
	 * take the port off its governor.
	 */
	if (dsh_rounds < DSH_BOOT_RETRIES)
		delay = msecs_to_jiffies(DSH_FIRST_DELAY_MS);
	else
		delay = msecs_to_jiffies(DSH_STEADY_DELAY_MS);

	schedule_delayed_work(&dsh_gov_work, delay);
}

/* ---- /proc/dsh_gov/ -------------------------------------------------------- */

static int dsh_gov_stats_show(struct seq_file *m, void *v)
{
	unsigned int cpu;

	seq_printf(m, "default governor : %s\n", dsh_default_gov);
	seq_printf(m, "enforce          : %d\n", READ_ONCE(dsh_enforce) ? 1 : 0);
	seq_printf(m, "scx_gov_ctrl     : %d  (hmbird /proc/hmbird_sched/scx_gov_ctrl)\n",
		   READ_ONCE(scx_gov_ctrl));
	seq_printf(m, "rounds           : %d\n", dsh_rounds);
	seq_printf(m, "switches         : %lu\n", dsh_switch_count);
	seq_printf(m, "failures         : %lu\n", dsh_fail_count);
	seq_printf(m, "last error       : %d\n", dsh_last_err);
	seq_puts(m, "per-policy governor:\n");

	for_each_possible_cpu(cpu) {
		struct cpufreq_policy *policy;
		char cur[DSH_GOV_NAME_LEN] = "?";

		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;
		if (policy->cpu != cpu) {
			cpufreq_cpu_put(policy);
			continue;
		}
		down_read(&policy->rwsem);
		if (policy->governor)
			strscpy(cur, policy->governor->name, sizeof(cur));
		up_read(&policy->rwsem);
		cpufreq_cpu_put(policy);

		seq_printf(m, "  policy%u : %-12s attempts=%d%s\n", cpu, cur,
			   dsh_states[cpu].attempts,
			   dsh_states[cpu].give_up ? "  [gave up, reverted]" : "");
	}

	return 0;
}
static int dsh_gov_stats_open(struct inode *i, struct file *f)
{
	return single_open(f, dsh_gov_stats_show, NULL);
}
static const struct proc_ops dsh_gov_stats_pops = {
	.proc_open	= dsh_gov_stats_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int dsh_gov_default_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%s\n", dsh_default_gov);
	return 0;
}
static int dsh_gov_default_open(struct inode *i, struct file *f)
{
	return single_open(f, dsh_gov_default_show, NULL);
}
static ssize_t dsh_gov_default_write(struct file *f, const char __user *ubuf,
				     size_t cnt, loff_t *ppos)
{
	char buf[DSH_GOV_NAME_LEN];

	if (cnt == 0 || cnt >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, cnt))
		return -EFAULT;
	buf[cnt] = '\0';
	strim(buf);
	if (!buf[0])
		return -EINVAL;
	strscpy(dsh_default_gov, buf, sizeof(dsh_default_gov));

	return cnt;
}
static const struct proc_ops dsh_gov_default_pops = {
	.proc_open	= dsh_gov_default_open,
	.proc_read	= seq_read,
	.proc_write	= dsh_gov_default_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int dsh_gov_enforce_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", READ_ONCE(dsh_enforce) ? 1 : 0);
	return 0;
}
static int dsh_gov_enforce_open(struct inode *i, struct file *f)
{
	return single_open(f, dsh_gov_enforce_show, NULL);
}
static ssize_t dsh_gov_enforce_write(struct file *f, const char __user *ubuf,
				     size_t cnt, loff_t *ppos)
{
	char buf[8];
	bool val;

	if (cnt == 0 || cnt >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, cnt))
		return -EFAULT;
	buf[cnt] = '\0';
	if (kstrtobool(buf, &val))
		return -EINVAL;
	WRITE_ONCE(dsh_enforce, val);
	if (val) {
		dsh_rounds = 0;
		cancel_delayed_work(&dsh_gov_work);
		schedule_delayed_work(&dsh_gov_work, msecs_to_jiffies(1000));
	}

	return cnt;
}
static const struct proc_ops dsh_gov_enforce_pops = {
	.proc_open	= dsh_gov_enforce_open,
	.proc_read	= seq_read,
	.proc_write	= dsh_gov_enforce_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static struct proc_dir_entry *dsh_gov_dir;

static int __init dsh_gov_enforce_init(void)
{
	dsh_gov_dir = proc_mkdir("dsh_gov", NULL);
	if (!dsh_gov_dir) {
		pr_err("dsh_gov: cannot create /proc/dsh_gov\n");
		return -ENOMEM;
	}
	proc_create("stats", 0444, dsh_gov_dir, &dsh_gov_stats_pops);
	proc_create("default", 0644, dsh_gov_dir, &dsh_gov_default_pops);
	proc_create("enforce", 0644, dsh_gov_dir, &dsh_gov_enforce_pops);

	INIT_DELAYED_WORK(&dsh_gov_work, dsh_gov_workfn);
	schedule_delayed_work(&dsh_gov_work,
			      msecs_to_jiffies(DSH_FIRST_DELAY_MS));
	dsh_running = true;

	pr_info("dsh_gov: will keep governor '%s' selected (enforce=%d)\n",
		dsh_default_gov, dsh_enforce ? 1 : 0);
	return 0;
}
late_initcall(dsh_gov_enforce_init);
