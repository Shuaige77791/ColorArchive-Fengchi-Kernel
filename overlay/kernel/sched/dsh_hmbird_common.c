// SPDX-License-Identifier: GPL-2.0-only
/*
 * DSH hmbird common sysctl -- the userspace-facing half of OPLUS's fengchi
 * stack that works on this tree.
 *
 * Ported from the vendor tree:
 *	vendor/oplus/kernel/cpu/sched_ext/hmbird_common_sysctl.c  (4,128 bytes)
 *
 * The vendor file is one of the few pieces of that stack that does not depend on
 * sched_ext, on struct oplus_task_struct, or on the seven android_vh_scx_* hooks
 * this tree does not have -- so it is the part worth carrying over verbatim:
 *
 *	/proc/sys/hmbird/common/hmbird_bpf_log_level
 *		verbosity of the fengchi scheduler, 0 (off) .. 5 (debug).
 *	/proc/sys/hmbird/common/hmbird_manager_register
 *		write a pid and that task becomes the fengchi manager: it is put
 *		on SCHED_FIFO just below the top RT priority so it can never be
 *		starved by the workloads it is managing.
 *
 * Deliberate differences from the vendor copy:
 *   - the trace_hmbird_bpf_log_level_update() tracepoint is dropped; it only
 *     fed a private trace buffer that does not exist here.
 *   - everything else (semantics, defaults, error codes, locking) is kept.
 *   - proc_handler takes a non-const struct ctl_table * here; 6.6 made it const.
 */

#include <linux/slab.h>
#include <linux/sysctl.h>
#include <linux/kmemleak.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/pid.h>
#include <linux/sched/task.h>
#include <uapi/linux/sched/types.h>

enum {
	HMBIRD_BPF_LOG_OFF_LEVEL	= 0,
	HMBIRD_BPF_LOG_CRITICAL_LEVEL	= 1,
	HMBIRD_BPF_LOG_ERR_LEVEL	= 2,
	HMBIRD_BPF_LOG_WARN_LEVEL	= 3,
	HMBIRD_BPF_LOG_INFO_LEVEL	= 4,
	HMBIRD_BPF_LOG_DBG_LEVEL	= 5,
	HMBIRD_BPF_LOG_LEVEL_MAX	= HMBIRD_BPF_LOG_DBG_LEVEL
};

unsigned int hmbird_bpf_log_level = HMBIRD_BPF_LOG_ERR_LEVEL;

/*
 * The fengchi helper task.  hmbird_common.h in the vendor tree declares this
 * extern, so it is defined here to keep that contract.
 */
struct task_struct *sched_ext_helper;

static int cfg_hmbird_bpf_log_level(struct ctl_table *table, int write,
				    void *buffer, size_t *lenp, loff_t *ppos)
{
	int ret = -EPERM;
	unsigned int val;
	static DEFINE_MUTEX(mutex);
	struct ctl_table tmp = {
		.data		= &val,
		.maxlen		= sizeof(val),
		.mode		= table->mode,
	};

	mutex_lock(&mutex);
	if (!write) {
		val = hmbird_bpf_log_level;
		ret = proc_dointvec(&tmp, write, buffer, lenp, ppos);
	} else {
		ret = proc_dointvec(&tmp, write, buffer, lenp, ppos);
		if (ret) {
			pr_warn("hmbird: set bpf_log_level failed, ret %d\n", ret);
			goto unlock;
		}

		if (val < HMBIRD_BPF_LOG_OFF_LEVEL || val > HMBIRD_BPF_LOG_LEVEL_MAX) {
			ret = -EINVAL;
			pr_warn("hmbird: illegal LOG_LEVEL %d\n", val);
			goto unlock;
		}

		hmbird_bpf_log_level = val;
	}

unlock:
	mutex_unlock(&mutex);
	return ret;
}

static struct task_struct *oplus_hmbird_bpf_manager;
#define OPLUS_HMBIRD_BPF_MANAGER_PRIO	10

static int hmbird_bpf_manager_register(struct ctl_table *table, int write,
				       void *buffer, size_t *lenp, loff_t *ppos)
{
	int ret = -EPERM;
	int pid = -1;
	struct task_struct *manager;
	struct sched_param param = {
		.sched_priority = MAX_RT_PRIO - 1 - OPLUS_HMBIRD_BPF_MANAGER_PRIO
	};
	static DEFINE_MUTEX(mutex);
	struct ctl_table tmp = {
		.data		= &pid,
		.maxlen		= sizeof(pid),
		.mode		= table->mode,
	};

	mutex_lock(&mutex);

	if (write) {
		ret = proc_dointvec(&tmp, write, buffer, lenp, ppos);
		if (ret) {
			ret = -EFAULT;
			goto unlock;
		}
		manager = get_pid_task(find_vpid(pid), PIDTYPE_PID);
		if (!manager) {
			ret = ESRCH;
			pr_err("hmbird: manager_register: no such process\n");
			goto unlock;
		}

		if (manager == oplus_hmbird_bpf_manager) {
			pr_err("hmbird: manager_register: already registered\n");
			put_task_struct(manager);
			goto unlock;
		}

		ret = sched_setscheduler_nocheck(manager, SCHED_FIFO, &param);
		if (ret) {
			pr_err("hmbird: manager_register: sched_setscheduler_nocheck failed\n");
			put_task_struct(manager);
			goto unlock;
		}

		if (oplus_hmbird_bpf_manager) {
			put_task_struct(oplus_hmbird_bpf_manager);
			oplus_hmbird_bpf_manager = NULL;
		}
		oplus_hmbird_bpf_manager = manager;
		sched_ext_helper = manager;
	} else {
		char buf[32];

		if (oplus_hmbird_bpf_manager)
			snprintf(buf, sizeof(buf), "%s %d\n",
				 oplus_hmbird_bpf_manager->comm,
				 oplus_hmbird_bpf_manager->pid);
		else
			snprintf(buf, sizeof(buf), "none\n");
		tmp.data = buf;
		tmp.maxlen = sizeof(buf);
		ret = proc_dostring(&tmp, write, buffer, lenp, ppos);
	}

unlock:
	mutex_unlock(&mutex);
	return ret;
}

static struct ctl_table hmbird_common_table[] = {
	{
		.procname	= "hmbird_bpf_log_level",
		.data		= &hmbird_bpf_log_level,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= cfg_hmbird_bpf_log_level,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_INT_MAX,
	},
	{
		.procname	= "hmbird_manager_register",
		.data		= &oplus_hmbird_bpf_manager,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= hmbird_bpf_manager_register,
	},
	{ }
};

static struct ctl_table_header *hmbird_common_hdr;

static int __init dsh_hmbird_common_init(void)
{
	hmbird_common_hdr = register_sysctl("hmbird/common", hmbird_common_table);
	kmemleak_not_leak(hmbird_common_hdr);
	pr_info("hmbird: /proc/sys/hmbird/common registered\n");
	return 0;
}
late_initcall(dsh_hmbird_common_init);
