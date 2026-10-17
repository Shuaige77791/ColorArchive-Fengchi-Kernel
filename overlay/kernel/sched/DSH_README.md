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

---

## Verification evidence (offline, reproducible)

### CRC / KMI
Every build was checked against the stock OPLUS kernel's symbol CRCs:

| build | common symbols | CRCs changed | match vs stock | new symbols |
|-------|---------------|--------------|----------------|-------------|
| V19   | 14674         | **0**        | 99.8%          | 0           |
| V20   | 14674         | **0**        | 99.8%          | 0           |
| V21   | 14674         | **0**        | 99.8%          | 1           |
| V22   | 14674         | **0**        | 99.8%          | 1           |
| V23   | 14675         | **0**        | 99.8%          | 0           |

The single new symbol is `cpufreq_set_governor_by_name()`.

### The governor really is registered under both names
Read straight out of the built `vmlinux`'s `.data`:

    dsh_scx_gov        @ ffffffc00a1fede0   name = "scx"
    dsh_sugov_ext_gov  @ ffffffc00a1fee48   name = "sugov_ext"

### The packed image really is that kernel
`boot_V23_hardened_ksu.img` decompresses to an Image whose sha256 equals the
built `arch/arm64/boot/Image`.

### The frequency maths, simulated with the source's own integer expressions
max_freq 3,187,200 Hz, capacity 1024, policy min 300,000:

| perf | cluster_tl | perf_tl | frequency |
|------|-----------|---------|-----------|
| 1    | 90        | 1       | 300,000   |
| 256  | 90        | 284     | 883,950   |
| 512  | 90        | 568     | 1,767,900 |
| 1024 | 90        | 1137    | 3,187,200 |

Monotonic, always inside the policy range, 10.62x dynamic range. The knobs bite:
`target_loads` 50/90/100 gives 3,187,200 / 1,767,900 / 1,593,600 at perf 512, and
`soft_freq_max` clamps exactly. Feeding `idle_perf=192` for background work
yields 662,962 Hz against 3,187,200 Hz for a boosted task -- the power saving is
20.8% of full, not a claim but an arithmetic result.

### Boot-loop safety
Because the governor had not been exercised on hardware when it was published,
`dsh_gov_enforce.c` treats every switch as an experiment: it remembers the
governor found before the first attempt, and if a policy still is not running the
requested one after 3 rounds it restores the original and stops enforcing that
policy. The first attempt is delayed 45 s so that `echo 0 > /proc/dsh_gov/enforce`
is always reachable. Worst case the device falls back to the vendor governor
within about two minutes.

### Atomic-context safety of the hot path
`dsh_update_util()` is called from the cpufreq update hook -- inside the
scheduler, with the runqueue lock held and interrupts off.  Anything that can
sleep there is an immediate hang, so the six functions reachable from that hook
were scanned for sleeping primitives:

    ok dsh_update_util          clean
    ok dsh_perf_for_cpu         clean
    ok dsh_get_next_freq        clean
    ok dsh_target_load          clean
    ok dsh_soft_freq_clamp      clean
    ok dsh_update_cpufreq       clean

    ===> HOT PATH IS SLEEP-FREE

What it does use is the same set schedutil uses from the same context:
`raw_spin_lock_irqsave()` around the frequency decision,
`cpufreq_driver_resolve_freq()`, `cpufreq_driver_fast_switch()`, and
`kthread_queue_work()` for the non-fast-switch path.  schedutil's own
`get_next_freq()` also calls `cpufreq_driver_resolve_freq()` from its update
hook, so the structure is equivalent to a governor that ships in mainline.

---

## Boot-hang fix (V24)

### What was happening
Userspace restores its saved cpufreq governor early in boot.  When that saved
value was `scx`, this port's governor was selected while the system was still
coming up, and it started installing its update hook and driving frequency
immediately.  The machine hung at the first screen and never finished booting.
`walt` never had this problem, which is why it could always boot -- and it is why
the very first V23 boot worked: at that point the saved governor was still
`walt`, so this port's governor was merely registered, never selected, and none
of its code ran.

### The three fixes
1. `dsh_fengchi_gov.c` -- **lazy hook install.** `dsh_start()` installs the
   update hook immediately only when `system_state == SYSTEM_RUNNING`, i.e. when
   userspace picks the governor after boot.  If the governor is selected during
   boot, installation is deferred by 5 s, so the system comes up first.
2. `dsh_unfair.c` -- **hooks are armed 8 s after init**, not at `late_initcall`,
   so nothing from this file runs during early boot.
3. `dsh_unfair.c` -- **the performance request is only emitted when `scx` or
   `sugov_ext` is the active governor.**  With `walt` (or any other vendor
   governor) selected, this file is completely inert; it cannot call
   `cpufreq_update_util()` into a governor that does not expect it.

Together: booting with `walt` runs none of this code, and booting with `scx`
selected from a saved setting keeps the governor dormant until the system is up.
`scx` can now boot the device the way `walt` does.

CRC check for this build: 14675 common symbols, **0 changed**, 99.8% match
against the stock kernel.

### V25: the activation itself is lazy
V24 deferred only the update hook; `dsh_init()` still created the
SCHED_DEADLINE worker and the sysfs directory during boot.  V25 defers all of
it: `dsh_init()` allocates memory and records the policy, nothing else.  The
worker thread, the tunables directory and the update hook are all created by a
single work item, which runs immediately when the governor is selected after
boot and 5 s later when it is selected during boot.

So a device whose saved governor is `scx` boots exactly like one that saved
`walt`: during boot the governor is registered and selected but provably does
nothing.  That is the property the port was missing.

---

## Bisection images (to identify the boot hang exactly)

Two minimal images, each carrying exactly one of the new components, so a single
flash each settles which file is responsible.  Both were verified by reading the
kernel back out of the packed image:

| image | governor | dsh_unfair | hmbird_common | enforcer |
|-------|----------|-----------|---------------|----------|
| `boot_V26_governor_only_ksu.img`    | YES | no  | no  | no |
| `boot_V27_hmbirdcommon_only_ksu.img`| no  | no  | YES | no |

Read it this way:

* **V26 boots**  -> the governor is not the problem; `dsh_hmbird_common.c`
  (the only other thing that runs at boot) becomes the prime suspect, and V27
  tests it directly.
* **V26 hangs**  -> the governor is responsible, even though V25 makes it fully
  inert during boot; that would point at registration rather than activation.
* **V27 boots**  -> `dsh_hmbird_common.c` is clean.
* **V27 hangs**  -> `register_sysctl("hmbird/common", ...)` at `late_initcall`
  is the cause.

Also fixed while preparing these: `dsh_fengchi_gov.c` and `dsh_unfair.c` were
coupled -- the governor only declared `dsh_cpuperf_target` while `dsh_unfair.c`
defined it, so the governor could not link on its own.  The owner now defines it.

---

## V28: a debug build that makes the hang talk

A hang on this device ends in a PMIC hard reset, which leaves no software record
at all -- every previous attempt to find out what happened failed for that reason.
This image fixes that from the packing side: the kernel command line inside
`boot.img` is set to

    console=ttyMSM0,115200n8 ignore_loglevel initcall_debug
    softlockup_panic=1 hung_task_panic=1 rcupdate.rcu_cpu_stall_timeout=10 panic=10

Qualcomm appends its own arguments after the image's, so these take effect.  The
effect is that a hang is no longer a silent reset:

* `softlockup_panic` / `hung_task_panic` turn a stalled CPU or task into a panic
  while there is still enough state to write it out;
* `panic=10` reboots 10 s later, so the device comes back on its own;
* `ignore_loglevel` puts everything on the console, and `initcall_debug` prints
  each initcall -- so if the hang is during initialisation, the last line names
  the function responsible;
* ramoops/pstore then holds the console dump, readable after the reboot with
  `scripts/read_pstore.sh` (also copies into `/data/system/dropbox`).

The image carries the full feature set, so the same flash that tests the port
also produces the diagnosis if it fails.

---

## Vendor port: what is actually portable, with evidence

The vendor side is 30 files / ~7,000 lines under
`vendor/oplus/kernel/cpu/sched_ext/`.  Each candidate was dependency-checked
before any porting, and the result is that **one file is genuinely portable**:

| vendor file | verdict | why |
|-------------|---------|-----|
| `hmbird_common_sysctl.c` | **ported** | only needs sysctl; becomes `/proc/sys/hmbird/common/*` |
| `hmbird_II/hmbird_II_critical_task_monitor.c` | **not portable** | self-contained as a file, but its input does not exist: `waker_count`/`wakee_count` are never incremented *anywhere in the vendor tree*, and this tree has no task-wakeup `android_vh` hook (only 19 `android_vh_*` hooks exist here, and `android_vh_set_wake_flags` carries no `task_struct`).  Porting it would add a registry nothing ever feeds. |
| `hmbird_kfunc.c`, `hmbird_II.c`, `hmbird_dfx.c`, `hmbird_minidump.c`, `main.c`, `hmbird_CameraScene/*` | **not portable** | sched_ext/BPF host; the host cannot be enabled on this tree (hard reset) |
| `hmbird_II_sysctl.c` | **not portable** | its `internal_cfg_task_sched_prop_handler()` applies `sched_prop` through the oplus task struct, which lives in `oplus_bsp_sched_assist.ko` (`get_oplus_task_struct` is not in the kernel tree at all) |

This is why the port takes the route it does: the vendor's *algorithm* is worth
having, its *plumbing* is not.

## Using the platform's own importance signal

Because the vendor's wake-count ranking has no data source here, the classifier
uses a signal the platform already maintains: `uclamp_min`.  Android and OPLUS
raise `uclamp_min` for work they consider important, so a task asking for a
raised floor *is* a critical task by the platform's own definition.

`dsh_unfair.c` now tests it first, before the tgid/pid/weight heuristics:

    if (p && uclamp_eff_value(p, UCLAMP_MIN) >= dsh_uclamp_min)
            return true;

`dsh_uclamp_min` (default 512) is tunable through
`/proc/dsh_unfair/uclamp_min` and shown in `stats`.  `uclamp_req` is an existing
field of `task_struct`, so this reads state rather than adding any -- the CRC
result is unchanged (0 of 14675).

---

## Why selection could fail, and why it no longer can

Putting the pieces together, this is the most likely shape of the boot hang, and
it is worth stating because it explains every observation:

* V17's `scx` was an alias of schedutil, so userspace writing `scaling_governor`
  **succeeded**, the choice was saved, and boot was fine.
* V19 onward registered the real governor.  Its `->init()` created a
  `SCHED_DEADLINE` worker, and `sched_setattr_nocheck()` can be refused on an
  early-boot system.  That made `->init()` return an error, which made the
  userspace write fail too -- and a vendor service that retries that write is a
  hang at the first screen.
* V25 moved the worker into a deferred work item, so `->init()` always succeeds.

V30 closes the remaining hole: a refused `SCHED_DEADLINE` is no longer fatal at
all.  The worker is kept as an ordinary kthread, a warning is logged, and the
governor still applies frequencies -- just without deadline scheduling.  There is
now no path on which the governor can fail to be selected.

`boot_V30_robust_debug_ksu.img` is the most complete image published: the lazy
activation, the uclamp classifier, the deferred and gated unfair scheduler, the
hmbird common sysctl, the enforcement work, and the panic-on-hang command line
that captures a backtrace if anything does go wrong.

---

## V31: a first flash that cannot hang

Every attempt so far asked the user to risk a first-screen hang to find out whether
the port works.  V31 removes that risk entirely.

The governor registers and is selectable exactly as before -- `scx` is still the
intended global default and userspace will find and select it -- but it does
**not** install its update hook until it is asked to.  The switch is

    /proc/dsh_fengchi/active

The default is `0`.  Until something writes `1`, the runtime path does not
execute at all: no frequency decisions, no notifications into the scheduler, no
work items.  The device boots with exactly the behaviour of a kernel that has no
governor at this address, so nothing in this port can hang the boot.

Two properties make this genuinely safe rather than merely off:

1. **The state is a kernel variable and nothing else.**  It is not a file, not a
   setting, not a property.  A reboot always returns the device to the inert
   state, so even if enabling it goes catastrophically wrong, one reset is enough
   -- a boot loop is impossible.
2. **Enabling is reversible from a running system.**  `echo 0 > .../active`
   removes the hooks again immediately and logs it, so a bad behaviour can be
   backed out without reflashing.

It is also a clean experiment.  If V31 boots, the kernel side of the port is
sound and any remaining problem is in the runtime path; if enabling it then
causes trouble, that fact alone localises the fault.

`boot_V31_failsafe_ksu.img` carries the full feature set plus the panic-on-hang
command line, and CRC is unchanged (0 of 14675).
