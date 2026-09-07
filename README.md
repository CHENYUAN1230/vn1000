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

The core datapath (`driver/vnic-core.c`) never touches hardware directly — it
calls only `vnic_hal_rd32/wr32`, `vnic_hal_ring_alloc`, `vnic_hal_map` etc.
(`include/vnic-hal.h`), and a backend implements them:

- `driver/vnic-hal-engine.c` — the baseline: an in-kernel engine kthread that
  plays the device (the prototype's model, now behind the seam)
- real PCI / DMA / MSI — from Phase 2 on

Swapping the backend is what lets the same datapath run first against the
kthread, then against QEMU, with the core unchanged.

## Status

| Phase | Scope | Status |
|---|---|---|
| 1 | HAL split — register/DMA seam extracted, engine + `ICR`/`ICS`/`IMS`/`IMC` semantics moved behind an in-kernel backend; datapath unchanged; prototype test suite re-runs green | **Done** (`e1ce188`) |
| 2 | QEMU PCI device skeleton (QOM, vendor/device ID, BAR0 MMIO, `msi_init`) + `pci_driver` probe (`pci_iomap`, `dma_set_mask`, `pci_alloc_irq_vectors` MSI, `request_irq`); self-built QEMU pinned to the guest's version | Not started |
| 3 | DMA datapath — `dma_alloc_coherent` rings, `dma_map_single` buffers, real MSI ISR → `napi_schedule`; QEMU device does IP-swap loopback internally so the full test methodology re-runs over real DMA/MMIO/MSI | Not started |
| 4 | Real `tap` backend — device TX `qemu_send_packet`, RX `can_receive`/`receive`; ping guest ↔ host, `tcpdump` on the tap | Not started |
| 5 | Finalize the netdev; prototype (`vnic_loop0`) and vn1000 (`vnic0`) loadable at once, distinct names, no subnet clash; real-peer test variant | Not started |
| 6 | Polish — TX doorbell moderation, device-side ITR, MTU sweep, KASAN/lockdep/kmemleak gate on the DMA paths, docs/diagrams, demo | Not started |

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

## Layout

```
driver/vnic-core.c        netdev core: rings, NAPI, TX/RX, ndo ops
driver/vnic-hal-engine.c  baseline backend: the in-kernel engine kthread
include/vnic-hal.h        the core <-> backend seam
include/vnic.h            private structs (vnic_priv, vnic_ring), ring sizes
include/vnic_regs.h       register / descriptor spec (shared with the QEMU device)
driver/Makefile           out-of-tree kbuild Makefile
driver/raw_flood.py       raw-socket flood helper for past-ring-capacity stress
test-logs/                per-phase test logs
```

## License

GPL-2.0, see [`LICENSE`](LICENSE).
