#!/bin/bash
# Produce the kernel .config for the build.
#
# Preferred path: copy config/fengchi_defconfig verbatim -- it is the exact
# config that produced a working kernel locally (overhang 84 bytes).
# Fallback: build up from whatever defconfig the base ships + the fragment.
set -eu
BASE="$1"; FRAG="$2"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$BASE"

FULL="$ROOT/config/fengchi_defconfig"
if [ -f "$FULL" ]; then
  echo "using full defconfig: $FULL"
  cp -f "$FULL" .config
  make ARCH=arm64 SRCARCH=arm64 LLVM=1 LLVM_IAS=1 CROSS_COMPILE=aarch64-linux-gnu- \
       HOSTCC=gcc HOSTCXX=g++ olddefconfig >/dev/null 2>&1 || true
else
  echo "no full defconfig; bootstrapping from base defconfig + fragment"
  DEF=$(ls arch/arm64/configs/*defconfig 2>/dev/null | head -1)
  if [ -n "$DEF" ]; then
    make ARCH=arm64 SRCARCH=arm64 LLVM=1 LLVM_IAS=1 CROSS_COMPILE=aarch64-linux-gnu- \
         HOSTCC=gcc HOSTCXX=g++ "$(basename "$DEF")" >/dev/null 2>&1 || true
  fi
  # the fragment (path may be absolute)
  FR="$FRAG"; [ -f "$FR" ] || FR="$ROOT/$FRAG"
  if [ -f "$FR" ]; then
    while IFS= read -r line; do
      case "$line" in ''|\#*) continue ;; esac
      sym="${line%%=*}"; val="${line#*=}"
      if [ "$val" = "n" ]; then
        ./scripts/config --disable "$sym"
      else
        ./scripts/config --enable "$sym" 2>/dev/null || \
          ./scripts/config --set-val "$sym" "$val" 2>/dev/null || true
      fi
    done < "$FR"
  fi
  ./scripts/config --disable DEBUG_INFO          || true
  ./scripts/config --disable BOEFFLA_WL_BLOCKER  || true
  ./scripts/config --enable  SCHED_CLASS_EXT     || true
  ./scripts/config --enable  HMBIRD_SCHED        || true
  make ARCH=arm64 SRCARCH=arm64 LLVM=1 LLVM_IAS=1 CROSS_COMPILE=aarch64-linux-gnu- \
       HOSTCC=gcc HOSTCXX=g++ olddefconfig >/dev/null 2>&1 || true
fi

echo "--- key config ---"
grep -E 'DEBUG_INFO|SCHED_CLASS_EXT|HMBIRD_SCHED|BOEFFLA_WL_BLOCKER|MODVERSIONS' .config || true
if grep -q '^CONFIG_DEBUG_INFO=y' .config; then
  echo "FATAL: CONFIG_DEBUG_INFO=y -- the Image layout will break"; exit 1
fi
if ! grep -q '^CONFIG_SCHED_CLASS_EXT=y' .config; then
  echo "FATAL: CONFIG_SCHED_CLASS_EXT is not enabled"; exit 1
fi
if ! grep -q '^CONFIG_HMBIRD_SCHED=y' .config; then
  echo "FATAL: CONFIG_HMBIRD_SCHED is not enabled"; exit 1
fi
echo "config OK"
