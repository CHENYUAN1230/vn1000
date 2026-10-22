#!/bin/bash
# test-tap.sh
#
# Repeatable tap-mode test: boots the nested VM with the VN1000 on tap0
# (loopback=off) and checks traffic both ways between the guest (10.1.0.1)
# and this host (10.1.0.2). The guest half is the vn1000.test=tap branch of
# /init (build-initramfs.sh); the two halves sync on "=== VNIC ..." lines on
# the serial console, and the host signals the guest over TCP port 9000.
#
#   ./test-tap.sh                       # needs a PCI build: make VNIC_BACKEND=pci,
#                                       # then ./build-initramfs.sh
#   MAC=52:54:00:aa:bb:cc ./test-tap.sh # expect (and set) a different MAC
#   TIMEOUT=120 ./test-tap.sh           # seconds to wait for each guest step
#
# Needs sudo (setup-tap.sh, ping -f). Exit status 0 only if every check passes.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
MAC="${MAC:-52:54:00:12:34:56}"
TIMEOUT="${TIMEOUT:-60}"
WORK="$(mktemp -d)"
LOG="$WORK/serial.log"
MON="$WORK/monitor.sock"
PASS=0
FAIL=0

for f in "$HERE/vmlinuz" "$HERE/initramfs.cpio.gz"; do
	[ -f "$f" ] || { echo "missing $f: see the header comments of run.sh / build-initramfs.sh" >&2; exit 2; }
done

result() {	# result <ok|fail> <name> <detail>
	if [ "$1" = ok ]; then
		PASS=$((PASS + 1)); printf '[PASS] %-7s %s\n' "$2" "$3"
	else
		FAIL=$((FAIL + 1)); printf '[FAIL] %-7s %s\n' "$2" "$3"
	fi
}

# wait_for <marker>: wait until the guest prints "=== VNIC <marker>"
wait_for() {
	local i
	for ((i = 0; i < TIMEOUT * 2; i++)); do
		grep -aq "^=== VNIC $1" "$LOG" && return 0
		sleep 0.5
	done
	return 1
}

# field <marker>: the text after "=== VNIC <marker> "
field() {
	grep -a "^=== VNIC $1" "$LOG" | head -1 | tr -d '\r' | cut -d' ' -f4-
}

finish() {
	python3 - "$MON" <<'EOF' 2>/dev/null
import socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.sendall(b"quit\n")
EOF
	wait "$QEMU_PID" 2>/dev/null
	echo "$PASS/$((PASS + FAIL)) passed. Full log: $LOG"
	[ "$FAIL" -eq 0 ]
	exit
}

abort() {	# abort <name> <what>: a guest step never showed up
	result fail "$1" "timed out waiting for the guest ($2)"
	finish
}

"$HERE/setup-tap.sh" >/dev/null || exit 2
sudo ip neigh flush dev tap0

TEST=tap "$HERE/run.sh" \
	-netdev tap,id=n0,ifname=tap0,script=no,downscript=no \
	-device vnic,loopback=off,netdev=n0,mac="$MAC" \
	-monitor unix:"$MON",server,nowait \
	</dev/null >"$LOG" 2>&1 &
QEMU_PID=$!

# guest: MAC, then A (L2 -> L1 ping) and B (same after a down/up)
wait_for MAC || abort MAC "boot / insmod"
got="$(field MAC)"
[ "$got" = "$MAC" ] && result ok MAC "vnic0 = $got" || result fail MAC "vnic0 = $got, expected $MAC"

wait_for "B " || abort B "guest pings"
[ "$(field 'A ')" = ok ] && result ok A "L2 -> L1 ping 0% loss" || result fail A "L2 -> L1 ping lost packets"
[ "$(field 'B ')" = ok ] && result ok B "L2 down/up -> ping 0% loss" || result fail B "L2 ping after down/up lost packets"

# host: C, D, NEIGH while the guest waits on port 9000
wait_for READY || abort C "READY"
ping -c 3 -i 0.3 -W 1 10.1.0.1 | grep -q " 0% packet loss" \
	&& result ok C "L1 -> L2 ping 0% loss" || result fail C "L1 -> L2 ping lost packets"
out="$(sudo ping -f -c 2000 -W 1 10.1.0.1 | grep transmitted)"
echo "$out" | grep -q " 0% packet loss" \
	&& result ok D "L1 -> L2 flood: $out" || result fail D "L1 -> L2 flood: $out"
ip neigh show dev tap0 10.1.0.1 | grep -qi "lladdr $MAC" \
	&& result ok NEIGH "L1 neighbor 10.1.0.1 = $MAC" \
	|| result fail NEIGH "L1 neighbor: $(ip neigh show dev tap0 10.1.0.1)"

for ((i = 0; i < 10; i++)); do	# tell the guest to go on; retry until it listens
	nc -N -w 1 10.1.0.1 9000 </dev/null 2>/dev/null && break
	sleep 0.5
done

# F: the guest is down for 10 s; flood it for 7 s so frames queue in QEMU,
# then check they all arrived after it came back up, with no RX drops
wait_for DOWN || abort F "DOWN"
read -r rx0 _ <<<"$(field DOWN)"
sent="$(sudo ping -f -w 7 10.1.0.1 | grep -o '^[0-9]* packets transmitted' | cut -d' ' -f1)"
wait_for UP || abort F "UP"
read -r rx1 drop <<<"$(field UP)"
delta=$((rx1 - rx0))
[ "$delta" -ge "${sent:-0}" ] && [ "$drop" -eq 0 ] \
	&& result ok F "backlog of $sent frames across down/up: $delta received, rx_dropped $drop" \
	|| result fail F "backlog of $sent frames: $delta received, rx_dropped $drop"

wait_for DONE || abort DONE "DONE"
finish
