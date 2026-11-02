# Phase 6 Test Log

## Background

Phase 6 goal: measure the datapath, find where its time goes, fix what the
numbers point at, and make every test and measurement a script that runs
the same way each time.

- **Test and benchmark scripts.** `nested-vm/test-loopback.sh` turns
  Phase 3's manual loopback suite into PASS/FAIL checks,
  `nested-vm/bench-tap.sh` measures throughput over the tap, and both share
  `nested-vm/common.sh` with `test-tap.sh`: an environment check before
  every run and a freshly packed initramfs. The VM image now carries
  `iperf3`.
- **TX doorbell batching.** `vnic_tx()` used to write `TDT` once per packet.
  It now supports BQL and rings the doorbell only when the stack has no
  more packets queued for it (`xmit_more`), so a burst costs one `TDT`
  write.
- **ISR masking.** `vnic_pci_isr()` scheduled NAPI without masking the
  device's interrupt causes, so the device kept raising MSIs while a poll was
  pending or running. It now masks them first, as NAPI expects.
- **TX drop accounting.** A TX frame the device rejects is now counted only
  in `STAT_TXDROP`, in both device modes and in the engine backend.
- **Engine backend interrupt model.** `test-loopback.sh` crashed the guest
  kernel on the engine backend. The fix gives the engine the same interrupt
  model as the QEMU device, and makes `vnic_open()` install the interrupt
  handler before it starts the device.

## Environment

```
guest:   nested-VM harness (nested-vm/), busybox init, -smp 2 -m 512, q35
host:    kernel 6.8.0-147-generic, 8 vCPUs, a QEMU/KVM VM (q35) running
         under WSL2 on a Windows laptop, Intel Core Ultra 5 125U
QEMU:    self-built 9.1.3 at ~/qemu; hw/net/vnic.c re-synced from
         qemu-device/vnic.c before every build
driver:  VNIC_BACKEND=pci unless noted; engine backend rebuilt and tested
         after each driver change, then the PCI build restored
network: tap0 on the host, 10.1.0.2/24 (setup-tap.sh); guest 10.1.0.1/24
```

The development machine is itself a VM: Windows runs WSL2, WSL2 runs
QEMU/KVM, and that VM is where this project is built and where `run.sh`
starts the test VM. `KVM=1` therefore runs the test VM under **nested**
virtualization, several levels deep. Every VM exit of the test VM goes to
the outermost hypervisor first and is handed back down level by level,
which makes exits roughly 20 to 100 times more expensive than on bare
metal (step 2). All throughput numbers in this log are under `KVM=1`.

Absolute throughput depends on the state of the laptop and moves a lot
between sessions, so only measurements taken back to back in the same
session are compared, and each one is the median of several runs.
`bench-tap.sh REV=<commit>` builds the driver of that commit, which is how
the "before" numbers are produced.

## Test Steps

### 1. Test and benchmark scripts

All three scripts start the same way (`common.sh`):

- `preflight`: stop if `nested-vm/vmlinuz` is not this host's kernel, if
  `~/qemu/hw/net/` differs from `qemu-device/vnic.c` /
  `include/vnic_regs.h`, or if the QEMU binary is older than its source;
  tap scripts also (re)create tap0 with `setup-tap.sh`.
- `pack`: rebuild the initramfs around the module under test, so the VM
  never runs a stale `vnic.ko`.
- `boot <test>`: start the VM with `vn1000.test=<test>`, which selects the
  matching guest half in `/init` (`build-initramfs.sh`). The guest reports
  over the serial console with `=== VNIC ...` lines.

| Script | Kind | What it covers |
|---|---|---|
| `test-tap.sh` | PASS/FAIL | Phase 5's 7 tap checks, plus `TXDROP` (step 5) |
| `test-loopback.sh` | PASS/FAIL | Phase 3's loopback suite (ping, payload sizes, 65000 byte fragmented ping, MTU range, down/up, `/raw_flood 5000`, `rmmod` while up, five reload rounds), plus BQL `inflight` back to 0, `TXDROP` and a clean `dmesg`; runs on either backend |
| `bench-tap.sh` | numbers | TCP TX/RX, 64 byte UDP TX with 1 and 4 streams, 64 byte UDP RX at fixed rates; every run also reports the guest's interrupt rate and average frame size |

```bash
cd nested-vm
./test-tap.sh
KVM=1 ./test-loopback.sh
KVM=1 ./bench-tap.sh                     # working tree driver
KVM=1 REV=07ab85f ./bench-tap.sh         # driver before Phase 6
KVM=1 MTU=9000 TESTS="tcp-tx tcp-rx" ./bench-tap.sh
```

`bench-tap.sh` runs the iperf3 server in the guest and the client on the
host, so every test is timed by the host's clock, and reads results from
iperf3's JSON output: TCP from the receiver side plus the sender's
retransmits, UDP from the receiver side with its loss. UDP RX is offered at
fixed rates (1000 to 100000 packets per second) and reported as the
highest rate whose median loss stays under 1%, along with where the lost
datagrams went: the host's tap0 queue, `vnic0`'s `rx_dropped`, or the
guest socket's full receive buffer (`RcvbufErrors`).

`build-initramfs.sh` copies the host's `iperf3` into the image with every
library `ldd` lists, plus `libgcc_s.so.1` (loaded by glibc's
`pthread_cancel` through `dlopen()`, so `ldd` does not list it) and a
`/tmp` (iperf3 creates a file per stream there). The image grew from 1.7 MB
to 5.2 MB.

### 2. Where the time went

Before any change, a 10 s guest TCP TX test reached 26 Mbit/s under KVM
against 384 Mbit/s under TCG. VM exits recorded on the host during that
test (development measurement, one run):

```bash
sudo perf kvm stat record -o kvm.data -p $(pgrep -x qemu-system-x86) -- sleep 10
sudo perf kvm -i kvm.data stat report --event=vmexit --stdio
sudo perf kvm -i kvm.data stat report --event=mmio --stdio
```

| Exit | Count | Avg time |
|---|---|---|
| EPT_MISCONFIG (all MMIO) | 49330 | 94.7 µs |
| &nbsp;&nbsp;MMIO write `TDT` (0x110) | 20615 | 88.6 µs |
| &nbsp;&nbsp;MMIO read `ICR` (0x300) | 28095 | 49.0 µs |
| &nbsp;&nbsp;MMIO write `RDT` (0x210) | 486 | 70.2 µs |
| MSR_WRITE (interrupt EOI) | 23410 | 21.2 µs |
| INTERRUPT_WINDOW (interrupt injection) | 19184 | 33.2 µs |
| HLT | 923 | 9.2 ms, **57.7% of the time** |

Every MMIO access is one exit, which is why EPT_MISCONFIG and the MMIO
total match exactly. Per transmitted packet the guest paid about one `TDT`
exit and about 1.4 interrupts, each with an `ICR` read, an EOI and an
injection. TCG has no VM exits at all, which is why it was faster here.
TCP could not keep enough data in flight to hide the exit cost (the guest
sat in HLT more than half the time), so TX was bound by per packet latency.
RX suffered less because NAPI already handled about 20 packets per
interrupt.

### 3. TX doorbell batching with BQL

`vnic-core.c`:

- `vnic_tx()` reports each packet to BQL and writes `TDT` only when
  `__netdev_tx_sent_queue(txq, skb->len, netdev_xmit_more())` says so:
  when no more packets follow, or when the queue has been stopped.
- It stops the queue right after the ring fills, before that call, so the
  stop also forces the doorbell. The `NETDEV_TX_BUSY` path is kept as a
  fallback.
- The paths that queue nothing (ring full, `skb_linearize()` failure, DMA
  mapping failure) go through a new `flush:` label, which writes `TDT` when
  `!netdev_xmit_more()` or the queue is stopped. An earlier packet may have
  left its doorbell to this one, and no later packet would ring it.
- `vnic_tx_poll()` reports completed packets and bytes with
  `netdev_completed_queue()`; `vnic_tx_drain()` calls `netdev_reset_queue()`
  for the packets it frees without completing.

BQL is required for the batching, not optional: the qdisc only hands a
driver several packets at once (setting `xmit_more`) when BQL leaves room
for them, and without BQL that room is 0.

Development measurement, `perf kvm stat` over 10 s of guest TCP TX, before
and after this change in the same session:

| | before | after |
|---|---|---|
| `TDT` writes | 18808 | 6296 |
| packets per `TDT` write | ~1.1 | **~50** |

Correctness: BQL's `inflight` must return to 0 once traffic stops, or BQL
would eventually stop the queue for good; `test-loopback.sh` checks it
after `/raw_flood 5000`, which also overfills the 256 entry ring and so
exercises the stop and wake path.

### 4. ISR masking

`vnic_pci_isr()` now does what NAPI expects of an interrupt handler:

```c
if (napi_schedule_prep(&hal->priv->napi)) {
	vnic_hal_wr32(hal, VN_REG_IMC, VN_INT_TXDONE | VN_INT_RXAVAIL | VN_INT_RXEMPTY);
	__napi_schedule(&hal->priv->napi);
}
```

Causes raised while masked stay latched in `ICR`; the `IMS` write at the
end of `vnic_poll()` makes the device raise the MSI then, so none are lost.

Development measurement, before and after this change in the same session:
interrupts per second during guest TCP RX went from 1281 to **87**, during
guest TCP TX from 1950 to 554. During RX the guest now stays in NAPI
polling almost all the time.

### 5. TX drop accounting

`vnic_run()` (QEMU device) and `engine_run()` (engine backend) now count a
rejected TX frame in `STAT_TXDROP` instead of `STAT_RXDROP`, in both
loopback and tap mode: the frame is invalid before either mode handles it.
They also no longer count it in `STAT_TXPKTS`/`STAT_TXBYTES`;
`include/uapi/linux/if_link.h` defines `tx_packets` and `tx_dropped` as
disjoint. Its descriptor still gets `DD`, so the driver reclaims the skb.
`STAT_RXDROP` stays for frames from the backend that do not fit the guest's
RX buffer (`vnic_receive()`).

A correct driver never sends such a frame, so the `TXDROP` check lowers
`MAXFRM` behind the driver's back with `devmem` and sends 1042 byte frames
(`txdrop_check()` in `/init`):

```bash
devmem $((BAR + 0x20)) 32 500            # MAXFRM
ping -c 3 -W 1 -s 1000 10.1.0.2          # three 1042 byte frames, all rejected
ip link set vnic0 down; ip link set vnic0 up   # vnic_hw_config() restores MAXFRM
ping -c 3 -W 1 -s 1000 10.1.0.2
```

It passes when `tx_dropped` rises by 3, `rx_dropped` by 0 and `tx_bytes`
by less than the 3126 bytes the rejected frames would add, and when
`MAXFRM` reads 1518 again after down/up with the large ping answered.
Against the QEMU device of the previous commit the same frames showed up
as `rx_dropped=3 tx_drop=0`, in both modes.

### 6. Engine backend: kernel crash on reload

The first run of `test-loopback.sh` on the engine backend crashed the guest
in the third reload round:

```
BUG: kernel NULL pointer dereference, address: 0000000000000000
#PF: supervisor instruction fetch in kernel mode
RIP: 0010:0x0
Call Trace:
 tasklet_action_common.isra.0+0xfd/0x1e0
 ...
 run_ksoftirqd+0x3c/0x60
Kernel panic - not syncing: Fatal exception in interrupt
```

`RIP: 0x0` from `tasklet_action_common` means a tasklet ran with a NULL
callback. The engine's tasklet stands in for the interrupt, and
`vnic_open()` (unchanged since Phase 2) did this:

```
vnic_hw_config()
CTRL = TXEN | RXEN        open the TX/RX gate
vnic_rx_post()            writes RDT: wakes the engine kthread on another CPU,
                          whose engine_run() ended with tasklet_schedule()
vnic_hal_irq_request()    engine: tasklet_setup(), only now
```

`struct vnic_hal` comes from `kzalloc()`, so until `tasklet_setup()` the
callback is NULL. Whenever the engine thread won that race, the tasklet ran
with no callback. This order dates from Phase 2, which moved
`tasklet_setup()` into `vnic_hal_irq_request()`; Phase 1's reload loop on
the engine ran before that, and since Phase 2 the loop had only been run on
the PCI backend.

The PCI backend got away with the same order only because its device comes
out of reset with `IMS = 0`, so no MSI can arrive before `vnic_open()`
unmasks at the end. The engine had no such gate: `engine_run()` scheduled
the tasklet unconditionally, and the mask was only checked inside the
tasklet's handler. It also turned `IMS` on at module load, unlike the QEMU
device. The fix gives the engine the QEMU device's interrupt model:

- New `engine_irq_update()`, the counterpart of the device's
  `vnic_irq_update()`: schedules the tasklet only when `ICR & IMS` is
  nonzero. `vnic_hal_wr32()` calls it on `ICS` writes (a cause was raised)
  and on `IMS` writes (a pending cause was unmasked, so it fires now).
- `engine_run()` no longer schedules the tasklet itself, and
  `engine_intr_tasklet()` is now only the ISR, like `vnic_pci_isr()`: read
  and clear `ICR`, mask, schedule NAPI.
- `engine_create()` no longer sets `IMS`, so the engine powers on with
  every register 0, like the QEMU device.
- `vnic_open()` calls `vnic_hal_irq_request()` before it opens the gate, so
  on either backend the handler exists before the device can raise
  anything. Its error path now also closes the gate, quiesces the backend
  and frees the interrupt.

Verification: `test-loopback.sh` on the engine backend five times in a row,
10/10 each time (30 open/close cycles); the PCI backend's
`test-loopback.sh` and `test-tap.sh` still pass (step 8).

### 7. Benchmark results

KVM, MTU 1500, same session, `bench-tap.sh` with the driver before Phase 6
(`REV=07ab85f`) and the final driver, five runs of 10 s each, medians:

| Test | before Phase 6 | after Phase 6 | |
|---|---|---|---|
| TCP guest TX | 16 Mbit/s | **414 Mbit/s** | ×26 |
| &nbsp;&nbsp;interrupts/s | 1856 | 202 | |
| TCP guest RX | 262 Mbit/s | **434 Mbit/s** | ×1.7 |
| &nbsp;&nbsp;interrupts/s | 1025 | 5 | |
| 64 byte UDP guest TX, 4 streams | 3318 pkt/s | **8984 pkt/s** | ×2.7 |
| 64 byte UDP guest RX, highest rate under 1% loss | 1000 pkt/s | 1000 pkt/s | |
| &nbsp;&nbsp;drops in `vnic0` (driver/device), any rate | 0 | 0 | |

64 byte UDP guest TX with one stream does not change: one sender thread
finds the qdisc empty on every `sendmsg()`, so each packet reaches the
driver alone and `xmit_more` is never set. With four streams packets queue
up and the batching applies, which is the ×2.7 above. Because this metric
varies a lot between runs, it was measured interleaved, three rounds of
before / after, three runs each: medians 2050 and 1960 pkt/s.

64 byte UDP guest RX loses datagrams at the guest's socket (`RcvbufErrors`)
before anywhere else: the iperf3 server cannot read faster than about 1000
per second in this VM. `vnic0` dropped none at any rate, before or after;
at 50000 per second and above the host's tap0 queue also overflows, less
so after Phase 6 (median 14179 against 145060 at 50000 per second).

Guest TCP RX by MTU, final driver, same session:

| MTU | 1500 | 4000 | 9000 |
|---|---|---|---|
| TCP guest RX | 434 Mbit/s | 585 Mbit/s | 781 Mbit/s |
| average frame | 1513 B | 3978 B | 8838 B |

Larger frames spread the per packet cost over more bytes. The average frame
size, from the guest's `/proc/net/dev`, confirms each MTU took effect.

### 8. Final functional results

| Script | Backend | Result |
|---|---|---|
| `test-tap.sh` | PCI | 8/8 |
| `test-loopback.sh` | PCI | 11/11 |
| `test-loopback.sh` | engine | 10/10, `TXDROP` skipped (no BAR0), five runs in a row |

## Conclusion

Under nested KVM a VM exit costs tens of microseconds, and the driver paid
several per packet. Batching the TX doorbell with BQL and masking
interrupts in the ISR the way NAPI requires took guest TCP TX from 16 to
414 Mbit/s and TCP RX from 262 to 434 Mbit/s, with the RX interrupt rate
down from about 1000 to 5 per second. Rejected TX frames are now reported
as TX drops only. Every test and measurement is now a script that checks its
environment first, and the new loopback script found and pinned down a
reload crash in the engine backend, fixed by giving it the same interrupt
model as the QEMU device.
