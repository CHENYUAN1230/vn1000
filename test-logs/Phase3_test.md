# Phase 3 Test Log

## Background

Phase 3 goal: replace Phase 2's "device reachable but no datapath" with a
real DMA TX/RX path:

- `dma_alloc_coherent` for the TX/RX descriptor rings (`vnic_hal_ring_alloc`)
- `dma_map_single` per packet buffer (`vnic_hal_map`/`unmap`/`mapping_error`)
- the QEMU device side (`qemu-device/vnic.c`) doing the IP-swap loopback for
  real, over `pci_dma_read`/`pci_dma_write`, instead of Phase 1's in-kernel
  `memcpy`
- a real MSI ISR calling `napi_schedule()` (Phase 2's ISR only set a flag)
- `register_netdev()`, and re-running the Phase 1 test suite against the
  real PCI backend

Two things came up during the work that weren't in the original plan, both
driven by the same realization (writing `TDT`/`RDT` always rings the
doorbell on real MMIO, since there is no driver-side "raw" write the way the
engine backend has):

- a `CTRL.TXEN`/`RXEN` gate, checked by `vnic_run()` (device side) and by
  `engine_run()` (engine side), so the drain paths' register resets can't
  accidentally trigger a real pass mid-teardown
- `vnic_hal_wr32_raw()` removed entirely once the gate made it redundant
  for both backends

A third, `VN_CTRL_RST`, came up during testing itself (step 7) rather than
during implementation; see that step for the full story.

## Environment

```
guest:   same nested-VM harness as Phase 2 (nested-vm/), busybox init
host kernel: 6.8.0-142-generic (bumped from Phase 2's 139 by a host update;
             nested-vm/vmlinuz re-copied to match: a module built against
             one vermagic will not insmod under the other)
QEMU:    same self-built 9.1.3 at ~/qemu, hw/net/vnic.c re-synced from
         qemu-device/vnic.c before every build in this log
driver:  built with VNIC_BACKEND=pci unless noted; VNIC_BACKEND=engine
         (default) used only for the Phase 1 regression check in step 5
```

`arp -s <ip> <mac>` was used instead of Phase 1's `ip neigh replace ... nud
permanent`: this busybox build's `ip neigh` only supports `show`/`flush`, no
`add`/`replace`. `arp -s` (no `temp` flag) installs the same kind of
permanent static entry and ping's loopback trick doesn't care which tool set
it.

## Test Steps

### 1. Build: QEMU device model + both driver backends

```bash
cp qemu-device/vnic.c ~/qemu/hw/net/vnic.c
ninja -C ~/qemu/build qemu-system-x86_64
cd driver && make clean && make                    # engine
make clean && make VNIC_BACKEND=pci                 # pci
```

Expected: all three build clean. `hw/net/` builds with `-Werror`, so a
bounds/type mistake in `vnic.c` fails the build outright (as in Phase 2).

Result: all three built with zero warnings.

PASSED.

### 2. PCI enumeration + probe

```bash
TEST=1 ./nested-vm/run.sh -device vnic
```

Expected: `1234:11e9` enumerates, `vnic_pci_probe()` reads the ID magic,
allocates both rings, and `register_netdev()` succeeds. The Phase 2
self-test is gone (removed this phase; see Background), so success now
shows up as a netdev, not a "self-test passed" line.

Result:

```
0000:00:03.0  0x1234:0x11e9  class 0x020000
vnic 0000:00:03.0: VN1000 found over MMIO, ID=0x564e3130
vnic0: VN1000 loaded, real PCI/MMIO/MSI backend (ID=0x564e3130)
insmod OK
```

PASSED.

### 3. Loopback ping + counter parity (PCI backend)

```bash
ip link set vnic0 up
ip addr add 10.1.0.1/24 dev vnic0
arp -s 10.1.0.2 02:00:00:00:00:02
ping -c 5 -i 0.3 -W 1 10.1.0.2
cat /proc/net/dev
```

Expected: 5/5, `/proc/net/dev` RX == TX (bytes and packets), matching Phase
1's test 3 against the engine backend.

This did not pass on the first two attempts; both failures and fixes are
part of this test's result, not separate bugs found "outside" testing.

**Attempt 1: 100% loss, RX drop count == TX packet count exactly**

```
vnic0:       0       0    0    9 ...     810       9    0    0 ...
```

Every TX frame counted as an RX drop. Traced to `vnic_run()`'s new
`txd.len > s->regs[VN_REG_MAXFRM / 4]` bound (added this phase, to stop a
guest-controlled `MAXFRM` write from being trusted on its own; see
`vnic_run()`'s doc comment): `vnic_realize()` never initialized
`VN_REG_MAXFRM`, so it read back as 0, and any real frame length exceeded
it. The engine backend has the identical gap (`grep` for `VN_REG_MAXFRM` in
`vnic-engine.c` finds nothing) but never tripped over it, because Phase 1's
`engine_run()` has no upper-bound check at all; this phase's new check was
the first thing to ever read the register.

Fix: `vnic_realize()` now sets `s->regs[VN_REG_MAXFRM / 4] = VN_MAXFRM_DEFAULT`
alongside the other RO/default registers.

**Attempt 2: driver counters parity, ping still 100% loss**

```
vnic0:    1006      11    0    0 ...     1006      11    0    0 ...
```

RX == TX, 0 drops, `dmesg` showed `vnic_rx_poll: delivered` for every frame:
the DMA/loopback path itself was working. `ping` still saw nothing.
`/proc/net/snmp` narrowed it down:

```
Ip: ... InReceives InHdrErrors ... InDelivers ...
Ip: ...         2           2  ...          0 ...
```

2 pings, 2 `InHdrErrors`, 0 delivered to ICMP: the IP layer received the
frames and rejected them as malformed. Root cause: `skb_put()`, which
marks how much of the skb's buffer is real data, was never called in the
shared `vnic_rx_poll()`. It only existed in the engine backend's own
`engine_run()` (which can call it directly, being in-kernel C code); the
PCI backend's "device" is QEMU, which cannot touch an `sk_buff` at all, so
every RX skb reached `eth_type_trans()` with `skb->len` still 0: a
zero-length IP header reads as `InHdrErrors`, not a driver-side drop, so it
never showed up in step "Attempt 2"'s counters.

Fix: moved `skb_put(skb, rx_ring->desc[ntc].len)` into `vnic_rx_poll()`
(shared), removed the now-duplicate call from `engine_run()`, and changed
`engine_flip_addr()` to take the frame length as a parameter instead of
reading `skb->len` (which isn't valid yet at the point `engine_run()` calls
it: `skb_put()` now happens afterward, in the shared code).

**Attempt 3: result**

```
PING 10.1.0.2 (10.1.0.2): 56 data bytes
64 bytes from 10.1.0.2: seq=0 ttl=64 time=10.386 ms
64 bytes from 10.1.0.2: seq=1 ttl=64 time=1.696 ms
64 bytes from 10.1.0.2: seq=2 ttl=64 time=2.453 ms
64 bytes from 10.1.0.2: seq=3 ttl=64 time=0.990 ms
64 bytes from 10.1.0.2: seq=4 ttl=64 time=2.503 ms

5 packets transmitted, 5 packets received, 0% packet loss

vnic0:    1496      16    0    0 ...     1496      16    0    0 ...
```

5/5, RX == TX, 0 drops.

PASSED (after two fixes; see above).

### 4. CTRL gate sanity: debug build, dmesg trace

Same run as step 3 (attempt 3), built with `DEBUG=y`:

```
vnic: vnic_rx_post: posted 256 rx buffers
vnic: vnic_open: posted 255 rx buffers, opening queue
vnic: vnic_tx: queued idx=0 len=90
vnic: vnic_rx_poll: dest_mac=da:e5:ee:4c:88:4f dev_addr=da:e5:ee:4c:88:4f
vnic: vnic_rx_poll: eth_type_trans pkt_type=0
vnic: vnic_rx_poll: delivered 1/64 (budget), next_to_clean=1, next_to_use=0
vnic: vnic_poll: budget=64 work_done=1
```

`pkt_type=0` is `PACKET_HOST` on every delivered frame: `vnic_loopback()`'s
MAC swap (device side) correctly turns the frame's destination back into
this host's own address before `eth_type_trans()` ever sees it. This was
checked explicitly while diagnosing attempt 2, to rule out a `PACKET_HOST`
vs `PACKET_OTHERHOST` misclassification before `/proc/net/snmp` pointed at
`skb_put()` instead.

PASSED.

### 5. E1: ping payload size sweep (runt to near-MTU)

```bash
for sz in 0 1 64 1400 1472 1473; do ping -c 5 -i 0.05 -W 2 -s $sz 10.1.0.2; done
cat /proc/net/dev
```

Expected: 0% loss at every size, matching Phase 1's test 4 against the
engine backend; 1473 is the first size that forces IP fragmentation
(56-byte ping overhead pushes the frame past the 1500 MTU), so this also
covers multi-fragment frames through the loopback for the first time.

Result: 0% loss at every size (0, 1, 64, 1400, 1472, 1473).
`/proc/net/dev` after the sweep: `vnic0: 47546 77 0 0 ... 47546 77 0 0 ...`,
RX == TX, 0 drops.

PASSED.

### 6. E2 / E3: oversized (fragmented) ping, MTU range rejection

```bash
ping -c 5 -i 0.2 -W 2 -s 65000 10.1.0.2
cat /proc/net/dev

ip link set vnic0 mtu 67      # < VN_MTU_MIN (68)
ip link set vnic0 mtu 9001    # > VN_MTU_MAX (9000)
ip link set vnic0 mtu 9000    # in range, but interface is UP
ip link set vnic0 down
ip link set vnic0 mtu 9000    # in range, DOWN, should now succeed
ip link set vnic0 mtu 1500
ip link set vnic0 up
```

Expected: the 65000-byte ping (many IP fragments per echo) comes back 0%
loss with RX == TX; the three MTU attempts are rejected / accepted exactly
per `netdev->min_mtu`/`max_mtu` and `vnic_change_mtu()`'s own
`netif_running()` guard (matching Phase 1's test 7, adapted: Phase 1 only
exercised the generic `min_mtu`/`max_mtu` rejection, this backend adds the
"in range but UP" case since `vnic_change_mtu()` is new shared-code logic
being exercised against this backend for the first time).

Result:

```
5 packets transmitted, 5 packets received, 0% packet loss
vnic0:  665626     447    0    0 ...   665626     447    0    0 ...

[   11.449886] vnic0: mtu less than device minimum
ip: SIOCSIFMTU: Invalid argument
[   11.756342] vnic0: mtu greater than device maximum
ip: SIOCSIFMTU: Invalid argument
ip: SIOCSIFMTU: Device or resource busy          <- in range, still UP
[after down] mtu 9000 accepted, `ip link show` confirms it
[mtu 1500 + up] accepted
```

All three MTU cases behaved exactly as expected; the oversized/fragmented
ping had 0% loss with RX == TX. `vnic_change_mtu()`'s `-EBUSY` while running
and `vnic_hal_wr32(..., VN_REG_MAXFRM, ...)` sync both confirmed working
against the real device for the first time here.

PASSED.

### 7. Down/up cycle: drain & re-post through the seam

```bash
ping -c 5 -i 0.2 -W 1 10.1.0.2      # baseline, passes
ip link set vnic0 down
ip link set vnic0 up
ping -c 5 -i 0.3 -W 1 10.1.0.2
cat /proc/net/dev
```

Expected (Phase 1's test 8): ping works again after the cycle, same as
before it.

This did not pass on the first attempt, and the investigation turned up a
real bug independent of the ones in step 3.

**Attempt 1: driver counters show nothing even queued on the device side**

Second round: 9 TX frames queued, `/proc/net/dev` shows RX == TX (so no
driver-side drop), but **zero** `vnic_rx_poll: delivered` lines in `dmesg`
for any of them: a different failure shape from step 3's attempts (there,
delivery happened but was still rejected upstream; here, delivery never
happens at all).

Root cause: `TDH`/`RDH` are read-only to the guest (`vnic_mmio_write()`
ignores writes to them; see that function's switch). `vnic_tx_drain()`/
`vnic_rx_drain()` have always tried to zero them on close, but that write
has never actually done anything on the PCI backend; nothing reset the
*device's* internal `th`/`rh` cursors, so after the second `up` the device
was still reading from wherever it left off before the first `down`, out of
sync with the driver's freshly-reset software cursors and freshly-reposted
ring contents. The engine backend never had this gap: its `vnic_hal_wr32()`
writes `TDH`/`RDH` with no RO restriction at all, so the same drain call
really did reset its cursors.

Fix: wired up `VN_CTRL_RST` (defined in `vnic_regs.h` since Phase 1, never
implemented on either side). `vnic_release()` now writes
`CTRL = VN_CTRL_RST` instead of `CTRL = 0`, with the same gate-closing
effect (`TXEN`/`RXEN` are 0 in that value too), plus the backend zeroes `TDH`/`RDH`
and self-clears the bit. Implemented identically in both backends
(`vnic_hal_wr32()` in `vnic-engine.c`, `vnic_mmio_write()` in
`qemu-device/vnic.c`) rather than only on the PCI side, so the two no longer
reach the same result through two different mechanisms. `vnic_tx_drain()`/
`vnic_rx_drain()`'s own `TDH`/`RDH` writes were removed as redundant.

**Attempt 2: fix confirmed at the driver level, ping still 100% loss**

With the fix built, `dmesg` for the second round showed the identical
healthy pattern as the first round: `queued` -> `dest_mac`/`pkt_type=0` ->
`delivered` -> `work_done=1` for every frame, but `ping` still reported 0
received. A length-42 frame (exactly an ARP frame's size) appeared in the
second round's TX trace that wasn't present in the first: `ip link down`
flushes the device's neighbour table, taking the static `arp -s` entry with
it. The guest was sending real ARP requests again, which this loopback
cannot resolve (it reflects the opcode unchanged, so a request comes back
as another request, never a reply), not a driver or device bug, but a gap
in the test's own command sequence.

Fix (to the test, not the code): re-run `arp -s 10.1.0.2 02:00:00:00:00:02`
after every `up`, confirmed missing via `arp -n` returning empty right
after the cycle.

**Attempt 3: result**

```bash
ping -c 5 -i 0.2 -W 1 10.1.0.2          # baseline
ip link set vnic0 down
ip link set vnic0 up
arp -s 10.1.0.2 02:00:00:00:00:02        # re-add, down flushed it
ping -c 5 -i 0.3 -W 1 10.1.0.2
```

```
5 packets transmitted, 5 packets received, 0% packet loss   [both rounds]
vnic0:    2600      28    0    0 ...     2600      28    0    0 ...
```

5/5 both before and after the cycle, RX == TX across the whole run.

PASSED (after one code fix and one test-methodology fix; see above).

### 8. Regression: engine backend still passes Phase 1's loopback ping

```bash
make clean && make           # VNIC_BACKEND=engine (default)
# same up/addr/arp/ping sequence as step 3, no -device vnic
```

Expected: the `skb_put()`/`engine_flip_addr()` changes in step 3 touched
shared code (`vnic_rx_poll()`) and an engine-backend-only function
(`engine_run()`, `engine_flip_addr()`); this needs to still behave
identically to Phase 1 for the engine backend, not just compile.

Result:

```
vnic0: VN1000 loaded, in-kernel engine backend (ID=0x564e3130)
5 packets transmitted, 5 packets received, 0% packet loss
vnic0:    1496      16    0    0 ...     1496      16    0    0 ...
```

Identical to the PCI backend's result in step 3. No regression.

PASSED.

## Conclusion

The PCI backend's full DMA datapath works end to end: ring allocation,
per-packet `dma_map_single`, the QEMU-side `pci_dma_read`/`pci_dma_write`
loopback, `napi_schedule()` from a real hardirq, and delivery through
`eth_type_trans()`/`napi_gro_receive()` all the way to a real `ping`
getting real replies, with driver-level RX/TX counter parity. The engine
backend (Phase 1's control group) still passes the same test unchanged.

Three real bugs were found and fixed in the process, all logged above as
part of the test rather than fixed quietly beforehand: `VN_REG_MAXFRM`
never had a device-side default; `skb_put()` was missing from the shared RX
path (silently masked for the engine backend by backend-specific code, with
no equivalent possible on the PCI side); and `TDH`/`RDH` were never truly
resettable on the PCI backend, because they are read-only to the guest and
nothing reset them from the device side until `VN_CTRL_RST` was wired up.
All three were latent in the design since Phase 1/2; the first two only
needed `vnic_run()`'s new bounds-checking and the PCI backend's real network
stack delivery to surface; the third needed an actual down/up cycle against
a real device, which nothing before this phase ever exercised.

Phase 1's suite now re-run against the PCI backend and passing: loopback
ping + counter parity (step 3), payload-size sweep (step 5),
oversized/fragmented ping and MTU range rejection (step 6), and the down/up
cycle (step 7). The last of these also turned up that `ip link down`
flushes the static neighbour entry, so the test sequence (not the driver)
needs `arp -s` re-applied after every `up`.

### 9. H1: rmmod while UP, no prior down

```bash
ip link set vnic0 up
ip addr add 10.1.0.1/24 dev vnic0
arp -s 10.1.0.2 02:00:00:00:00:02
ping -c 3 -i 0.2 -W 1 10.1.0.2    # baseline, 3/3
rmmod vnic
echo "rmmod rc=$?"
ip link show vnic0               # expect: device gone
dmesg | tail -20
```

Expected: `rmmod` on a module whose netdev is still UP should still succeed
cleanly, since `unregister_netdev()` calls `ndo_stop()` (`vnic_release()`)
internally before actually removing the device; no separate `down` should
be required.

Result:

```
3 packets transmitted, 3 packets received, 0% packet loss
rmmod rc=0
ip link show vnic0: Device "vnic0" does not exist.

vnic: vnic_release: closing, draining rx ring
vnic: vnic_tx_drain: freed 0 tx buffers
vnic: vnic_rx_drain: freed 255 rx buffers
```

Clean `rmmod`, no crash, no warning, `vnic_release()`/`vnic_tx_drain()`/
`vnic_rx_drain()` ran in the expected order even though no explicit `down`
was issued first.

PASSED.

### 10. H2: double insmod rejection

```bash
insmod /vnic.ko
ip link set vnic0 up
ip addr add 10.1.0.1/24 dev vnic0
arp -s 10.1.0.2 02:00:00:00:00:02
ping -c 3 -i 0.2 -W 1 10.1.0.2    # baseline, 3/3
insmod /vnic.ko                  # second insmod, same module
echo "insmod2 rc=$?"
ping -c 2 -i 0.2 -W 1 10.1.0.2    # confirm first instance still works
```

Expected: the second `insmod` is rejected by the kernel module loader
(the module is already live), and the first instance keeps working
unaffected.

Result:

```
insmod: can't insert '/vnic.ko': File exists
insmod2 rc=17
2 packets transmitted, 2 packets received, 0% packet loss
```

Second `insmod` rejected with `EEXIST` (`rc=17`), first instance unaffected.

PASSED.

### 11. Repeated load/unload x5

```bash
for i in 1 2 3 4 5; do
    insmod /vnic.ko
    ip link set vnic0 up
    ip addr add 10.1.0.1/24 dev vnic0
    arp -s 10.1.0.2 02:00:00:00:00:02
    ping -c 3 -i 0.2 -W 1 10.1.0.2
    ip link set vnic0 down
    rmmod vnic
    echo "round${i}_rmmod_rc=$?"
done
```

Expected: five clean cycles, 0% ping loss and `rmmod rc=0` every round, no
cumulative resource leak symptom (growing latency, failing allocation, etc).

Result: all five rounds passed, 3/3 ping every round, `rmmod rc=0` every
round:

```
round1_rmmod_rc=0
round2_rmmod_rc=0
round3_rmmod_rc=0
round4_rmmod_rc=0
round5_rmmod_rc=0
```

Ping latency stayed in the same low-millisecond range across all five
rounds, no sign of accumulating state between cycles.

PASSED.

### 12. Raw-socket flood (C rewrite)

`driver/raw_flood.py` needed Python3, which this busybox nested-VM image
does not have, so it was rewritten as `driver/raw_flood.c`: same
`AF_PACKET`/`SOCK_RAW` frame, bound to `vnic0`, sent in a tight loop, built
as a static binary (`gcc -static`) so it drops straight into the initramfs
with no interpreter or shared libraries needed. `build-initramfs.sh` now
copies it in alongside `busybox` and the module whenever it exists.

```bash
cd driver && make clean && make VNIC_BACKEND=pci && make raw_flood
cd ../nested-vm && ./build-initramfs.sh ../driver/vnic.ko
```

```bash
ip link set vnic0 up
ip addr add 10.1.0.1/24 dev vnic0
arp -s 10.1.0.2 02:00:00:00:00:02
/raw_flood 5000
echo "flood_rc=$?"
cat /proc/net/dev
ping -c 3 -i 0.2 -W 1 10.1.0.2    # confirm still healthy afterward
```

Expected: 5000 frames sent back to back at line rate (no `ping`-style
inter-packet delay), RX == TX with 0 drops on `/proc/net/dev`, no crash or
warning, and `ping` still works afterward.

Result:

```
flood_rc=0
vnic0:  300516    5006    0    0 ...   300516    5006    0    0 ...
3 packets transmitted, 3 packets received, 0% packet loss
```

5000/5000 sent, RX == TX, 0 drops (the extra 6 packets/bytes beyond 5000
are the gratuitous ARP announcement from `ip addr add` and similar
housekeeping traffic, counted identically on both sides). No crash or
warning in `dmesg`, `ping` still healthy afterward.

PASSED.

## Conclusion (updated)

With steps 9 to 12, all items from Phase 1's suite that were deferred
earlier in this log are now done against the PCI backend: `rmmod`-while-up
(H1), double-`insmod` rejection (H2), repeated load/unload x5, and the
raw-socket flood (rewritten in C, step 12). All passed cleanly. Nothing
from the original test plan remains deferred.
