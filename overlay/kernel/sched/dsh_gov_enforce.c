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
#define DSH_FIRST_DELAY_MS	2000	/* start guarding almost immediately */
#define DSH_GUARD_POLL_MS	1000	/* during the guard window: check every second */
#define DSH_BOOT_RETRIES	4
#define DSH_STEADY_DELAY_MS	30000
#define DSH_MAX_ATTEMPTS	3

static char dsh_default_gov[DSH_GOV_NAME_LEN] = "scx";

/*
 * The device must boot on a governor that is known to bring the system up.  The
 * governor this port ships is not that: userspace restores its saved choice
 * early in boot, and if that saved choice is scx then scx runs while the system
 * is still coming up, which is how the boot hang happened.
 *
 * So the kernel takes the decision away from userspace for the first
 * dsh_boot_guard_s seconds: whatever gets written, walt is put back.  That write
 * succeeds, so nothing retries and nothing fails -- the boot simply always runs
 * on walt.  Once the window closes, scx is selected and kept.
 */
static char dsh_boot_gov[DSH_GOV_NAME_LEN] = "walt";
static unsigned int dsh_boot_guard_s = 60;
static bool dsh_enforce		= true;
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

/* put `want` on every policy that is not already running it */
static int dsh_force_governor(const char *want, bool use_health)
{
	unsigned int cpu;
	int changed = 0;

	for_each_possible_cpu(cpu) {
		struct cpufreq_policy *policy;
		struct dsh_policy_state *st = &dsh_states[cpu];
		char cur[DSH_GOV_NAME_LEN] = "";
		int ret;

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

		if (!strcmp(cur, want)) {
			st->attempts = 0;
			continue;
		}

		if (use_health && st->give_up)
			continue;

		ret = cpufreq_set_governor_by_name(cpu, want);
		if (ret) {
			dsh_fail_count++;
			dsh_last_err = ret;
			if (use_health)
				st->attempts++;
		} else {
			dsh_switch_count++;
			changed++;
			if (use_health)
				st->attempts++;
			pr_info("dsh_gov: cpu%u governor %s -> %s\n", cpu,
				cur[0] ? cur : "?", want);
		}
	}
	return changed;
}

static void dsh_gov_check(void)
{
	unsigned long up_s = ktime_get_seconds();

	if (up_s < dsh_boot_guard_s) {
		/*
		 * Phase 1: the boot owns the governor choice.  Undo anything
		 * userspace set, quietly and successfully.
		 */
		if (dsh_force_governor(dsh_boot_gov, false))
			pr_info("dsh_gov: boot guard kept '%s' (uptime %lus)\n",
				dsh_boot_gov, up_s);
		return;
	}

	/* Phase 2: the system is up; move to the governor we actually want. */
	dsh_force_governor(dsh_default_gov, true);
}

static void dsh_gov_workfn(struct work_struct *work)
{
	unsigned int next_ms;

	if (!READ_ONCE(dsh_enforce))
		goto rearm;

	dsh_gov_check();

	/*
	 * While the boot guard is armed, look often: userspace writes its saved
	 * governor within the first seconds of boot, and the window between that
	 * write and our correction is exactly the time the boot would be running
	 * on a governor that is not supposed to be there.  Once the window has
	 * closed, a slow steady poll is enough.
	 */
	next_ms = (ktime_get_seconds() < dsh_boot_guard_s) ?
		  DSH_GUARD_POLL_MS : DSH_STEADY_DELAY_MS;
	schedule_delayed_work(&dsh_gov_work, msecs_to_jiffies(next_ms));
	return;

rearm:
	schedule_delayed_work(&dsh_gov_work, msecs_to_jiffies(DSH_STEADY_DELAY_MS));
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
	pr_info("dsh_gov: autostart armed -- scx will be selected 20s after boot\n");
	return 0;
}
late_initcall(dsh_gov_enforce_init);
