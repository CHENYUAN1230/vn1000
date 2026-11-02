#!/bin/bash
# test-loopback.sh
#
# Repeatable loopback-mode test: Phase 3's suite (ping, payload sizes, a
# fragmented ping, MTU range, down/up, raw flood, rmmod while up, reload x5)
# plus BQL accounting, TX drop counting and a clean dmesg. The guest half is
# the vn1000.test=loopback branch of /init (build-initramfs.sh); it runs the
# whole suite on its own and prints one "=== VNIC ..." line per check.
# Checks the environment first and repacks the initramfs (common.sh).
#
#   ./test-loopback.sh            # tests whichever backend driver/vnic.ko was built for:
#                                 # PCI build: the QEMU device, loopback=on
#                                 # engine build: the in-kernel engine (TXDROP skipped)
#   KVM=1 ./test-loopback.sh      # same, under KVM
#   TIMEOUT=120 ./test-loopback.sh
#
# Needs sudo (preflight). Exit status 0 only if every check passes.
set -uo pipefail

source "$(dirname "$0")/common.sh"

preflight
pack
if is_pci; then
	echo "testing the PCI backend (QEMU device, loopback=on)"
	boot loopback -device vnic
else
	echo "testing the engine backend"
	boot loopback
fi

okline() {	# okline <marker> <what>: the guest printed "ok" for this check
	wait_for "$1 " || abort "$1" "$1"
	local got
	got="$(field "$1 ")"
	[ "$got" = ok ] && result ok "$1" "$2" || result fail "$1" "$2: $got"
}

okline PING   "ping 0% loss"
okline SIZES  "ping -s 0 1 64 1400 1472 1473, 0% loss each"
okline FRAG   "ping -s 65000 (fragmented) 0% loss"
okline MTU    "mtu 67 and 9001 rejected, 9000 refused while up, accepted while down"
okline DOWNUP "ping after down/up 0% loss"

wait_for "FLOOD " || abort FLOOD "FLOOD"
read -r rc tx rx <<<"$(field 'FLOOD ')"
[ "$rc" = 0 ] && [ "$tx" -ge 5000 ] && [ "$tx" = "$rx" ] \
	&& result ok FLOOD "raw_flood 5000: tx +$tx, rx +$rx" \
	|| result fail FLOOD "raw_flood rc=$rc: tx +$tx, rx +$rx (want rc 0, tx >= 5000, rx == tx)"

wait_for "BQL " || abort BQL "BQL"
got="$(field 'BQL ')"
[ "$got" = 0 ] && result ok BQL "inflight 0 once traffic stops" || result fail BQL "inflight $got, want 0"

wait_for "TXDROP " || abort TXDROP "TXDROP"
if [ "$(field 'TXDROP ')" = skip ]; then
	result skip TXDROP "engine backend: no BAR0 to lower MAXFRM through"
else
	check_txdrop
fi

wait_for "RMMOD " || abort RMMOD "RMMOD"
got="$(field 'RMMOD ')"
[ "$got" = 0 ] && result ok RMMOD "rmmod while up, vnic0 gone" || result fail RMMOD "rmmod: $got"

okline RELOAD "insmod, up, ping, down, rmmod x5"

wait_for "DMESG " || abort DMESG "DMESG"
got="$(field 'DMESG ')"
[ "$got" = 0 ] && result ok DMESG "no WARNING/BUG/Oops/refcount/underflow" || result fail DMESG "$got matching lines"

wait_for DONE || abort DONE "DONE"
finish
