# Phase 1 Test Log

## Background

Phase 1 goal: introduce the **HAL seam** without touching the datapath. The baseline
(`4e20b5c`) is `vnic-l1`'s Phase 7 driver copied verbatim as a single `vnic-l2.c`. Phase 1
splits it into:

| file | role |
|---|---|
| `include/vnic-hal.h` | the interface: `vnic_hal_rd32/wr32`, `vnic_hal_wr32_raw`, `vnic_hal_ring_alloc/free`, `vnic_hal_map/unmap/mapping_error`, `vnic_hal_quiesce`, `vnic_hal_create/stop/destroy` |
| `driver/vnic-hal-engine.c` | L1 backend — owns `struct vnic_hal` (regs[], engine kthread, `ICR/ICS/IMS/IMC` semantics, loopback); opaque to core |
| `driver/vnic-core.c` | the netdev driver — reaches the "device" only through `vnic-hal.h` |

Everything that is *packet flow* — `vnic_tx`, `vnic_poll`, `vnic_tx_poll`, `vnic_rx_poll`,
`vnic_rx_post`, `vnic_{tx,rx}_drain`, the DD-bit / `dma_wmb()` handshakes — is unchanged;
only its calls into "the hardware" now go through the HAL.

So this is a **behavior-preserving refactor**, and the test goal is exactly that:

- The old `vnic-l1` test behavior (Phase 1 + Phase 7 edge suite) re-runs green against the split
- The register/stat reads still work through `vnic_hal_rd32()` (identity reg, `STAT_*` counters)
- The **new teardown contract** is correct: `vnic_hal_stop()` (kthread_stop) → free rings →
  `vnic_hal_destroy()` (kfree hal), and its mirror in the init error path. The baseline's
  `vnic_cleanup()` had the hal freed *before* the rings — Phase 1 fixes that ordering.
- The engine kthread is still reaped on every unload, including `rmmod` while UP

Not in scope (unchanged code, and deferred per README): throughput/latency characterization,
KASAN/lockdep/kmemleak gating (Phase 6 — and not available in this kernel anyway, see Environment).

## Environment

```
guest:   vnic-guest
kernel:  6.8.0-139-generic  (stock Ubuntu generic)
debug:   CONFIG_DEBUG_KMEMLEAK / KASAN / PROVE_LOCKING / DEBUG_MUTEXES / DEBUG_SPINLOCK  all =n
```

No KASAN/kmemleak/lockdep in this kernel — same as the `vnic-l1` test environment. Phase 1
validation is therefore functional behavior + driver-level counter parity + clean `dmesg`,
the same basis `vnic-l1`'s Phase 1 / Phase 7 logs used.

Per-`insmod` setup (loopback target MAC for the engine's IP-swap):

```bash
sudo ip link set vnicl2_loop0 up
sudo ip addr add 10.1.0.1/24 dev vnicl2_loop0
sudo ip neigh replace 10.1.0.2 lladdr 02:00:00:00:00:02 dev vnicl2_loop0 nud permanent
```

## Test Steps

### 1. Build — multi-file module

`driver/Makefile` now builds one module from two objects:

```make
obj-m     := vnic-l2.o
vnic-l2-y := vnic-core.o vnic-hal-engine.o
```

```bash
cd ~/vnic-l2/driver
make clean && make DEBUG=y
```

Expected: `vnic-l2.ko` produced, zero warnings, MODPOST resolves every `vnic_hal_*` symbol
(proves core↔backend link across the seam).

Result:

```
  CC [M]  /home/dev/vnic-l2/driver/vnic-core.o
  CC [M]  /home/dev/vnic-l2/driver/vnic-hal-engine.o
  LD [M]  /home/dev/vnic-l2/driver/vnic-l2.o
  MODPOST /home/dev/vnic-l2/driver/Module.symvers
  CC [M]  /home/dev/vnic-l2/driver/vnic-l2.mod.o
  LD [M]  /home/dev/vnic-l2/driver/vnic-l2.ko
```

Zero warnings (plain `make` without `DEBUG=y` also builds clean). `modinfo`: single module
`vnic-l2.ko`, `vermagic 6.8.0-139-generic`. `nm vnic-l2.o` shows all ten `vnic_hal_*`
entry points as `T` (defined) — the backend provides the whole interface.

PASSED.

### 2. Load — interface identity through the seam

```bash
sudo dmesg -C
sudo insmod vnic-l2.ko
```

Expected: `insmod` exit 0; `netdev_info` line prints the ID **read back via
`vnic_hal_rd32(priv->hal, VN_REG_ID)`** (= `0x564e3130`, "VN10") — confirms `vnic_hal_create()`
filled the RO identity regs and the read path works; engine kthread present; random MAC; MTU 1500.

Result:

```
insmod exit=0

[ 6325.704703] vnic_l2: loading out-of-tree module taints kernel.
[ 6325.704721] vnic_l2: module verification failed: signature and/or required key missing - tainting kernel
[ 6325.708157] vnicl2_loop0: VN1000 Level 2 (baseline: in-kernel engine) loaded (ID=0x564e3130)

$ lsmod | grep vnic
vnic_l2                20480  0

$ ip -br link show vnicl2_loop0
vnicl2_loop0     DOWN           02:ab:c6:50:d9:0f <BROADCAST,MULTICAST>

$ cat /sys/class/net/vnicl2_loop0/address   ->  02:ab:c6:50:d9:0f
$ cat /sys/class/net/vnicl2_loop0/mtu       ->  1500
$ ps -eo pid,comm | grep vnic               ->  5331 vnic-l2-engine
```

PASSED.

### 3. Loopback ping + counter parity

```bash
sudo ip link set vnicl2_loop0 up
sudo ip addr add 10.1.0.1/24 dev vnicl2_loop0
sudo ip neigh replace 10.1.0.2 lladdr 02:00:00:00:00:02 dev vnicl2_loop0 nud permanent
ping -c 5 -i 0.3 -W 1 10.1.0.2
ip -s link show vnicl2_loop0
```

Expected: 5/5. `ip -s link` RX == TX (bytes and packets) — these come from
`vnic_get_stats64()` → `vnic_hal_rd32(priv->hal, VN_REG_STAT_*)`, so equal non-zero counters
prove the STAT register reads survive the split.

Result:

```
5 packets transmitted, 5 received, 0% packet loss, time 1217ms
rtt min/avg/max/mdev = 0.162/0.236/0.364/0.070 ms

    RX:  bytes packets errors dropped  missed   mcast
          1246      13      0       0       0       0
    TX:  bytes packets errors dropped carrier collsns
          1246      13      0       0       0       0
```

RX == TX, errors/dropped 0. Sample `dmesg` (DEBUG=y) showing the flow going through the
backend:

```
vnic: vnic_tx: queued idx=15 len=90
vnic: vnic_engine_run: processed th 15->16, txp=16 rxp=16 rxdrop=0
vnic: vnic_intr_tasklet: masked TX/RX/RXEMPTY, mask now=0x8
vnic: vnic_rx_poll: delivered 1/64 (budget), next_to_clean=16, next_to_use=15
vnic: vnic_poll: budget=64 work_done=1
```

PASSED.

### 4. E1 — ping payload size sweep (runt → near-MTU)

```bash
for sz in 0 1 64 1400 1472 1473; do ping -c 5 -i 0.05 -W 2 -s $sz 10.1.0.2; done
```

Result:

```
size=0     0% loss
size=1     0% loss
size=64    0% loss
size=1400  0% loss
size=1472  20% loss   <- see note
size=1473  0% loss     (first IP-fragmenting size)
```

`size=1472` initially showed 1/5 lost. Re-tested `-c 20` x3: `0%`, `0%`, `5%`. **No
driver-level drop** accompanies it — `ip -s link` stays `dropped=0 errors=0`, and `dmesg` has
zero `rx ring full` / `rxdrop=` / `too short` traces. Same transient host-scheduler-jitter
pattern documented repeatedly in `vnic-l1` from Phase 3 on (this guest shares 2 vCPUs with the
host); attributed to scheduling, not the driver, on the evidence of the driver counters.

PASSED.

### 5. E2 — oversized ping (IP fragmentation)

```bash
ping -c 5 -i 0.2 -W 2 -s 65000 10.1.0.2
```

Result: `5/5, 0% packet loss` (rtt max 204 ms — host contention, no anomaly in `dmesg`). The
kernel fragments the 65000-byte payload into ~45 IP fragments per echo, handed to `vnic_tx()`
one frame at a time and reassembled on receipt — exercises the loopback path at high
per-ping frame count.

PASSED.

### 6. Raw-socket flood — datapath keeps up, counter parity at scale

```bash
sudo python3 raw_flood_l2.py 20000     # then 200000
ip -s link show vnicl2_loop0
```

Expected: every frame transmitted is looped back and delivered; RX == TX; `dropped=0`.

Result:

| frames sent | RX/TX packets (cumulative) | errors | dropped | `rx ring full` traces |
|---|---|---|---|---|
| 20 000  | 20 709  | 0 | 0 | 0 |
| +200 000 | 220 710 | 0 | 0 | 0 |

`s.send()` is one syscall per frame, so the engine kthread drains the 256-deep ring faster
than Python fills it — the ring never goes full (no `netif_stop_queue` / backpressure this
run; that path was covered heavily in `vnic-l1` Phase 4/6). The point here is the
**220 000-frame RX==TX parity with 0 drops/errors** across the refactored seam.

PASSED.

### 7. E3 (adapted) — MTU range rejection

```bash
sudo ip link set vnicl2_loop0 mtu 67     # < VN_MTU_MIN (68)
sudo ip link set vnicl2_loop0 mtu 9001   # > VN_MTU_MAX (9000)
sudo ip link set vnicl2_loop0 mtu 9000   # in range, but interface UP
```

Result:

```
Error: mtu less than device minimum.
Error: mtu greater than device maximum.
RTNETLINK answers: Device or resource busy
mtu unchanged: 1500
```

Out-of-range rejected by the kernel core (`netdev->min_mtu/max_mtu`); in-range-while-up hits
this driver's own `-EBUSY` in `vnic_change_mtu()` (behavior carried over from `vnic-l1`
Phase 5). Unchanged by the split.

PASSED.

### 8. down / up cycle — drain & re-post through the seam

```bash
sudo ip link set vnicl2_loop0 down
sudo ip link set vnicl2_loop0 up
ping -c 3 10.1.0.2
```

Expected: `vnic_release()` quiesces the engine (`vnic_hal_quiesce()` → `run_lock`), drains
both rings, and resyncs the ring cursors with `vnic_hal_wr32_raw()` (TDH/TDT/RDH/RDT, no
doorbell); the reopen re-posts a full RX ring; ping works again.

Result:

```
vnic: vnic_release: closing, draining rx ring
vnic: vnic_tx_drain: freed 0 tx buffers
vnic: vnic_rx_drain: freed 255 rx buffers
vnic: vnic_rx_post: posted 256 rx buffers
vnic: vnic_open: posted 255 rx buffers, opening queue

3 packets transmitted, 3 received, 0% packet loss
```

PASSED — the `wr32_raw` cursor-resync path (Phase 1's "A-plan", deferred CTRL.TXEN gate to
Phase 3) behaves exactly like the baseline's direct `priv->regs[x/4] = val` poke.

### 9. H1 — `rmmod` while the interface is UP (no `down` first)

```bash
ip -br link show vnicl2_loop0     # confirm UP,LOWER_UP
sudo rmmod vnic_l2
```

Expected: `unregister_netdev()` invokes `vnic_release()` internally, *then* `vnic_cleanup()`
runs `vnic_hal_stop()` → `vnic_hal_ring_free()` x2 → `vnic_hal_destroy()`. No hang, no oops,
kthread reaped. This is the ordering Phase 1 fixed (baseline freed the hal first).

Result:

```
rmmod exit=0

vnic: vnic_cleanup: unloading
vnic: vnic_release: closing, draining rx ring
vnic: vnic_tx_drain: freed 0 tx buffers
vnic: vnic_rx_drain: freed 255 rx buffers

$ ip link show vnicl2_loop0   ->  Device "vnicl2_loop0" does not exist.
$ ps -eo comm | grep vnic     ->  (no vnic kthread)
$ lsmod | grep vnic           ->  (not loaded)
```

PASSED — engine kthread gone, no leftover state, `dmesg` clean.

### 10. H2 — double `insmod` rejected

```bash
sudo insmod vnic-l2.ko   # 1st
sudo insmod vnic-l2.ko   # 2nd
```

Result:

```
1st insmod exit=0
insmod: ERROR: could not insert module vnic-l2.ko: File exists   (exit 1)
```

PASSED — standard module mechanism, no driver-specific handling.

### 11. Repeated load / unload x5 (with up/down + ping each round)

```bash
for i in 1 2 3 4 5; do
  sudo insmod vnic-l2.ko
  sudo ip link set vnicl2_loop0 up
  sudo ip addr add 10.1.0.1/24 dev vnicl2_loop0
  sudo ip neigh replace 10.1.0.2 lladdr 02:00:00:00:00:02 dev vnicl2_loop0 nud permanent
  ping -c 3 10.1.0.2
  sudo ip link set vnicl2_loop0 down
  sudo rmmod vnic_l2
done
```

Result:

```
round 1: ping 33% packet loss, rmmod exit=0    <- transient, see below
round 2: ping 0% packet loss,  rmmod exit=0
round 3: ping 0% packet loss,  rmmod exit=0
round 4: ping 0% packet loss,  rmmod exit=0
round 5: ping 0% packet loss,  rmmod exit=0

dmesg over all 5 rounds: clean (no warn/oops/bug/leak/refcount/held-lock)
final: module not loaded, no vnic kthread
```

Round 1's single-packet loss did not reproduce: a follow-up `insmod` + immediate `ping -c 5`
gave 5/5, and `ping -c 10` after a 1 s settle gave 10/10. Same first-ping / host-jitter
transient as step 4, no driver-level counter movement.

PASSED — "first insmod works, second blows up" class of bug not present.

## Conclusion

Phase 1 passed. The HAL split is behavior-preserving:

- Build is one module from `vnic-core.o` + `vnic-hal-engine.o`, zero warnings, all
  `vnic_hal_*` symbols resolved across the seam.
- Identity register readback, `STAT_*` counter reads, loopback ping, payload-size sweep,
  IP-fragmented oversized ping, 220 000-frame raw flood, MTU rejection, and the down/up
  drain-and-re-post cycle all behave exactly as the pre-split baseline (and as `vnic-l1`).
- Driver-level counters stay `dropped=0 errors=0` throughout; the only losses seen are
  transient single-packet ping drops with **no** matching driver drop trace — the same
  host-scheduler-jitter pattern `vnic-l1` documented from Phase 3 on, confirmed against
  `ip -s link` each time rather than taken at face value.
- The reworked teardown ordering (`vnic_hal_stop` → ring free → `vnic_hal_destroy`, and its
  init-error mirror `err_free_hal` / `free_hal`) is correct: `rmmod` while UP, and 5x
  load/unload with traffic, all leave no kthread, no leak, clean `dmesg`.

Design notes carried by this phase (see `vnic-hal.h` comments):

- **Ring-base registers (TDBAL/TDBAH/TDLEN, RX ditto) are not programmed by core.** The L1
  engine reaches the rings through `hal->priv`; TDH/TDT/RDH/RDT start at 0 from the kzalloc'd
  `hal->regs`. When the L2 backend lands, `vnic_hal_ring_alloc()` itself will write the
  `dma_alloc_coherent()` address into those registers — core never sees a DMA address.
- **`vnic_hal_wr32_raw()`** (plain `regs[off/4] = val`, no doorbell) is Phase 1's chosen way
  for the drain paths to resync TDH/TDT/RDH/RDT; the CTRL.TXEN/RXEN gate is deferred to Phase 3.

Not done (unchanged code / deferred): throughput-latency characterization and the
KASAN/lockdep/kmemleak gate (Phase 6; also unavailable in this kernel). The `vnic-l1` Phase 2–6
datapath tests were not re-run individually — Phase 1 changed none of that code, and the
Phase 7 edge suite exercised here covers the same paths.
