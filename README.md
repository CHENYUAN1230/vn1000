# vn1000

A virtual 1GbE NIC built from scratch: a QEMU PCI device model and its Linux
driver, developed against each other so both sides of the MMIO / DMA boundary
agree by construction.

The Linux driver is `vnic` — the VN1000's driver, the way `igb` is the I210's.
`include/vnic_regs.h` is the hardware spec, shared verbatim by the driver and
the QEMU device.

## Origin

The descriptor-ring / NAPI datapath was first proven in a pure-software
prototype, [`vnic-l1`](https://github.com/CHENYUAN1230/vnic-l1): a `priv->regs[]`
array as the register file and a kthread as the "device", no PCI or DMA. This
repo takes that datapath **byte-identical** and runs it over real PCI / MMIO /
DMA / MSI against a purpose-built QEMU device.

| | prototype (vnic-l1) | vn1000 |
|---|---|---|
| register access | `priv->regs[]` array | `readl`/`writel` on BAR0 MMIO |
| the "device" | engine kthread | QEMU PCI device (`hw/net/vnic.c`) |
| ring memory | `kcalloc` | `dma_alloc_coherent` |
| packet buffers | direct pointer | `dma_map_single` |
| interrupt | `ICS` write → tasklet | device raises a real MSI → ISR |
| entry point | `module_init` | `pci_register_driver` + `probe` |

## The HAL seam

The core datapath (`driver/vnic-core.c`) never touches hardware directly: it
calls only `vnic_hal_rd32/wr32`, `vnic_hal_ring_alloc`, `vnic_hal_map` etc.
(`include/vnic-hal.h`), and one of two backends implements them, picked at
build time (`make VNIC_BACKEND=engine|pci`, default `engine`):

- `driver/vnic-engine.c`: the baseline, an in-kernel engine kthread that plays
  the device (the prototype's model, now behind the seam)
- `driver/vnic-pci.c`: real PCI / MMIO / MSI (DMA from Phase 3) against
  `qemu-device/vnic.c`, a from-scratch QEMU device model

Swapping the backend is what lets the same core run first against the
kthread, then against QEMU, with `vnic-core.c` unchanged.

## Status

| Phase | Scope | Status |
|---|---|---|
| 1 | HAL split: register/DMA seam extracted, engine + `ICR`/`ICS`/`IMS`/`IMC` semantics moved behind an in-kernel backend; datapath unchanged; prototype test suite re-runs green | **Done**, see [`test-logs/Phase1_test.md`](test-logs/Phase1_test.md) |
| 2 | QEMU PCI device (QOM, vendor/device ID, BAR0 MMIO, `msi_init`) + `pci_driver` probe (`pcim_iomap`, `pci_alloc_irq_vectors` MSI, `request_irq`); self-built QEMU 9.1.3; `lspci` sees the device, `readl` reads it over real MMIO, a device-raised MSI reaches the ISR | **Done**, see [`test-logs/Phase2_test.md`](test-logs/Phase2_test.md) |
| 3 | DMA datapath: `dma_alloc_coherent` rings, `dma_map_single` buffers, real MSI ISR → `napi_schedule`, `register_netdev`; QEMU device does IP-swap loopback internally so the full test methodology re-runs over real DMA/MMIO/MSI | Not started |
| 4 | Real `tap` backend: device TX `qemu_send_packet`, RX `can_receive`/`receive`; ping guest ↔ host, `tcpdump` on the tap | Not started |
| 5 | Finalize the netdev; prototype (`vnic_loop0`) and vn1000 (`vnic0`) loadable at once, distinct names, no subnet clash; real-peer test variant | Not started |
| 6 | Polish: TX doorbell moderation, device-side ITR, MTU sweep, KASAN/lockdep/kmemleak gate on the DMA paths, docs/diagrams, demo | Not started |

## Building and loading

```bash
cd driver
make
sudo insmod vnic.ko
ip link show vnic0

sudo ip link set vnic0 up
sudo ip addr add 10.1.0.1/24 dev vnic0
sudo ip neigh replace 10.1.0.2 lladdr 02:00:00:00:00:02 dev vnic0 nud permanent
ping -c 5 10.1.0.2

sudo ip link set vnic0 down
sudo rmmod vnic
```

The loopback engine swaps the Ethernet (and IPv4) addresses and reflects each
frame, so `ping 10.1.0.2` comes back from `10.1.0.1`; the static neighbour entry
stands in for the (fictional) peer's ARP reply.

`make DEBUG=y` builds with `-DVNIC_DEBUG`, enabling the `PDEBUG()` trace macro.

### The PCI backend (Phase 2)

`vnic-pci.c` has no netdev yet (that's Phase 3), so there is nothing to `ping`;
what you can check is that the driver finds the device over real MMIO and
that its MSI reaches the ISR. This needs `qemu-device/vnic.c` wired into a
QEMU build (see the note at the bottom of that file) and a VM to run it in,
since the device only exists inside that QEMU binary; `nested-vm/` boots one
using this machine's own kernel, so no separate VM image is needed.

```bash
make VNIC_BACKEND=pci

cd nested-vm
cp /boot/vmlinuz-$(uname -r) vmlinuz   # once: your own kernel image, not tracked in git
./build-initramfs.sh                   # packs busybox + ../driver/vnic.ko
TEST=1 ./run.sh -device vnic           # boots, insmods, prints dmesg, powers off
```

Expected in the output: `VN1000 found over MMIO, ID=0x564e3130` and
`MSI self-test passed`. Drop `TEST=1` for an interactive shell instead
(`Ctrl-a x` quits qemu); more options (`KVM=1`, `QEMU_BIN=`) are documented
in `run.sh`'s header comment. Full walkthrough in
[`test-logs/Phase2_test.md`](test-logs/Phase2_test.md).

## Layout

```
driver/vnic-core.c        netdev core: rings, NAPI, TX/RX, ndo ops
driver/vnic-engine.c      backend: the in-kernel engine kthread (VNIC_BACKEND=engine)
driver/vnic-pci.c         backend: real PCI/MMIO/MSI (VNIC_BACKEND=pci)
qemu-device/vnic.c        the emulated PCI device vnic-pci.c talks to
include/vnic-hal.h        the core <-> backend seam
include/vnic.h            private structs (vnic_priv, vnic_ring), ring sizes
include/vnic_regs.h       register / descriptor spec (shared with the QEMU device)
driver/Makefile           out-of-tree kbuild Makefile
driver/raw_flood.py       raw-socket flood helper for past-ring-capacity stress
nested-vm/                scripts that boot a throwaway VM to test the PCI backend
test-logs/                per-phase test logs
```

## License

GPL-2.0, see [`LICENSE`](LICENSE).
