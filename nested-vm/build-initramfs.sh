#!/bin/bash
# build-initramfs.sh [module.ko ...]
#
# Builds nested-vm/initramfs.cpio.gz: busybox + an /init that mounts the
# pseudo filesystems, prints the PCI vendor/device of every device, and drops
# to a shell. Any .ko paths given on the command line are copied to / in the
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

cat > "$ROOT/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox --install -s /bin
mount -t proc     none /proc
mount -t sysfs    none /sys
mount -t devtmpfs none /dev
echo
echo "=== nested VM: vn1000 phase 2 test shell ==="
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

echo "modules in /:  $(ls /*.ko 2>/dev/null)"
echo "try:  insmod /vnic.ko ; dmesg | grep -i vnic"
echo
exec setsid cttyhack sh
INIT
chmod +x "$ROOT/init"

( cd "$ROOT" && find . | cpio -o -H newc --quiet | gzip -9 ) > "$HERE/initramfs.cpio.gz"
echo "built $HERE/initramfs.cpio.gz ($(du -h "$HERE/initramfs.cpio.gz" | cut -f1))"
echo "modules: ${MODS[*]}"
