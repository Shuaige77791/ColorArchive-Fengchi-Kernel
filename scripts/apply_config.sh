#!/bin/bash
set -eu
BASE="$1"; FRAG="$2"
cd "$BASE"
# start from the arch defconfig that the base repo ships
DEF=$(ls arch/arm64/configs/*defconfig 2>/dev/null | head -1)
echo "defconfig: $DEF"
if [ -n "$DEF" ]; then
  make ARCH=arm64 SRCARCH=arm64 LLVM=1 LLVM_IAS=1 \
       CROSS_COMPILE=aarch64-linux-gnu- HOSTCC=gcc HOSTCXX=g++ \
       "$(basename "$DEF")" >/dev/null 2>&1 || true
fi
if [ -f .config ]; then
  while IFS= read -r line; do
    case "$line" in
      ''|\#*) continue ;;
    esac
    sym="${line%%=*}"
    val="${line#*=}"
    if [ "$val" = "n" ]; then
      ./scripts/config --disable "$sym"
    else
      ./scripts/config --enable "$sym" 2>/dev/null || \
        ./scripts/config --set-val "$sym" "$val" 2>/dev/null || true
    fi
  done < "../$FRAG" 2>/dev/null || while IFS= read -r line; do
    case "$line" in ''|\#*) continue ;; esac
    sym="${line%%=*}"; val="${line#*=}"
    if [ "$val" = "n" ]; then ./scripts/config --disable "$sym"; else ./scripts/config --enable "$sym"; fi
  done < "$FRAG"
else
  echo "no .config produced; relying on default"
fi
# the critical one
./scripts/config --disable DEBUG_INFO
./scripts/config --disable BOEFFLA_WL_BLOCKER 2>/dev/null || true
./scripts/config --enable SCHED_CLASS_EXT
./scripts/config --enable HMBIRD_SCHED
make ARCH=arm64 SRCARCH=arm64 LLVM=1 LLVM_IAS=1 CROSS_COMPILE=aarch64-linux-gnu- \
     HOSTCC=gcc HOSTCXX=g++ olddefconfig >/dev/null 2>&1 || true
echo "--- key config ---"
grep -E 'DEBUG_INFO|SCHED_CLASS_EXT|HMBIRD_SCHED|BOEFFLA_WL_BLOCKER' .config || true
