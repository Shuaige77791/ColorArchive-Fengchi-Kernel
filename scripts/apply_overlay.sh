#!/bin/bash
# Copy every file from overlay/ over the base tree, then record the result.
set -eu
BASE="$1"; OVL="$2"
[ -d "$BASE" ] || { echo "no base dir $BASE"; exit 1; }
n=0
cd "$OVL"
while IFS= read -r f; do
  mkdir -p "$BASE/$(dirname "$f")"
  cp -f "$f" "$BASE/$f"
  n=$((n+1))
done < <(find . -type f | sed 's|^\./||' | sort)
echo "applied $n files from overlay"
cd "$BASE"
# sanity: key markers must be present
for m in kernel/sched/ext.c kernel/sched/hmbird/hmbird.c kernel/dsh_eco.c; do
  [ -f "$m" ] && echo "  ok $m" || { echo "  MISSING $m"; exit 1; }
done
grep -q 'CONFIG_SCHED_CLASS_EXT' init/Kconfig && echo "  ok Kconfig" || echo "  warn Kconfig"
