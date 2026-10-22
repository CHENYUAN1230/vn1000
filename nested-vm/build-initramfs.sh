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
mkdir -p "$ROOT"/{bin,proc,sys,dev}

cp "$(command -v busybox)" "$ROOT/bin/busybox"

for ko in "${MODS[@]}"; do
	cp "$ko" "$ROOT/$(basename "$ko")"
done

[ -x "$HERE/../driver/raw_flood" ] && cp "$HERE/../driver/raw_flood" "$ROOT/raw_flood"

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
	pingok() { ping -c 3 -i 0.3 -W 1 "$1" | grep -q " 0% packet loss" && echo ok || echo fail; }
	rx() { grep vnic0 /proc/net/dev | awk '{print $3, $5}'; }   # RX packets, RX drop
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
	echo "=== VNIC DONE"
	exec sleep 3600
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
