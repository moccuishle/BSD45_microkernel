#!/usr/bin/env bash
#
# Copyright (c) 2026 jialiang.  All rights reserved.
#

# test_kthread.sh - S21 acceptance: kernel threads and softirq carriers.
set -uo pipefail
ws="$(cd "$(dirname "$0")/.." && pwd)"
iso="${1:-$ws/out/BSD45-ELF-x86.iso}"
[ -f "$iso" ] || { echo "ISO not found: $iso" >&2; exit 1; }
fails=0

check() {   # check <name> <log> <needle...>
    local name="$1" log="$2" n
    shift 2
    for n in "$@"; do
        if printf '%s' "$log" | grep -qF -- "$n"; then
            echo "  ok   $name: found $n"
        else
            echo "  FAIL $name: missing $n" >&2
            fails=$((fails + 1))
        fi
    done
}

echo "S21 acceptance: kernel threads + softirq"
log="$("$ws/tools/run_qemu_test.sh" -Iso "$iso" \
    -Keys "kthread|softirq|kthreadtest|softirqtest|kthread" \
    -WaitFor "softirqtest: PASS" -TimeoutSec 60)"

check "stacks"      "$log" "thread slots at fc000000" "16 KB stacks"
check "soft thread" "$log" "kthread: spawn softirq"
check "thread test" "$log" "kthreadtest: counter 3 PASS"
check "softirq test" "$log" "softirqtest: count 1 PASS"
check "switches"    "$log" "kthread: switches"
check "reclaim handler" "$log" "softirq: 0 runs"

printf '%s' "$log" | grep -q 'TRAP' && { echo "  FAIL kernel trap" >&2; fails=$((fails+1)); }

if [ "$fails" -ne 0 ]; then echo "S21 kthread: FAILED ($fails check(s))" >&2; exit 1; fi
echo "S21 kthread: PASSED"
exit 0
