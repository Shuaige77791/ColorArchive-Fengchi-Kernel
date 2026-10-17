#!/bin/bash
# Read whatever ramoops/pstore captured from the previous boot.
set -u
ADB=/mnt/c/Users/Administrator/Desktop/EEVDF-Checker/platform-tools/adb.exe
D=$("$ADB" devices 2>/dev/null | awk 'NR>1 && NF>=2 {print $1}' | head -1)
[ -z "$D" ] && { echo "NO DEVICE"; exit 0; }
R() { "$ADB" -s "$D" shell "su -c '$1'" 2>&1 | tr -d '\r'; }

echo "############ 1. what is running now ############"
"$ADB" -s "$D" shell 'uname -v; getprop ro.boot.slot_suffix; getprop sys.boot_completed' 2>&1 | tr -d '\r' | sed 's/^/  /'

echo
echo "############ 2. pstore contents ############"
R 'ls -la /sys/fs/pstore/ 2>&1'
R 'for f in /sys/fs/pstore/*; do echo "=== $f"; done 2>&1'

echo
echo "############ 3. the panic backtrace, if any ############"
R 'cat /sys/fs/pstore/console-ramoops-0 2>/dev/null | tail -100'
R 'cat /sys/fs/pstore/dmesg-ramoops-0 2>/dev/null | tail -80'

echo
echo "############ 4. dropbox copies (Android moves pstore here) ############"
R 'ls -lat /data/system/dropbox/ 2>/dev/null | head -20'
R 'for f in $(ls -t /data/system/dropbox/* 2>/dev/null | head -5); do echo "=== $f ==="; head -80 "$f"; done'

echo
echo "############ 5. misc tells ############"
R 'cat /proc/last_boot_reason 2>/dev/null'
R 'getprop | grep -i -E "bootreason|panic|reset" | head -12'

echo
echo "############ 6. did the debug cmdline apply? ############"
R 'cat /proc/cmdline | tr " " "\n" | grep -E "softlockup_panic|hung_task_panic|initcall_debug|ignore_loglevel|rcu_cpu_stall"'

echo
echo "############ 7. our own messages ############"
R 'dmesg | grep -i -E "dsh_gov|dsh_unfair|hmbird: /proc/sys" | head -30'
R 'dmesg | grep -i "initcall.*dsh" | head -20'
echo "############ DONE ############"