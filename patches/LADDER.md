# Bisection ladder (built with the same toolchain and config lineage as V3)

| image | contents | purpose |
|---|---|---|
| `boot_CTL3_orig_ksu.img` | pristine op10t sources + #50 config | **verified BOOTS on device** — proves toolchain, config, packing and ramdisk are innocent |
| `boot_L1_eco_ksu.img` | L0 + `kernel/dsh_eco.c` | **verified BOOTS on device** — dsh_eco is innocent too |
| `boot_T0f_noclass_ksu.img` | every fix in place, both scheduler classes OFF | isolates "are the fixes themselves sound?" — should boot |
| `boot_V3_ksu.img` | full fengchi + 11 fixes + 17 hooks | the actual target |
| T1 (ext only) | — | not buildable, see BOOTFIXES.md |

Flash order once the device is back:
1. V3 — if it boots, done.
2. If it does not, T0f — if that boots, the remaining fault is in the class code;
   if it does not, one of the eleven fixes is still wrong.
