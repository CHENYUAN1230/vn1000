# common.sh: helpers shared by test-*.sh and bench-*.sh. Source it, don't run it.
#
# Every script calls preflight() first, so each run starts from the same
# checked state whatever happened since the last one: a host reboot drops
# tap0, a kernel update needs a new vmlinuz, an edit to qemu-device/vnic.c
# needs a QEMU rebuild. pack() then rebuilds the initramfs around the module
# under test, so the VM never runs a stale vnic.ko.

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
QEMU_SRC="${QEMU_SRC:-$HOME/qemu}"
export QEMU_BIN="${QEMU_BIN:-$QEMU_SRC/build/qemu-system-x86_64}"   # run.sh reads it too
TIMEOUT="${TIMEOUT:-60}"
WORK="$(mktemp -d)"
LOG="$WORK/serial.log"
MON="$WORK/monitor.sock"
PASS=0
FAIL=0

die() { echo "$*" >&2; exit 2; }

# preflight [tap]: stop on anything a run would otherwise get silently wrong;
# with "tap", also (re)create tap0
preflight() {
	local q="$QEMU_SRC/hw/net"

	[ -f "$HERE/vmlinuz" ] || die "missing nested-vm/vmlinuz: see run.sh"
	sudo cmp -s "/boot/vmlinuz-$(uname -r)" "$HERE/vmlinuz" ||
		die "nested-vm/vmlinuz is not this host's kernel ($(uname -r)): recopy it, see run.sh"
	cmp -s "$REPO/qemu-device/vnic.c" "$q/vnic.c" && cmp -s "$REPO/include/vnic_regs.h" "$q/vnic_regs.h" ||
		die "$q differs from the repo: copy qemu-device/vnic.c and include/vnic_regs.h there, then ninja -C $QEMU_SRC/build"
	[ "$QEMU_BIN" -nt "$q/vnic.c" ] || die "$QEMU_BIN is older than $q/vnic.c: ninja -C $QEMU_SRC/build"
	if [ "${1:-}" = tap ]; then
		"$HERE/setup-tap.sh" >/dev/null || die "setup-tap.sh failed"
	fi
}

# is_pci [vnic.ko]: true if the module is the PCI backend build
is_pci() {
	modinfo -F alias "${1:-$REPO/driver/vnic.ko}" | grep -q 'v00001234d000011E9'
}

# pack [vnic.ko]: rebuild the initramfs around this module (default driver/vnic.ko)
pack() {
	local ko="${1:-$REPO/driver/vnic.ko}"

	[ -f "$ko" ] || die "missing $ko: build the driver first, see README"
	[ -x "$REPO/driver/raw_flood" ] || die "missing driver/raw_flood: make -C driver raw_flood"
	"$HERE/build-initramfs.sh" "$ko" >/dev/null || die "build-initramfs.sh failed"
}

# boot <test> [qemu args...]: start the VM in the background with /init's
# vn1000.test=<test> branch, serial console to $LOG
boot() {
	local t="$1"
	shift
	TEST="$t" "$HERE/run.sh" "$@" -monitor unix:"$MON",server,nowait </dev/null >"$LOG" 2>&1 &
	QEMU_PID=$!
	trap 'kill "$QEMU_PID" 2>/dev/null' EXIT
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

# field <marker>: the text after "=== VNIC <marker> " (first occurrence)
field() {
	grep -a "^=== VNIC $1" "$LOG" | head -1 | tr -d '\r' | cut -d' ' -f4-
}

result() {	# result <ok|fail|skip> <name> <detail>
	case "$1" in
	ok)   PASS=$((PASS + 1)); printf '[PASS] %-7s %s\n' "$2" "$3" ;;
	skip) printf '[SKIP] %-7s %s\n' "$2" "$3" ;;
	*)    FAIL=$((FAIL + 1)); printf '[FAIL] %-7s %s\n' "$2" "$3" ;;
	esac
}

quit_vm() {
	python3 - "$MON" <<'EOF' 2>/dev/null
import socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.sendall(b"quit\n")
EOF
	wait "$QEMU_PID" 2>/dev/null
}

finish() {
	quit_vm
	echo "$PASS/$((PASS + FAIL)) passed. Full log: $LOG"
	[ "$FAIL" -eq 0 ]
	exit
}

abort() {	# abort <name> <what>: a guest step never showed up
	result fail "$1" "timed out waiting for the guest ($2)"
	finish
}

# check_txdrop: the guest's "=== VNIC TXDROP" line, written by txdrop_check()
# in /init: three 1042 byte frames sent with MAXFRM lowered to 500 must count
# as TX drops only, and down/up must restore MAXFRM (1518 at MTU 1500)
check_txdrop() {
	local dt dr db mf pg
	read -r dt dr db mf pg <<<"$(field TXDROP)"
	if [ "$dt" = 3 ] && [ "$dr" = 0 ] && [ "$db" -lt 3126 ] && [ "$mf" = 1518 ] && [ "$pg" = ok ]; then
		result ok TXDROP "3 frames over MAXFRM: tx_dropped +$dt, rx_dropped +$dr, tx_bytes +$db; MAXFRM $mf after down/up, ping ok"
	else
		result fail TXDROP "tx_dropped +$dt (want 3), rx_dropped +$dr (want 0), tx_bytes +$db (want < 3126), MAXFRM $mf (want 1518), ping $pg"
	fi
}
