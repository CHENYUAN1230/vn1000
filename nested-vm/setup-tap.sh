#!/bin/bash
# setup-tap.sh [del]
#
# Creates tap0 on this host for run.sh's tap mode: owned by $USER, so QEMU can
# open it without root, with 10.1.0.2/24 as the host's end of the link. A tap
# made by "ip tuntap add" does not survive a reboot, so rerun this after one.
# Safe to rerun: it skips whatever already exists. Needs sudo.
#
#   ./setup-tap.sh        # create tap0
#   ./setup-tap.sh del    # remove it
#   ./run.sh -netdev tap,id=n0,ifname=tap0,script=no,downscript=no \
#            -device vnic,loopback=off,netdev=n0
#
# tap0 shows DOWN / NO-CARRIER until QEMU opens it; that is expected.
set -euo pipefail

TAP=tap0
ADDR=10.1.0.2/24

if [ "${1:-}" = "del" ]; then
	sudo ip link del "$TAP" 2>/dev/null && echo "removed $TAP" || echo "$TAP not present"
	exit 0
fi

if ! ip link show "$TAP" >/dev/null 2>&1; then
	sudo ip tuntap add dev "$TAP" mode tap user "$USER"
fi
if ! ip -4 addr show dev "$TAP" | grep -q "${ADDR%/*}"; then
	sudo ip addr add "$ADDR" dev "$TAP"
fi
sudo ip link set "$TAP" up

ip -br addr show "$TAP"
