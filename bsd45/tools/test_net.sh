#!/usr/bin/env bash
# test_net.sh - S402/S403 acceptance: VFS transparent forwarding, shared-page
#               bulk data loop, and user-space network stack (UDP/IP + loopback).
# test_net.sh - S402/S403 验收：VFS 透明转发、共享页大数据闭环、用户态网络栈。
set -uo pipefail
ws="$(cd "$(dirname "$0")/.." && pwd)"
iso="${1:-$ws/out/BSD45-ELF-x86.iso}"
[ -f "$iso" ] || { echo "ISO not found: $iso" >&2; exit 1; }
fails=0

check() {
    local name="$1" log="$2" n
    shift 2
    for n in "$@"; do
        if printf '%s' "$log" | grep -qF -- "$n"; then echo "  ok   $name: found $n"
        else echo "  FAIL $name: missing $n" >&2; fails=$((fails + 1)); fi
    done
}

run() {
    "$ws/tools/run_qemu_test.sh" -Iso "$iso" \
        -Keys "shell|$1|exit 0" -WaitFor "user exit" -TimeoutSec 90
}

echo "S402/S403 acceptance: transparent VFS + shared page + user-space net"

# VFS 透明转发 / transparent forwarding
log="$(run /bin/vfstest)"
check "vfs roundtrip" "$log" "vfs: transparent roundtrip ok"
check "vfs eio"       "$log" "vfs: fault isolation ok (EIO)"
check "vfs restart"   "$log" "vfs: restart ok"
check "vfs close fwd" "$log" "vfs: close forward ok (20 open/close, no slot leak)"
check "vfs verdict"   "$log" "vfs: PASS"

# 共享页闭环 / shared-page loop
log="$(run /bin/shmtest)"
check "shm bulk"   "$log" "shm: bulk 4KB verify ok"
check "shm noleak" "$log" "shm: re-alloc ok (no leak)"
check "shm verdict" "$log" "shm: PASS"

# 用户态网络栈 / user-space network stack
log="$(run /bin/nettest)"
check "net udp"     "$log" "net: udp loopback ok"
check "net eio"     "$log" "net: fault isolation ok (EIO)"
check "net restart" "$log" "net: restart ok"
check "net verdict" "$log" "net: PASS"

# 关键负向断言：内核绝不能 TRAP
if printf '%s' "$log" | grep -qiE 'kernel TRAP|panic|triple fault'; then
    echo "  FAIL kernel trapped" >&2
    fails=$((fails + 1))
else
    echo "  ok   no kernel trap"
fi

if [ "$fails" -ne 0 ]; then echo "S402/S403: FAILED ($fails)" >&2; exit 1; fi
echo "S402/S403: PASSED"
exit 0
