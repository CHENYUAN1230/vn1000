#!/bin/bash
# build-initramfs.sh [module.ko ...]
#
# Builds nested-vm/initramfs.cpio.gz: busybox + an /init that mounts the
# pseudo filesystems, prints the PCI vendor/device of every device, and drops
# to a shell (or runs a test, see vn1000.test= in the /init below). Any .ko
# paths given on the command line are copied to / in the
# image (default: driver/vnic.ko next to this script's repo, whichever
# backend it was last built with).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/root"
MODS=("$@")
[ ${#MODS[@]} -eq 0 ] && MODS=("$HERE/../driver/vnic.ko")

rm -rf "$ROOT"
mkdir -p "$ROOT"/{bin,proc,sys,dev,tmp}

cp "$(command -v busybox)" "$ROOT/bin/busybox"

for ko in "${MODS[@]}"; do
	cp "$ko" "$ROOT/$(basename "$ko")"
done

[ -x "$HERE/../driver/raw_flood" ] && cp "$HERE/../driver/raw_flood" "$ROOT/raw_flood"

# host iperf3 plus its shared libs, same paths (L2 runs the host kernel and glibc).
# libgcc_s is dlopen()ed by pthread_cancel, so ldd does not list it.
if IPERF="$(command -v iperf3)"; then
	GCC_S="$(ldconfig -p | grep -o '/[^ ]*/libgcc_s.so.1' | head -1)"
	for f in "$IPERF" $(ldd "$IPERF" | grep -o '/[^ ]*') $GCC_S; do
		mkdir -p "$ROOT$(dirname "$f")"
		cp -L "$f" "$ROOT$f"
	done
fi

cat > "$ROOT/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox --install -s /bin
mount -t proc     none /proc
mount -t sysfs    none /sys
mount -t devtmpfs none /dev
echo
echo "=== nested VM: vn1000 test shell ==="
echo "--- PCI devices (vendor:device) ---"
for d in /sys/bus/pci/devices/*/; do
	printf '%s  %s:%s  class %s\n' "$(basename "$d")" \
		"$(cat "$d/vendor")" "$(cat "$d/device")" "$(cat "$d/class")"
done
echo "-----------------------------------"

# helpers for the test branches below
bar0() {	# BAR0 address of the VN1000, found by PCI ID; empty if there is none
	for d in /sys/bus/pci/devices/*; do
		[ "$(cat "$d/device")" = 0x11e9 ] && { head -1 "$d/resource" | cut -d' ' -f1; return; }
	done
}
nstat() {	# nstat <field>: vnic0's /proc/net/dev field (2 rx bytes, 3 rx pkts, 5 rx drop, 10 tx bytes, 11 tx pkts, 13 tx drop)
	awk -v f="$1" '$1 == "vnic0:" { print $f }' /proc/net/dev
}
pingok() {	# pingok <ip> [size] [count]: "ok" if no loss
	ping -c "${3:-3}" -i 0.3 -W 1 ${2:+-s "$2"} "$1" | grep -q " 0% packet loss" && echo ok || echo fail
}
# txdrop_check <peer> [cmd]: lower MAXFRM to 500 behind the driver's back,
# send three 1042 byte frames, then down/up (cmd runs after the up, e.g. to
# re-add a static ARP entry). Prints the tx_dropped, rx_dropped and tx_bytes
# changes, MAXFRM after the down/up, and whether a 1042 byte ping then works.
txdrop_check() {
	local b t0 r0 y0 t1 r1 y1
	b=$(bar0)
	t0=$(nstat 13); r0=$(nstat 5); y0=$(nstat 10)
	devmem $((b + 0x20)) 32 500
	ping -c 3 -i 0.3 -W 1 -s 1000 "$1" >/dev/null
	t1=$(nstat 13); r1=$(nstat 5); y1=$(nstat 10)
	ip link set vnic0 down; ip link set vnic0 up; ${2:-}
	sleep 2
	echo "=== VNIC TXDROP $((t1 - t0)) $((r1 - r0)) $((y1 - y0)) $(($(devmem $((b + 0x20)) 32))) $(pingok "$1" 1000)"
}

# non-interactive: pass  vn1000.test=1  on the kernel cmdline to insmod, dump
# the driver's dmesg, and power off. Otherwise drop to a shell.
if grep -q vn1000.test=1 /proc/cmdline; then
	echo "+ insmod /vnic.ko"
	insmod /vnic.ko && echo "insmod OK" || echo "insmod FAILED rc=$?"
	echo "--- dmesg (vnic) ---"
	dmesg | grep -i vnic || dmesg | tail -20
	poweroff -f
fi

# guest half of test-tap.sh: vn1000.test=tap. Prints "=== VNIC ..." markers
# that the host script waits on; the host ends the VM via the QEMU monitor.
if grep -q vn1000.test=tap /proc/cmdline; then
	rx() { echo "$(nstat 3) $(nstat 5)"; }   # RX packets, RX drop
	insmod /vnic.ko
	ip link set vnic0 up
	ip addr add 10.1.0.1/24 dev vnic0
	echo "=== VNIC MAC $(cat /sys/class/net/vnic0/address)"
	echo "=== VNIC A $(pingok 10.1.0.2)"
	ip link set vnic0 down; sleep 1; ip link set vnic0 up
	echo "=== VNIC B $(pingok 10.1.0.2)"
	echo "=== VNIC READY"
	nc -l -p 9000 </dev/null >/dev/null        # host connects once its C/D/NEIGH checks are done
	echo "=== VNIC DOWN $(rx)"
	ip link set vnic0 down; sleep 10; ip link set vnic0 up; sleep 3
	echo "=== VNIC UP $(rx)"
	txdrop_check 10.1.0.2
	echo "=== VNIC DONE"
	exec sleep 3600
fi

# guest half of test-loopback.sh: vn1000.test=loopback. Runs Phase 3's
# loopback suite plus BQL, TX drop and dmesg checks, one marker per check.
# Works for both backends; without a VN1000 (engine build) TXDROP is skipped.
if grep -q vn1000.test=loopback /proc/cmdline; then
	arp_lb() { arp -s 10.1.0.2 02:00:00:00:00:02; }   # the loopback has no peer to answer ARP
	up() { ip link set vnic0 up; ip addr add 10.1.0.1/24 dev vnic0 2>/dev/null; arp_lb; }
	insmod /vnic.ko
	up
	echo "=== VNIC PING $(pingok 10.1.0.2 '' 5)"
	r=ok
	for sz in 0 1 64 1400 1472 1473; do
		[ "$(pingok 10.1.0.2 $sz 5)" = ok ] || r="fail at -s $sz"
	done
	echo "=== VNIC SIZES $r"
	echo "=== VNIC FRAG $(pingok 10.1.0.2 65000 5)"
	r=""
	ip link set vnic0 mtu 67 2>/dev/null && r="$r 67_accepted"
	ip link set vnic0 mtu 9001 2>/dev/null && r="$r 9001_accepted"
	ip link set vnic0 mtu 9000 2>/dev/null && r="$r 9000_accepted_while_up"
	ip link set vnic0 down
	ip link set vnic0 mtu 9000 2>/dev/null || r="$r 9000_rejected_while_down"
	ip link set vnic0 mtu 1500
	ip link set vnic0 up; arp_lb
	echo "=== VNIC MTU ${r:-ok}"
	ip link set vnic0 down; ip link set vnic0 up; arp_lb
	echo "=== VNIC DOWNUP $(pingok 10.1.0.2 '' 5)"
	t0=$(nstat 11); r0=$(nstat 3)
	/raw_flood 5000 >/dev/null; rc=$?
	sleep 1
	echo "=== VNIC FLOOD $rc $(($(nstat 11) - t0)) $(($(nstat 3) - r0))"
	echo "=== VNIC BQL $(cat /sys/class/net/vnic0/queues/tx-0/byte_queue_limits/inflight)"
	if [ -n "$(bar0)" ]; then txdrop_check 10.1.0.2 arp_lb; else echo "=== VNIC TXDROP skip"; fi
	rmmod vnic; rc=$?
	[ -e /sys/class/net/vnic0 ] && rc="$rc vnic0_still_present"
	echo "=== VNIC RMMOD $rc"
	r=ok
	for i in 1 2 3 4 5; do
		insmod /vnic.ko && up && [ "$(pingok 10.1.0.2)" = ok ] && ip link set vnic0 down && rmmod vnic ||
			{ r="fail in round $i"; break; }
	done
	echo "=== VNIC RELOAD $r"
	echo "=== VNIC DMESG $(dmesg | grep -c -E 'WARNING|BUG|Oops|refcount|underflow')"
	echo "=== VNIC DONE"
	exec sleep 3600
fi

# guest half of bench-tap.sh: vn1000.test=bench. Brings vnic0 up on tap0 at
# the MTU given by vn1000.mtu=, runs the iperf3 server, then answers each
# connection to port 9000 with one SNAP line: IRQ count, then vnic0's rx
# bytes, rx packets, tx bytes, tx packets, rx dropped, then UDP RcvbufErrors
# (datagrams dropped because the receiving socket's buffer was full).
if grep -q vn1000.test=bench /proc/cmdline; then
	rcvbuf() {
		awk '/^Udp:/ { if (!c) { for (i = 1; i <= NF; i++) if ($i == "RcvbufErrors") c = i } else { print $c; exit } }' /proc/net/snmp
	}
	mtu=$(sed -n 's/.*vn1000\.mtu=\([0-9]*\).*/\1/p' /proc/cmdline)
	insmod /vnic.ko
	[ -n "$mtu" ] && ip link set vnic0 mtu "$mtu"
	ip link set vnic0 up
	ip addr add 10.1.0.1/24 dev vnic0
	/usr/bin/iperf3 -s -D
	echo "=== VNIC READY $(cat /sys/class/net/vnic0/mtu)"
	while :; do
		nc -l -p 9000 </dev/null >/dev/null
		echo "=== VNIC SNAP $(awk '/vnic/ { print $2 + $3 }' /proc/interrupts) $(nstat 2) $(nstat 3) $(nstat 10) $(nstat 11) $(nstat 5) $(rcvbuf)"
	done
fi

echo "modules in /:  $(ls /*.ko 2>/dev/null)"
echo "try:  insmod /vnic.ko ; dmesg | grep -i vnic"
echo
exec setsid cttyhack sh
INIT
chmod +x "$ROOT/init"

( cd "$ROOT" && find . | cpio -o -H newc --quiet | gzip -9 ) > "$HERE/initramfs.cpio.gz"
echo "built $HERE/initramfs.cpio.gz ($(du -h "$HERE/initramfs.cpio.gz" | cut -f1))"
echo "modules: ${MODS[*]}"
