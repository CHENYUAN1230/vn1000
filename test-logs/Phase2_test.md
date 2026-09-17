# Phase 2 Test Log

## Background

Phase 2 goal (the keystone): replace the in-kernel engine's fake register file
with a real emulated PCI device, and prove the three-part round trip before
any datapath exists:

1. `lspci` (via sysfs) shows the device: vendor:device `1234:11e9`
2. `readl(VN_REG_ID)` over real MMIO returns `0x564E3130`
3. a device-raised MSI reaches the driver's ISR

No descriptor rings, no DMA, and no netdev yet: those are Phase 3. The test goal
is exactly the three acceptance items above, plus confirming the new
`VNIC_BACKEND=pci` build does not disturb the existing `VNIC_BACKEND=engine`
build (Phase 1's control group).

Two new pieces, one per side of the MMIO/MSI boundary:

| file | role |
|---|---|
| `qemu-device/vnic.c` | the emulated device: QOM/PCIDevice, BAR0 register file, one MSI |
| `driver/vnic-pci.c` | the second `vnic-hal.h` backend: `readl`/`writel` HAL, `pci_driver` probe/remove |

## Environment

```
guest:   vnic-guest, bumped from 2 GiB/2 vCPU to 6 GiB/8 vCPU for the QEMU build
QEMU:    9.1.3, self-built, shallow clone, x86_64-softmmu only
         ./configure --target-list=x86_64-softmmu --enable-kvm --enable-slirp
                     --disable-docs --disable-werror --disable-gtk --disable-sdl
                     --disable-opengl --disable-vnc --disable-spice
                     --disable-tools --disable-guest-agent --disable-fdt
nested VM harness: nested-vm/, using this guest's own kernel (6.8.0-139-generic) +
         a busybox initramfs holding vnic.ko, booted under the just-built QEMU.
         TCG (no nested KVM group active this session); ~3-5s to a shell.
```

9.1.3 was chosen deliberately: Ubuntu 24.04's apt `meson` (1.3.2) is new
enough for 9.1's build but not 9.2's (which needs meson >= 1.5), and 9.1's
`class_init(ObjectClass *, void *data)` matches the code as written (9.2
added `const` to `data`).

## Test Steps

### 1. QEMU device builds into the tree

```bash
cp qemu-device/vnic.c        ~/qemu/hw/net/vnic.c
cp include/vnic_regs.h       ~/qemu/hw/net/vnic_regs.h
# hw/net/meson.build: + system_ss.add(when: 'CONFIG_VNIC', if_true: files('vnic.c'))
# hw/net/Kconfig:     + config VNIC / bool / default y if PCI_DEVICES / depends on PCI
ninja -C build qemu-system-x86_64
```

Expected: builds clean, `-device help` lists `vnic`.

Result:

```
[3/3] Linking target qemu-system-x86_64

$ qemu-system-x86_64 -device help | grep -i vnic
name "vnic", bus PCI, desc "VN1000 virtual 1GbE NIC"
```

No warnings (QEMU's `hw/net/` objects build with `-Werror`, so a bounds/type
mistake in `vnic.c` would fail the build outright: it did, twice, during
development: once on a nested `/* */` inside a skeleton comment, once on an
unused static before its caller was filled in).

PASSED.

### 2. Driver builds clean, both backends

```bash
cd driver
make clean && make VNIC_BACKEND=pci
make clean && make                    # VNIC_BACKEND defaults to engine
```

Expected: both build with zero warnings; `vnic-core.o` is shared, only the
backend `.o` changes.

Result:

```
  CC [M]  vnic-core.o
  CC [M]  vnic-pci.o
  LD [M]  vnic.o
  LD [M]  vnic.ko
pci build clean

  CC [M]  vnic-core.o
  CC [M]  vnic-engine.o
  LD [M]  vnic.o
  LD [M]  vnic.ko
engine build clean
```

`checkpatch --strict` on `vnic-pci.c`: `0 errors, 1 warnings` (the one
warning (filename mentioned in the file header) matches the same accepted
convention as `vnic-core.c`/`vnic-engine.c`).

PASSED. Phase 1's engine backend (the control group) is unaffected by the
new backend existing alongside it.

### 3. PCI enumeration

```bash
TEST=1 nested-vm/run.sh -device vnic
```

Expected: the emulated device appears with vendor:device `1234:11e9`, class
`0200` (Ethernet controller, matching `vnic_class_init()`'s `PCI_CLASS_NETWORK_ETHERNET`).

Result (nested VM's `/sys/bus/pci/devices/*/`):

```
0000:00:01.0  0x1234:0x1111  class 0x030000   (QEMU's stdvga)
0000:00:03.0  0x1234:0x11e9  class 0x020000   (our device)
```

PASSED.

### 4. ID readback over real MMIO + MSI self-test

Same run, `vnic_pci_probe()`'s dmesg output:

```
[    4.695956] vnic: loading out-of-tree module taints kernel.
[    4.696460] vnic: module verification failed: signature and/or required key missing - tainting kernel
[    4.718485] vnic 0000:00:03.0: VN1000 found over MMIO, ID=0x564e3130
[    4.722329] vnic 0000:00:03.0: MSI self-test passed
insmod OK
```

`ID=0x564e3130` is `VN_ID_MAGIC` ("VN10"), read via `pcim_iomap_table()`'s
BAR0 pointer with a plain `readl()`: the first byte to cross the MMIO
boundary in either direction. "MSI self-test passed" means the full round
trip worked: `vnic_pci_msi_selftest()` unmasked TXDONE and raised it via ICS
-> the device's `vnic_irq_update()` saw `ICR & IMS != 0` and called
`msi_notify()` -> QEMU injected the vector into the guest CPU -> the guest
kernel routed it to `vnic_pci_isr()`, which set `msi_seen`.

PASSED. Both halves of the Phase 2 acceptance criteria that need the guest
kernel running (items 2 and 3) are met. Item 1 (`lspci`) was step 3.

### 5. insmod / rmmod / re-insmod: clean reload

```bash
insmod /vnic.ko
rmmod vnic
insmod /vnic.ko
```

Expected: `vnic_pci_remove()` tears down without a crash or warning, and a
second `probe()` on the same device produces the identical result (no stale
state from the BAR mapping, MSI vector, or `struct vnic_hal`).

Result:

```
[first insmod]
vnic 0000:00:03.0: VN1000 found over MMIO, ID=0x564e3130
vnic 0000:00:03.0: MSI self-test passed
--- rmmod: no error, dmesg clean ---
[second insmod]
vnic 0000:00:03.0: VN1000 found over MMIO, ID=0x564e3130
vnic 0000:00:03.0: MSI self-test passed
```

Identical ID readback and a second passing self-test confirm `pcim_*`
devres correctly released BAR0 and the MSI vector on `rmmod`, and
`vnic_pci_probe()` re-acquires them cleanly the second time round.

PASSED.

## Conclusion

Phase 2 (the keystone) passes all three acceptance items, exercised together
in one nested-VM boot: PCI enumeration, a real MMIO register readback, and a
device-raised MSI reaching the driver's ISR. The `VNIC_BACKEND` build switch
introduced in Phase 1 does its job: `vnic-core.o` is untouched, the engine
backend still builds and (per Phase 1's own test log) still runs, and the new
`vnic-pci.c` backend is exercised end to end against a real, from-scratch
QEMU device model. A clean insmod/rmmod/insmod cycle rules out the most
common Phase-2-shaped bug (devres or MSI vectors not released on teardown).

Not done, deferred to Phase 3 by design: descriptor rings, `dma_alloc_coherent`,
`dma_map_single`, `register_netdev`, and therefore no `ping`/throughput
testing yet: there is no netdev to ping. `vnic_reset` (bus reset / guest
reboot handling) is also unimplemented; nothing in Phase 2 exercises a reboot
without a full QEMU restart, so it was left out rather than stubbed.
