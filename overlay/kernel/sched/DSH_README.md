# DSH fengchi governor + unfair scheduling for 5.10.236-ColorArchive

Four new files, all self-contained.  Nothing in this set changes the layout of
`struct task_struct`, `struct rq`, `struct cfs_rq` or `struct cpufreq_policy`, so
exported symbol CRCs are untouched (verified: 14674 common symbols, 0 changed,
99.8% match against the stock OPLUS kernel).

## dsh_fengchi_gov.c -- the real fengchi CPU governor
Ported from the vendor governor
`vendor/oplus/kernel/cpu/sched_ext/hmbird_II/hmbird_II_freqgov.c` (29,570 bytes),
whose core maths is kept verbatim:

    perf_tl = perf * 100 / target_load
    next_f  = max_freq * perf_tl / arch_scale_cpu_capacity(cpu)

plus `soft_freq_clamp()`, the SCHED_DEADLINE worker kthread packed like schedutil,
`fast_switch` when asked, and `__cpufreq_driver_target()` otherwise.

Registered under two names, and they are **not** aliases:

| name        | input                                                   |
|-------------|---------------------------------------------------------|
| `scx`       | the per-CPU performance request (unfair scheduling 2.0)  |
| `sugov_ext` | measured utilisation only (schedutil semantics)          |

The vendor's own input, `rq->scx.cpuperf_target`, is written only by the vendor's
BPF scheduler through a kfunc.  sched_ext cannot be enabled on this tree -- it
hard-hangs the SoC -- so `scx` reads `dsh_cpuperf_target` instead (see below).

The tunables live at `/sys/devices/system/cpu/cpufreq/{scx,sugov_ext}/`:
`rate_limit_us`, `up_rate_limit_us`, `down_rate_limit_us`, `target_loads`,
`soft_freq_min`, `soft_freq_max`, `apply_freq_immediately`, `prefer_perf_hint`.
The rate limits are real: the update hook refuses to ramp up or down faster than
they allow.  This platform sets `CPUFREQ_HAVE_GOVERNOR_PER_POLICY`, so the
directory is created explicitly under the global cpu/cpufreq kobject.

## dsh_unfair.c -- unfair scheduling 2.0 + power saving
Produces the performance request the vendor BPF scheduler used to produce, using
hooks this tree already has: `android_vh_account_task_time` and
`android_vh_scheduler_tick` (both `vh`, i.e. multi-registrant -- OPLUS's
`sched_assist` module uses the exclusive `rvh` family, so the two coexist; this
was verified against the shipped module binaries).

A task is critical if it belongs to the foreground tgid, is a registered pid, or
carries a boosted weight.  Critical CPUs get `boost_perf`; everything else is
capped at `idle_perf`, which is where the power saving comes from.
`/proc/dsh_unfair/` exposes `stats`, `enabled`, `target_tgid`, `boost_perf`,
`idle_perf`, `heavy_weight`, `add_pid`, `clear`.

## dsh_gov_enforce.c -- make scx the global default
Selecting a cpufreq governor is a userspace decision and vendor init rewrites
`scaling_governor` during boot, so a command-line default is not enough.  This
keeps `scx` selected: a delayed workqueue checks every policy after boot and then
periodically.  It is governed by hmbird's own knob
`/proc/hmbird_sched/scx_gov_ctrl` (which was a dead switch in this tree -- nothing
read it) and by `/proc/dsh_gov/{stats,default,enforce}`.

## drivers/cpufreq/cpufreq.c -- one added symbol
`cpufreq_set_governor_by_name()`: selects a governor from kernel context using the
same locking as the sysfs path (`cpus_read_trylock`, `cpufreq_cpu_get`,
`down_write(&policy->rwsem)`, `cpufreq_set_governor_by_name`).  It is the only new
exported symbol; existing CRCs are unchanged.

## dsh_hmbird_common.c -- the one vendor file worth carrying
`/proc/sys/hmbird/common/hmbird_bpf_log_level` and
`/proc/sys/hmbird/common/hmbird_manager_register` (writing a pid puts that task on
SCHED_FIFO just below top RT so it cannot be starved).

## What does **not** work, and why
* `sched_ext` cannot be enabled: switching the class on hard-hangs the SoC
  (PMIC PSHOLD reset, no software log).  `scx_ops_enable()` is line-for-line
  identical to the reference, and copying the upstream `scx_example_simple` ops
  still hangs, so this is not a porting defect.  `/sys/kernel/sched_ext/*` is
  created and an `enable` attribute is provided, but the default is off.
* The hmbird scheduling class has the same problem.
* The shadow tick is implemented and gated off by default (`dsh_shadow_tick`); a
  variant with it on is published as `boot_V20_shadowtick_ksu.img` for testing.
* The vendor half (7,016 lines / 30 files) is a sched_ext companion and is gated
  behind the device-tree node `/soc/oplus,hmbird/config_type`, which this device
  does not have; only the sysctl piece is portable and is ported above.
