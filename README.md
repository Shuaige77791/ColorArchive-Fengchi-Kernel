# ColorArchive Fengchi Kernel

OPLUS **风驰** game kernel ported to **OnePlus 10T / Ace Pro (PGP110, SM8475)**,
Linux **5.10.236-ColorArchive**.

风驰 = **sched_ext (SCX)** + OPLUS **HMBIRD** scheduling class.

## What is here

| path | content |
|---|---|
| `overlay/` | every source file that differs from the base kernel (22 new + 7 modified) |
| `patches/` | change notes, file list, config fragment |
| `scripts/` | apply / build / verify / pack helpers |
| `.github/workflows/build.yml` | cloud build + release |
| `boot_FENGCHI_*.img` | prebuilt images (also as Release assets) |

## Base kernel

```
OnePlusOSS/android_kernel_common_oneplus_sm8475
  branch: oneplus/sm8475_b_16.0.0_ace_2
```

The workflow takes `base_repo` / `base_branch` as inputs so you can retarget it.

## ★ Critical build requirement

**`CONFIG_DEBUG_INFO` must be OFF.**

With it on, `.eh_frame` grows and ~267 KB lands past `_end`; the Image layout no
longer matches what the bootloader expects and the phone **reboots before the
first printk** (the "logo flashes once then reboots" symptom).

Verify any build with:

```bash
python3 scripts/verify_image.py arch/arm64/boot/Image
# overhang must be tiny (~84 bytes), not hundreds of KB
```

| build | overhang | boots |
|---|---|---|
| working reference (#50) | 84 B | ✅ |
| broken (DEBUG_INFO=y) | 267,652 B | ❌ |
| this repo | 84 B | ✅ |

## Build locally

```bash
git clone --depth 1 -b oneplus/sm8475_b_16.0.0_ace_2 \
    https://github.com/OnePlusOSS/android_kernel_common_oneplus_sm8475.git base
bash scripts/apply_overlay.sh base overlay
bash scripts/apply_config.sh base patches/fengchi.config
cd base
make ARCH=arm64 SRCARCH=arm64 LLVM=1 LLVM_IAS=1 \
     CROSS_COMPILE=aarch64-linux-gnu- HOSTCC=gcc HOSTCXX=g++ \
     KCFLAGS=-Wno-error -j$(nproc) Image
python3 ../scripts/verify_image.py arch/arm64/boot/Image
```

## Flash

```bash
adb reboot bootloader
fastboot flash boot_a boot_FENGCHI_ksu.img     # with KernelSU
fastboot reboot
```

## My changes

### New files
- `kernel/sched/ext.c` — sched_ext core (from OnePlusOSS sm8750 GPL tree)
- `kernel/sched/hmbird/*` — HMBIRD scheduling class (14 files, from the leaked
  `fengchi_oneplus_ace_6.patch`)
- `kernel/sched/build_policy.c` — wraps `ext.c` the way 6.6 does
- `kernel/sched/hmbird_compat.h` — 6.x → 5.10 shims
- `kernel/dsh_eco.c` — information-flow power saver
- `include/linux/sched/{ext,hmbird,posix-timers}.h`, `include/linux/panic_notifier.h`

### Modified
- `include/asm-generic/vmlinux.lds.h` — register `__ext_sched_class` /
  `__hmbird_sched_class` in `SCHED_DATA` (else they are orphan sections and
  `for_each_class` never sees them)
- `include/linux/sched.h`, `kernel/sched/sched.h` — pull in headers, add KABI slots
  (`ANDROID_KABI_USE`) so `__crc_*` stays identical and vendor modules still load
- `kernel/sched/core.c` — `hmbird_sched_change_guard_*` wrappers
- `kernel/sched/Makefile`, `init/Kconfig` — build the new objects / add Kconfig

## Safety notes

- Every struct-layout change goes through `ANDROID_KABI_*` so the modversions CRC
  is unchanged (462 prebuilt vendor modules keep loading).
- No root is bundled; `*_noksu.img` has a stock ramdisk, `*_ksu.img` ships KernelSU
  in the ramdisk only (the Image itself contains no KernelSU code).
