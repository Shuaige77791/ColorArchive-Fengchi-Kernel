# Boot fixes applied after the first round of bisection

Bisection on device (OnePlus 10T / Ace Pro, PGP110, 5.10.236-ColorArchive):

| build | contents                                        | result |
|-------|-------------------------------------------------|--------|
| L0    | pristine op10t sources + #50 config + my toolchain | BOOTS  |
| L1    | L0 + kernel/dsh_eco.c                            | BOOTS  |
| L2    | L0 + the full fengchi port                       | FAILS  |

So the environment, the toolchain, the config, the packing and dsh_eco are all
exonerated.  The failure is in the port itself.  Eight defects were found and fixed:

1. **`sched_init()` contiguity `BUG_ON` (the prime suspect).**
   `kernel/sched/core.c` asserts that the original five classes are adjacent and
   in order:
       BUG_ON(&idle_sched_class + 1 != &fair_sched_class ||
              &fair_sched_class + 1 != &rt_sched_class || ...);
   The port put `ext_sched_class` and `hmbird_sched_class` between fair and rt in
   `SCHED_DATA`, so `&fair_sched_class + 1` is `ext_sched_class`, the assertion
   fires, and the kernel panics inside `sched_init()` -- before the console
   exists, hence no log anywhere, hence a deterministic ~2 s reset.  The
   assertions now cover all seven classes.

2. **`pick_task` inserted in the middle of `struct sched_class`.**
   Nothing in 5.10 calls `sched_class->pick_task`; the member only existed so that
   `ext.c`'s initialiser would compile.  Inserting it mid-struct shifted every
   later function pointer by 8 bytes, so prebuilt vendor modules that index
   `struct sched_class` by offset (the Qualcomm WALT scheduler among them) read
   the wrong pointer.  Removed; the struct is back to the stock 24 pointers.

3. **`init_sched_ext_class()` / `init_sched_hmbird_class()` were never called.**
   `init_sched_hmbird_class()` is what allocates `init_task`'s `hmbird_entity`:
       init_hmbird = kzalloc(sizeof(struct hmbird_entity), GFP_KERNEL);
       init_task.android_oem_data1[HMBIRD_TS_IDX] = (u64)init_hmbird;
   With the function never invoked, `get_hmbird_ts()` returned NULL for every
   task forever.  Both calls are now in `sched_init()`, right after
   `init_sched_fair_class()` where 6.x puts them.

4. **Three `hmbird` accessors had no NULL check.**
   `task_is_top_task()`, `get_top_task_prop()` and `reset_top_task_prop()`
   dereferenced `get_hmbird_ts(p)` directly while the seven accessors next to them
   checked for NULL.  All three are NULL-safe now.

5. **`.config` had both classes off.**
   Left over from an isolation experiment; with them off every
   `#ifdef CONFIG_HMBIRD_SCHED` block compiles away, so a "successful" build
   proves nothing.  Both are `y` again.

6. **`kernel/fork.c` had duplicated labels.**
   `bad_fork_cleanup_perf` and `bad_fork_cleanup_policy` were defined twice and
   `WARN_ON(tsk == current)` / `io_uring_free(tsk)` appeared twice in
   `free_task()`.  The diff against the pristine baseline is now exactly three
   hmbird insertions.

7. **`kernel/sched/idle.c` was structurally broken** by the insertion (duplicated
   `put_prev_task_idle`, the class table outside any function).  Restored from
   the pristine baseline.  The two `hmbird_update_idle()` calls it wanted are
   dropped on purpose: the idle class never runs on a runqueue hmbird owns.

8. **`for_balance_class_range` was defined twice** (`ext.h` and `hmbird.h`).
   Guarded with `#ifndef`.

## Deliberately not ported

Some `.rej` hunks cannot exist on 5.10 and are skipped rather than faked:

* `core.c`: `if (p->sched_class == &hmbird_sched_class)` and
  `return hmbird_prio_less(a, b, in_fi);` need `__task_prio()`, `prio_less()`,
  `cfs_prio_less()` and `sched_core_free()`.  None of those exist here --
  core-sched (`CONFIG_SCHED_CORE`) landed in 5.14.  `hmbird_prio_less()` is
  neither defined nor declared.  The HMBIRD class does not need them while
  `CONFIG_SCHED_CORE` is off.

* `hmbird.h` / `ext.h`'s `for_each_active_class` and `for_balance_class_range`
  assume 6.x's **ascending** class layout (`class++`).  This tree walks
  **descending**:
      #define for_class_range(class, _from, _to) \
              for (class = (_from); class != (_to); class--)
  so those macros would walk the classes the wrong way.  Use `for_each_class`.

## Verified build

    Image 39,583,828 bytes, build EXIT=0, zero errors
    all seven class symbols within [__begin_sched_classes, __end_sched_classes)
    every adjacent gap exactly 0xe0  -> the sched_init() assertions hold
    init_sched_ext_class, init_sched_hmbird_class, dsh_eco_active all present

---

# Second pass (found after the first build)

9. **`WF_EXEC` / `WF_TTWU` / `WF_SHIM` did not exist in 5.10, and the values chosen
   for them aliased live flags.**  `kernel/sched/ext.h` and `kernel/sched/hmbird.h`
   defined `WF_EXEC = 0x02`, `WF_TTWU = 0x08`, `WF_FORK = 0x04` and
   `WF_SYNC = 0x10`, while 5.10 already has `WF_SYNC = 0x01`, `WF_FORK = 0x02`,
   `WF_MIGRATED = 0x04`, `WF_ON_CPU = 0x08`.  Every wakeup reason the scheduler
   forwarded to BPF was therefore wrong.  They now sit on bits 5.10 leaves free
   (0x20 / 0x40 / 0x80), and the incorrect `WF_FORK` / `WF_SYNC` redefinitions
   were dropped.

10. **None of the seventeen hmbird hooks had ever landed.**  The `.rej` files were
    present but the code was absent, so the hmbird class was registered and then
    never reached: nothing called its enqueue, dequeue, pick or preemption path.
    All of these are now wired:

    | hook | site |
    |------|------|
    | `hmbird_pre_fork()` / `hmbird_post_fork()` | `sched_fork()` |
    | `hmbird_cancel_fork()` | `sched_fork()` failure path, via `hmbird_fork_cancel:` |
    | `hmbird_free()` | `free_task()` |
    | `task_on_hmbird()` branch | `sched_fork()` class choice |
    | `task_on_hmbird()` branch | `__setscheduler_prio()` |
    | `hmbird_check_setscheduler()` | `sched_setscheduler()` and the fork reset path |
    | `case SCHED_HMBIRD:` | `sched_get_priority_{min,max}()` |
    | `hmbird_can_stop_tick()` | `sched_can_stop_tick()` |
    | `hmbird_preempt_wanted()` | `check_preempt_curr()` |
    | `hmbird_notify_pick_next_task()` | `pick_next_task()`, skipping the class while disabled |
    | `hmbird_notify_sched_tick()` | `scheduler_tick()` |
    | `init_sched_ext_class()` | `sched_init()` |
    | `init_sched_hmbird_class()` | `sched_init()` |

11. **`init_sched_ext_class()` BUG_ONs on six allocations and runs before the
    console.**  Any of them failing panics with no log at all -- the exact
    observed symptom.  They are now `WARN_ON`, and the `rq->scx` allocation failure
    path `continue`s instead of dereferencing NULL.

---

# Second pass (found after the first build)

9. **`WF_EXEC` / `WF_TTWU` / `WF_SHIM` did not exist in 5.10, and the values chosen
   for them aliased live flags.**  `kernel/sched/ext.h` and `kernel/sched/hmbird.h`
   defined `WF_EXEC = 0x02`, `WF_TTWU = 0x08`, `WF_FORK = 0x04` and
   `WF_SYNC = 0x10`, while 5.10 already has `WF_SYNC = 0x01`, `WF_FORK = 0x02`,
   `WF_MIGRATED = 0x04`, `WF_ON_CPU = 0x08`.  Every wakeup reason the scheduler
   forwarded to BPF was therefore wrong.  They now sit on bits 5.10 leaves free
   (0x20 / 0x40 / 0x80), and the incorrect `WF_FORK` / `WF_SYNC` redefinitions
   were dropped.

10. **None of the seventeen hmbird hooks had ever landed.**  The `.rej` files were
    present but the code was absent, so the hmbird class was registered and then
    never reached: nothing called its enqueue, dequeue, pick or preemption path.
    All of these are now wired:

    | hook | site |
    |------|------|
    | `hmbird_pre_fork()` / `hmbird_post_fork()` | `sched_fork()` |
    | `hmbird_cancel_fork()` | `sched_fork()` failure path, via `hmbird_fork_cancel:` |
    | `hmbird_free()` | `free_task()` |
    | `task_on_hmbird()` branch | `sched_fork()` class choice |
    | `task_on_hmbird()` branch | `__setscheduler_prio()` |
    | `hmbird_check_setscheduler()` | `sched_setscheduler()` and the fork reset path |
    | `case SCHED_HMBIRD:` | `sched_get_priority_{min,max}()` |
    | `hmbird_can_stop_tick()` | `sched_can_stop_tick()` |
    | `hmbird_preempt_wanted()` | `check_preempt_curr()` |
    | `hmbird_notify_pick_next_task()` | `pick_next_task()`, skipping the class while disabled |
    | `hmbird_notify_sched_tick()` | `scheduler_tick()` |
    | `init_sched_ext_class()` | `sched_init()` |
    | `init_sched_hmbird_class()` | `sched_init()` |

11. **`init_sched_ext_class()` BUG_ONs on six allocations and runs before the
    console.**  Any of them failing panics with no log at all -- the exact
    observed symptom.  They are now `WARN_ON`, and the `rq->scx` allocation failure
    path `continue`s instead of dereferencing NULL.

---

## Known structural limitation: hmbird.h is not self-contained

`kernel/sched/hmbird.h` (the in-tree one, distinct from the public
`include/linux/sched/hmbird.h`) uses three things it never declares:

* `struct hmbird_dispatch_q` — declared in `include/linux/sched/hmbird.h:107`
* `get_hmbird_rq()`            — a macro in `include/linux/sched/hmbird.h:20`
* `hmbird_enabled()`           — declared in the public header

It compiles today only because `kernel/sched/ext.h` happens to be included
first (from `build_policy.c:152`), which pulls the public header in ahead of it.
Consequences:

| configuration | builds? |
|---|---|
| `SCHED_CLASS_EXT=y` + `HMBIRD_SCHED=y` (shipped) | yes |
| `SCHED_CLASS_EXT=y` alone | no — `ext.c` uses `SCHED_CHANGE_BLOCK()`, which lives in `hmbird.h` |

The proper fix is to move `SCHED_CHANGE_BLOCK()` and the
`hmbird_sched_change_guard_*` helpers into a header both classes include, and to
give `kernel/sched/hmbird.h` the declarations it is missing.  Not done here: the
shipped kernel enables both classes together, and reordering includes on the
scheduler-core path risks the boot-critical `sched_init()` path for no benefit on
this device.
