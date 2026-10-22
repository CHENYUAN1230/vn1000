#!/bin/bash
# run.sh [extra qemu args...]
#
# Boots the nested VM: a Linux kernel image you provide (nested-vm/vmlinuz,
# not tracked in git - copy your own, e.g.
# `cp /boot/vmlinuz-$(uname -r) nested-vm/vmlinuz`) plus
# nested-vm/initramfs.cpio.gz (build it with build-initramfs.sh first),
# serial console, no disk. Extra args are appended to the qemu command line:
#
#   ./run.sh -device vnic
#   TEST=1 ./run.sh -device vnic         # non-interactive: insmod + dmesg + poweroff
#   TEST=tap ...                         # guest half of test-tap.sh (run that instead)
#   KVM=1  ./run.sh -device vnic         # needs: sudo usermod -aG kvm $USER
#   QEMU_BIN=/path/to/qemu-system-x86_64 ./run.sh -device vnic
#
# -nic none stops QEMU from adding its default NIC, so the VN1000 is always
# at 00:02.0 with the default MAC 52:54:00:12:34:56. Add other NICs
# explicitly with -netdev/-device if a test needs them.
#
# Ctrl-a x   quits qemu.   The guest poweroffs on 'poweroff' or shell exit.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU_BIN:-$HOME/qemu/build/qemu-system-x86_64}"

for f in "$HERE/vmlinuz" "$HERE/initramfs.cpio.gz"; do
	[ -f "$f" ] || { echo "missing $f - see the header comment in $0" >&2; exit 1; }
done

ACCEL=(-accel tcg)
[ "${KVM:-0}" = "1" ] && ACCEL=(-accel kvm -cpu host)

CMDLINE="console=ttyS0 panic=-1 rdinit=/init"
[ -n "${TEST:-}" ] && CMDLINE="$CMDLINE vn1000.test=$TEST"   # picks a test branch in /init

exec "$QEMU" \
	-M q35 "${ACCEL[@]}" -m 512 -smp 2 \
	-kernel "$HERE/vmlinuz" \
	-initrd "$HERE/initramfs.cpio.gz" \
	-append "$CMDLINE" \
	-nographic -no-reboot -nic none \
	"$@"
