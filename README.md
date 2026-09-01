# vnic-l2

Level 2 of the `vnic` NIC-driver learning project: take the finished
[`vnic-l1`](https://github.com/CHENYUAN1230/vnic-l1) descriptor-ring / NAPI datapath and run it
over **real PCI / MMIO / DMA / MSI** against a purpose-built QEMU PCI device, instead of an
in-kernel engine kthread pretending to be the hardware.

## What this is

`vnic-l1` proved the datapath (rings, ownership handshakes, NAPI, simulated MSI) with everything in
software: a `priv->regs[]` array as the register file and a kthread as the "device". `vnic-l2`
keeps that datapath **byte-identical** and swaps out only what sits underneath it:

| | vnic-l1 | vnic-l2 |
|---|---|---|
| register access | `priv->regs[]` array | `readl`/`writel` on BAR0 MMIO |
| the "device" | engine kthread (`vnic_engine_run`) | QEMU PCI device (`hw/net/vnic.c`) |
| ring memory | `kcalloc` | `dma_alloc_coherent` |
| packet buffers | direct pointer | `dma_map_single` |
| interrupt | `ICS` write → tasklet | device raises a real MSI → ISR |
| entry point | `module_init` | `pci_register_driver` + `probe` |

The seam that makes this possible is a **HAL** (`vnic-hal.h`): the core datapath calls only
`vnic_hal_rd32/wr32`, `vnic_hal_ring_alloc`, `vnic_hal_map` etc., and a backend implements them —
the in-kernel engine for the baseline, real PCI/DMA from Phase 2 on.

`include/vnic_regs.h` is copied verbatim from `vnic-l1` and will be shared with the QEMU device
source, so both sides of the MMIO boundary agree on the register map by construction.

## Status

Phases renumber from 1 (Level-2 Phase N corresponds to the reference project's Phase N+7).

| Phase | Scope | Status |
|---|---|---|
| 1 | HAL split — extract the register/DMA seam, move the engine + `ICR`/`ICS`/`IMS`/`IMC` semantics behind an L1-style backend; core datapath unchanged; old vnic-l1 test suite re-runs green | In progress |
| 2 | QEMU PCI device skeleton (QOM, vendor/device ID, BAR0 MMIO, `msi_init`) + `pci_driver` probe (`pci_iomap`, `dma_set_mask`, `pci_alloc_irq_vectors` MSI, `request_irq`); self-built QEMU pinned to the guest's version | Not started |
| 3 | DMA datapath — `dma_alloc_coherent` rings, `dma_map_single` buffers, real MSI ISR → `napi_schedule`; QEMU device does IP-swap loopback internally so the full Phase 1–7 methodology re-runs over real DMA/MMIO/MSI | Not started |
| 4 | Real `tap` backend — device TX `qemu_send_packet`, RX `can_receive`/`receive`; ping guest ↔ host, `tcpdump` on the tap | Not started |
| 5 | Finalize L2 netdev; L1 (`vnic_loop0`) and L2 (`vnic0`) loaded at once, distinct names, no subnet clash; real-peer test variant | Not started |
| 6 | Polish — TX doorbell moderation, device-side ITR, MTU sweep, KASAN/lockdep/kmemleak gate on the DMA paths, docs/diagrams, demo | Not started |

## Baseline

The initial commit is `vnic-l1`'s Phase 7 driver copied over and renamed (`vnic-l2.c` /
`vnic-l2.h`, module `vnic-l2`, netdev `vnicl2_loop%d`, engine kthread `vnic-l2-engine`) with **no
logic changes** — it builds and behaves exactly like `vnic-l1`. Phase 1's diff against this commit
should therefore be confined to introducing the HAL seam.

## Building and loading

```bash
cd driver
make
sudo insmod vnic-l2.ko
ip link show vnicl2_loop0

sudo ip link set vnicl2_loop0 up
sudo ip addr add 10.1.0.1/24 dev vnicl2_loop0
sudo ip neigh replace 10.1.0.2 lladdr 02:00:00:00:00:02 dev vnicl2_loop0 nud permanent
ping -c 5 10.1.0.2

sudo ip link set vnicl2_loop0 down
sudo rmmod vnic-l2
```

`make DEBUG=y` builds with `-DVNIC_DEBUG`, enabling the `PDEBUG()` trace macro.

## Layout

```
driver/vnic-l2.c         driver implementation (baseline: monolithic; split into core + HAL in Phase 1)
include/vnic-l2.h        private structs (vnic_priv, vnic_ring) and ring-size constants
include/vnic_regs.h      register offsets, descriptor layout, bit definitions (shared with the QEMU device)
driver/Makefile          out-of-tree kbuild Makefile
driver/raw_flood.py      raw-socket flood helper for past-ring-capacity stress testing
test-logs/               per-phase test logs
```

## License

GPL-2.0, see [`LICENSE`](LICENSE).
