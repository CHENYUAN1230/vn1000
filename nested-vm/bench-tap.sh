#!/bin/bash
# bench-tap.sh
#
# Throughput benchmark over tap0: boots the nested VM (guest half: the
# vn1000.test=bench branch of /init), runs a fixed list of iperf3 tests from
# this host against the guest, REPS times each, and prints every run and the
# median. Checks the environment first and repacks the initramfs (common.sh).
# Not a pass/fail test.
#
#   KVM=1 ./bench-tap.sh                   # working tree's driver/vnic.ko (PCI build)
#   KVM=1 REV=07ab85f ./bench-tap.sh       # the driver at that commit, built in a temp dir
#   KVM=1 MTU=9000 ./bench-tap.sh          # guest and tap0 at MTU 9000 (tap0 back to 1500 after)
#   KVM=1 TESTS="tcp-tx tcp-rx" REPS=3 ./bench-tap.sh
#
# Tests (TESTS, default all), each REPS runs of DUR seconds, PAUSE seconds apart:
#   tcp-tx      guest sends TCP              Mbit/s, retransmits
#   tcp-rx      host sends TCP               Mbit/s, retransmits
#   udp-tx      guest sends 64 B UDP, as fast as it can, 1 stream     pkt/s, loss
#   udp-tx-p4   the same with 4 streams
#   udp-rx      host sends 64 B UDP at each rate in RATES (pkt/s): loss per
#               rate, and the highest rate whose median loss is under 1%; also
#               where the lost datagrams went: the host's tap0 queue (QEMU not
#               reading it), vnic0's rx_dropped (driver/device), or the guest
#               socket's full receive buffer (UDP RcvbufErrors)
# Every run also reports the guest's interrupt rate and average data frame size.
#
# REV swaps the driver only; the QEMU device is always the current build.
# Needs sudo (setup-tap.sh, tap0 MTU).
set -uo pipefail

source "$(dirname "$0")/common.sh"
REPS="${REPS:-5}"
DUR="${DUR:-10}"
PAUSE="${PAUSE:-2}"
MTU="${MTU:-1500}"
TESTS="${TESTS:-tcp-tx tcp-rx udp-tx udp-tx-p4 udp-rx}"
RATES="${RATES:-1000 2000 5000 10000 20000 50000 100000}"
GUEST=10.1.0.1

preflight tap
if [ -n "${REV:-}" ]; then
	git -C "$REPO" archive "$REV" driver include | tar -x -C "$WORK" || die "git archive $REV failed"
	make -C "$WORK/driver" VNIC_BACKEND=pci >"$WORK/build.log" 2>&1 || die "building $REV failed, see $WORK/build.log"
	KO="$WORK/driver/vnic.ko"
	WHAT="driver at $(git -C "$REPO" rev-parse --short "$REV")"
else
	KO="$REPO/driver/vnic.ko"
	WHAT="working tree driver ($(git -C "$REPO" rev-parse --short HEAD)$(git -C "$REPO" diff --quiet HEAD -- driver include || echo ' + changes'))"
fi
is_pci "$KO" || die "$KO is not a PCI build: make -C driver VNIC_BACKEND=pci"
pack "$KO"

sudo ip link set tap0 mtu "$MTU"
APPEND="vn1000.mtu=$MTU" boot bench \
	-netdev tap,id=n0,ifname=tap0,script=no,downscript=no \
	-device vnic,loopback=off,netdev=n0
trap 'kill "$QEMU_PID" 2>/dev/null; sudo ip link set tap0 mtu 1500' EXIT
wait_for READY || die "guest never got ready, see $LOG"
[ "$(field READY)" = "$MTU" ] || die "guest MTU is $(field READY), wanted $MTU"

echo "$WHAT, $([ "${KVM:-0}" = 1 ] && echo KVM || echo TCG), MTU $MTU, $REPS x ${DUR}s"

# snap: one guest snapshot, "IRQs rx_bytes rx_pkts tx_bytes tx_pkts rx_dropped rcvbuf_errors"
snap() {
	local n i
	n=$(grep -ac '^=== VNIC SNAP' "$LOG")
	for ((i = 0; i < 20; i++)); do	# retry until the guest's nc listens again
		nc -N -w 1 "$GUEST" 9000 </dev/null 2>/dev/null && break
		sleep 0.2
	done
	for ((i = 0; i < 50; i++)); do
		[ "$(grep -ac '^=== VNIC SNAP' "$LOG")" -gt "$n" ] && break
		sleep 0.1
	done
	grep -a '^=== VNIC SNAP' "$LOG" | tail -1 | tr -d '\r' | cut -d' ' -f4-
}

# ip3 <iperf3 args>: one run; prints "Mbit/s retransmits rx_pkt/s loss%"
# (receiver side; "-" where a field does not apply)
ip3() {
	iperf3 -c "$GUEST" -t "$DUR" -J "$@" >"$WORK/ip3.json" 2>/dev/null
	python3 - "$WORK/ip3.json" <<'EOF'
import json, sys
try:
    e = json.load(open(sys.argv[1]))["end"]; r = e["sum_received"]
except Exception:
    print("err - - -"); sys.exit()
retr = e.get("sum_sent", {}).get("retransmits", "-")
if "packets" in r:
    print("%.1f - %.0f %.2f" % (r["bits_per_second"] / 1e6,
          (r["packets"] - r["lost_packets"]) / r["seconds"], r["lost_percent"]))
else:
    print("%.0f %s - -" % (r["bits_per_second"] / 1e6, retr))
EOF
}

median() {
	printf '%s\n' "$@" | sort -g | awk '{ a[NR] = $1 } END { print (NR % 2) ? a[(NR + 1) / 2] : (a[NR / 2] + a[NR / 2 + 1]) / 2 }'
}

TAPDROP=/sys/class/net/tap0/statistics/tx_dropped   # host -> guest frames tap0 had to drop

# measure <tx|rx> <iperf3 args>: REPS runs; fills MB RT PPS LOSS IRQ FR, and
# the drop counters DTAP DVNIC DSOCK (FR: average frame size in the data
# direction, tx = guest sends)
measure() {
	local dir=$1 i irq0 rb0 rp0 tb0 tp0 vd0 sd0 irq1 rb1 rp1 tb1 tp1 vd1 sd1 td0 td1 mb rt pps loss
	shift
	MB=() RT=() PPS=() LOSS=() IRQ=() FR=() DTAP=() DVNIC=() DSOCK=()
	for ((i = 0; i < REPS; i++)); do
		sleep "$PAUSE"
		read -r irq0 rb0 rp0 tb0 tp0 vd0 sd0 <<<"$(snap)"
		td0=$(cat "$TAPDROP")
		read -r mb rt pps loss <<<"$(ip3 "$@")"
		td1=$(cat "$TAPDROP")
		read -r irq1 rb1 rp1 tb1 tp1 vd1 sd1 <<<"$(snap)"
		MB+=("$mb") RT+=("$rt") PPS+=("$pps") LOSS+=("$loss")
		IRQ+=($(((irq1 - irq0) / DUR)))
		DTAP+=($((td1 - td0))) DVNIC+=($((vd1 - vd0))) DSOCK+=($((sd1 - sd0)))
		if [ "$dir" = tx ]; then
			FR+=($(((tb1 - tb0) / (tp1 - tp0 > 0 ? tp1 - tp0 : 1))))
		else
			FR+=($(((rb1 - rb0) / (rp1 - rp0 > 0 ? rp1 - rp0 : 1))))
		fi
	done
}

tcp() {	# tcp <name> <tx|rx> <iperf3 args>
	local name=$1 dir=$2
	shift 2
	measure "$dir" "$@"
	printf '%-10s Mbit/s %-32s median %-6s retrans %-18s IRQ/s %-6s frame %s B\n' "$name" \
		"${MB[*]}" "$(median "${MB[@]}")" "${RT[*]}" "$(median "${IRQ[@]}")" "$(median "${FR[@]}")"
}

udp_tx() {	# udp_tx <name> <iperf3 args>
	local name=$1
	shift
	measure tx -R -u -b 0 -l 64 "$@"
	printf '%-10s pkt/s  %-32s median %-6s loss%% %-20s IRQ/s %-6s frame %s B\n' "$name" \
		"${PPS[*]}" "$(median "${PPS[@]}")" "$(median "${LOSS[@]}")" "$(median "${IRQ[@]}")" "$(median "${FR[@]}")"
}

udp_rx() {
	local rate best=none m
	for rate in $RATES; do
		measure rx -u -l 64 -b $((rate * 64 * 8))
		m=$(median "${LOSS[@]}")
		printf '%-10s %6s pkt/s offered: loss%% %-30s median %-6s IRQ/s %-5s drops (median): tap0 %s, vnic0 %s, socket %s\n' \
			udp-rx "$rate" "${LOSS[*]}" "$m" "$(median "${IRQ[@]}")" \
			"$(median "${DTAP[@]}")" "$(median "${DVNIC[@]}")" "$(median "${DSOCK[@]}")"
		awk -v m="$m" 'BEGIN { exit !(m < 1) }' && best=$rate
	done
	echo "udp-rx     highest offered rate with median loss under 1%: $best pkt/s"
}

for t in $TESTS; do
	case "$t" in
	tcp-tx)    tcp tcp-tx tx -R ;;
	tcp-rx)    tcp tcp-rx rx ;;
	udp-tx)    udp_tx udp-tx ;;
	udp-tx-p4) udp_tx udp-tx-p4 -P 4 ;;
	udp-rx)    udp_rx ;;
	*)         echo "unknown test $t" >&2 ;;
	esac
done

quit_vm
