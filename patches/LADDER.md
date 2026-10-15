# Bisection ladder

All four images come from the SAME tree and the SAME toolchain.  The only
difference between ③ and ④ is whether `CONFIG_SCHED_CLASS_EXT` and
`CONFIG_HMBIRD_SCHED` are on, so that pair isolates the scheduler classes
exactly.

| # | image | contents | status |
|---|-------|----------|--------|
| ① | `boot_CTL3_orig_ksu.img` | pristine op10t sources + #50 config | **verified BOOTS on device** |
| ② | `boot_L1_eco_ksu.img` | ① + `kernel/dsh_eco.c` | **verified BOOTS on device** |
| ③ | `boot_T0f_fromV3tree_ksu.img` | all eleven fixes, both classes OFF | built, untested |
| ④ | `boot_V3_final_ksu.img` | all eleven fixes + seventeen hooks, both classes ON | built, untested |

① and ② booting proves the toolchain, the configuration, the boot-image packing,
the ramdisk and dsh_eco are all innocent; the fault is in the port.

## How to read the results

* ④ boots — done.
* ④ fails, ③ boots — the fault is inside the two scheduler classes.
* both fail — one of the eleven fixes is still wrong.

## sha256

```
de15410902711ba05d0d3477580b445007405959065a2bf0bf3abe9503f85e40  boot_T0f_fromV3tree_ksu.img
244df320a6d37ed2c59772387db3ec51371b2f40ebb2286d1297fba9af390c78  boot_T0f_fromV3tree_noksu.img
2b816ce6878f2a428f54a32a08da2b3ea1c55f0dd94b045a0607a20b4bca1cf4  boot_V3_final_ksu.img
e1fe000d22528923bddebad0a148b4620786089ef774399bce46e100d17a2fb2  boot_V3_final_noksu.img
```

## Not buildable

`boot_T1_ext_only_*.img` was attempted (sched_ext without hmbird) and does not
compile: `kernel/sched/hmbird.h` is not self-contained.  See BOOTFIXES.md.
