# Phase 4 Test Log

## Background

Phase 4 goal: connect the QEMU device to a real host network, so frames
leave the nested VM and a real Linux network stack answers them, instead of
the device reflecting the guest's own traffic back.

- `NICConf conf` + `DEFINE_NIC_PROPERTIES`: `-device vnic` accepts
  `netdev=` and `mac=`
- `qemu_new_nic()` in `vnic_realize()`: the device becomes a net client
  peered with the `-netdev` backend (`qemu_del_nic()` in unrealize)
- a `loopback` bool property, default `on`: `on` keeps Phase 3's internal
  IP-swap loopback (so `-device vnic` behaves exactly as before), `off`
  forwards to the backend
- TX in backend mode: `vnic_run()` hands each frame to `qemu_send_packet()`
- RX in backend mode: `vnic_can_receive()` / `vnic_receive()` DMA host
  frames into the RX ring, and `qemu_flush_queued_packets()` on an `RDT`
  write or `CTRL.RXEN` turning on delivers frames the net layer queued
  while the guest could not take them

The driver is unchanged this phase: everything above is device side.

The RX ring logic that used to live inline in `vnic_run()` was first pulled
out into two helpers, `vnic_rx_has_space()` and `vnic_rx_deliver()`, so the
loopback path and the backend path share one copy (step 4). That refactor
changed one behavior on purpose: `RXEMPTY` is now raised when `RDH` reaches
`RDT` (no posted buffer). Phase 3 left that case silent and only raised
`RXEMPTY` when the descriptor at `RDH` still had `DD` set, but the datasheet
defines "no posted buffer" as the `RXEMPTY` condition. `engine_run()` in
`driver/vnic-engine.c` had the same deviation and was fixed the same way at
the end of this phase (step 7).

Two driver changes also went in at the end of this phase, both outside the
tap work itself: that `engine_run()` fix, and `dma_set_mask_and_coherent()`
in `vnic_pci_hal_create()` (step 8), which had been a TODO since Phase 3.

## Environment

```
guest:   same nested-VM harness as Phase 3 (nested-vm/), busybox init
host:    kernel 6.8.0-142-generic
QEMU:    same self-built 9.1.3 at ~/qemu (builds with -Werror); hw/net/vnic.c
         re-synced from qemu-device/vnic.c before every build in this log
driver:  VNIC_BACKEND=pci, unchanged from Phase 3
network: tap0 on the host, 10.1.0.2/24, owned by the user (no root QEMU);
         guest vnic0 is 10.1.0.1/24
```

The host rebooted twice during this phase, and a tap created with
`ip tuntap add` does not survive a reboot. `nested-vm/setup-tap.sh` was
added to recreate it:

```bash
./nested-vm/setup-tap.sh        # create tap0 (skips what already exists)
./nested-vm/setup-tap.sh del    # remove it
```

The standard command lines used below:

```bash
# loopback mode (Phase 3 behavior, no host network)
./nested-vm/run.sh -device vnic

# backend (tap) mode
./nested-vm/run.sh -netdev tap,id=n0,ifname=tap0,script=no,downscript=no \
                   -device vnic,loopback=off,netdev=n0
```

Guest commands were typed into the VM's serial console shell. For the
longer runs they were fed in from a script on the host with fixed delays
between them (same commands, no interaction needed); host side commands
ran in a second shell on the host while the VM was up.

## Test Steps

### 1. Build

```bash
cp qemu-device/vnic.c ~/qemu/hw/net/vnic.c
ninja -C ~/qemu/build
```

Expected: clean build after every step below (`-Werror` turns any warning,
including an unused static helper, into a failure).

Result: every build in this log had zero warnings.

PASSED.

### 2. tap backend opens without root

```bash
./nested-vm/setup-tap.sh
./nested-vm/run.sh -netdev tap,id=n0,ifname=tap0,script=no,downscript=no -device vnic
```

Expected: the VM boots (QEMU exits before boot if it cannot open the tap),
tap0 goes from `NO-CARRIER` to `UP`, and QEMU warns that nothing uses `n0`
yet.

Result:

```
qemu-system-x86_64: warning: netdev n0 has no peer
```

VM booted, tap0 `UP` with owner uid 1000. Without `setup-tap.sh` (after a
reboot) QEMU instead fails with
`could not configure /dev/net/tun (tap0): Operation not permitted`, since an
unprivileged QEMU cannot create the tap itself.

PASSED.

### 3. netdev= property and peering

Property parsing, before `qemu_new_nic()` existed:

```bash
./nested-vm/run.sh -netdev tap,id=n0,ifname=tap0,script=no,downscript=no -device vnic,netdev=n0
./nested-vm/run.sh -netdev tap,id=n0,ifname=tap0,script=no,downscript=no -device vnic,netdev=n9
```

Expected: `netdev=n0` is accepted (it previously failed with
`Property 'vnic.netdev' not found`); the `no peer` warning remains, since
nothing links the peers yet; a wrong id is rejected.

Result: `n0` accepted, VM boots, warning still printed. `n9`:

```
qemu-system-x86_64: -device vnic,netdev=n9: Property 'vnic.netdev' can't find value 'n9'
```

Peering, after `qemu_new_nic()` was added (with a stub `vnic_receive()`
that drops everything; it must exist, since the net layer calls `.receive`
without a NULL check):

```bash
./nested-vm/run.sh -netdev tap,id=n0,ifname=tap0,script=no,downscript=no -device vnic,netdev=n0
# QEMU monitor: Ctrl-a c, then
info network
```

Expected: no `no peer` warning, the monitor shows `n0` as the vnic's peer,
and frames the host sends into tap0 (its own IPv6 router solicitations etc.)
reach the stub without crashing QEMU.

Result:

```
vnic.0: index=0,type=nic,model=vnic,macaddr=52:54:00:12:34:56
 \ n0: index=0,type=tap,ifname=tap0,script=no,downscript=no
```

No warning, clean exit. tap0's `tx_packets` (host to guest) was 17 by the
end of the run, all delivered to the stub.

PASSED.

### 4. RX helper refactor: Phase 3 regression in loopback mode

```bash
./nested-vm/run.sh -device vnic
```

```bash
# in the guest
insmod /vnic.ko
ip link set vnic0 up
ip addr add 10.1.0.1/24 dev vnic0
arp -s 10.1.0.2 02:00:00:00:00:02
ping -c 5 -i 0.3 -W 1 10.1.0.2                                           # T3
cat /proc/net/dev
for sz in 0 1 64 1400 1472 1473; do ping -c 5 -i 0.05 -W 2 -s $sz 10.1.0.2; done   # T5
ping -c 5 -i 0.2 -W 2 -s 65000 10.1.0.2                                  # T6
ip link set vnic0 down; ip link set vnic0 up
arp -s 10.1.0.2 02:00:00:00:00:02
ping -c 5 -i 0.3 -W 1 10.1.0.2                                           # T7
/raw_flood 5000; echo "flood_rc=$?"                                      # T12
cat /proc/net/dev
ping -c 3 -i 0.2 -W 1 10.1.0.2
dmesg | grep -iE "warn|bug|call trace"
```

Test names follow `Phase3_test.md`. Expected: identical to Phase 3. T12 is
the one that matters most here: it fills the RX ring and exercises the
"no free buffer, leave TX pending, resume on RDT" path that now goes
through `vnic_rx_has_space()`.

Result:

```
T3   5 packets transmitted, 5 packets received, 0% packet loss
     vnic0:    1246      13    0    0 ...     1246      13    0    0 ...
T5   0% packet loss for every size
T6   5 packets transmitted, 5 packets received, 0% packet loss
T7   5 packets transmitted, 5 packets received, 0% packet loss
T12  flood_rc=0
     vnic0: 1015272    5546    0    0 ...  1015272    5546    0    0 ...
     3 packets transmitted, 3 packets received, 0% packet loss
```

RX == TX, 0 drops, no warnings in `dmesg`. The same suite was rerun with
identical results after step 5's change to `vnic_run()`.

PASSED.

### 5. Backend mode TX: guest frames reach the host

`vnic_run()` with `loopback=off` sends through `qemu_send_packet()`; RX is
still the step 3 stub, so only half the link works.

```bash
# host, second shell
sudo tcpdump -i tap0 -e -n not ip6

# host
./nested-vm/run.sh -netdev tap,id=n0,ifname=tap0,script=no,downscript=no \
                   -device vnic,loopback=off,netdev=n0

# guest
insmod /vnic.ko
ip link set vnic0 up
ip addr add 10.1.0.1/24 dev vnic0
cat /sys/class/net/vnic0/address
ping -c 3 -i 0.5 -W 1 10.1.0.2
cat /proc/net/dev
```

Expected: tcpdump shows the guest's ARP requests (source MAC = the guest's
vnic0) and the host's replies; the ping itself fails, because the replies
are dropped by the RX stub and the guest never resolves 10.1.0.2.

Result:

```
guest MAC ee:ca:99:0f:b1:30
ee:ca:99:0f:b1:30 > ff:ff:ff:ff:ff:ff, ARP, Request who-has 10.1.0.2 tell 10.1.0.1
e2:92:55:33:26:18 > ee:ca:99:0f:b1:30, ARP, Reply 10.1.0.2 is-at e2:92:55:33:26:18
(3 request / reply pairs)
3 packets transmitted, 0 packets received, 100% packet loss
vnic0:       0       0 ...      600       8 ...
```

Guest TX 8 frames, host tap0 `rx_packets` 0 to 10 (the 2 extra are IPv6
frames filtered out of the tcpdump above), guest RX 0.

PASSED.

### 6. Backend mode RX: bidirectional ping

```bash
# host, second shell
sudo tcpdump -i tap0 -n icmp or arp

# guest
insmod /vnic.ko
ip link set vnic0 up
ip addr add 10.1.0.1/24 dev vnic0
ping -c 5 -i 0.3 -W 1 10.1.0.2                                 # A
ip link set vnic0 down; sleep 1; ip link set vnic0 up
ping -c 5 -i 0.3 -W 1 10.1.0.2                                 # B

# host, while the guest is up
ping -c 5 -i 0.3 -W 1 10.1.0.1                                 # C
sudo ping -f -c 2000 -W 1 10.1.0.1                             # D
ping -c 3 -i 0.3 -W 1 -s 1472 10.1.0.1                         # E

# guest, at the end
cat /proc/net/dev
```

No `arp -s` this time: the host answers ARP for real. B exercises the flush
on `CTRL.RXEN` turning back on; D fills the 256-slot RX ring repeatedly.

Expected: 0% loss everywhere, no RX drops in the guest.

**Attempt 1: all pings pass, but the guest counts one RX drop**

```
A  5/5     B  5/5     C  5/5     D  2000/2000     E  3/3
vnic0:  202828    2029    0    1 ...
```

`rx_dropped` in `/proc/net/dev` is `STAT_RXDROP` from the device plus the
driver's own count. Traced to `vnic_receive()`'s "no free RX buffer"
branch, which counted a drop and returned `size`. The design assumed the
net layer always asks `vnic_can_receive()` before calling `.receive`, so
that branch would be unreachable. It is reachable: `qemu_net_queue_flush()`
(`net/queue.c`) delivers queued frames back to back without asking
`can_receive()`, and only stops when `.receive` returns 0 (which also sets
`receive_disabled` until the next flush, `net/net.c:849`). So a flush with
more queued frames than free RX slots dropped every frame past the last
free slot.

Fix: that branch now returns 0 (frame stays at the head of the queue) and
no longer counts a drop; it still raises `RXEMPTY`, so NAPI runs, refills,
and writes `RDT`, which triggers the next flush.

**Attempt 2: fix confirmed, plus a backlog larger than the ring**

Same as above, plus a new case F: the host keeps sending while the guest's
interface is down, so a backlog builds up in QEMU's queue, larger than the
RX ring.

```bash
# guest
cat /proc/net/dev
ip link set vnic0 down; sleep 10; ip link set vnic0 up; sleep 4
cat /proc/net/dev
ping -c 3 -i 0.3 -W 1 10.1.0.2

# host, during the guest's 10 s down window
sudo ping -f -c 600 -w 7 10.1.0.1                              # F
```

Result:

```
A  3/3     C  3/3     D  2000/2000
F  host: 636 packets transmitted, 0 received   (expected: its 7 s deadline
                                                ends before the guest is up)
   guest before down:  vnic0:  196888    2010    0    0 ...
   guest after up:     vnic0:  259276    2647    0    0 ...
   guest ping after:   3 packets transmitted, 3 packets received
```

The guest received all 637 queued frames (636 echo requests plus 1 ARP),
more than twice the ring size, with 0 drops. Guest TX rose only by 145
because the down/up cleared the guest's neighbor entry for 10.1.0.2, and
the guest kernel only holds a limited number of packets while it waits for
ARP to resolve; that is guest stack behavior, not the device.

PASSED.

### 7. Engine backend: RXEMPTY fix + regression

`engine_run()`'s loop was `while (th != tt && rh != rt)`, so "no posted
buffer" left the loop without raising `RXEMPTY`. It is now
`while (th != tt)` with `rh == rt` checked alongside `DD`, matching the
QEMU device.

The engine backend needs no QEMU device, so it was tested inside the nested
VM (nothing insmodded on the host):

```bash
cd driver && make clean && make && make raw_flood     # engine is the default
cd ../nested-vm && ./build-initramfs.sh ../driver/vnic.ko
./run.sh                                             # no -device vnic
```

```bash
# guest: same T3 / T7 / T12 sequence as step 4, then
ip link set vnic0 down; rmmod vnic; echo "rmmod rc=$?"
```

Expected: same results as the PCI backend's loopback mode; clean `rmmod`.

Result:

```
vnic0: VN1000 loaded, in-kernel engine backend (ID=0x564e3130)
T3   5 packets transmitted, 5 packets received, 0% packet loss
T7   5 packets transmitted, 5 packets received, 0% packet loss
T12  flood_rc=0
     vnic0:  300090    5001    0    0 ...   300090    5001    0    0 ...
     3 packets transmitted, 3 packets received, 0% packet loss
rmmod rc=0
```

No warnings in `dmesg`. (The first T12 attempt returned `flood_rc=127`,
command not found: `make clean` also deletes `driver/raw_flood`, so the
initramfs was packed without it. Rebuilt with `make raw_flood` and rerun.)

The driver and initramfs were rebuilt with `VNIC_BACKEND=pci` afterwards.

PASSED.

### 8. PCI backend: 64-bit DMA mask

`vnic_pci_hal_create()` now calls `dma_set_mask_and_coherent()` right
after `pci_set_master()`, before any ring is allocated: 64-bit first
(descriptors and ring bases carry 64-bit addresses), 32-bit as the
fallback, probe fails if neither works. Without it the PCI core's default
32-bit mask applies, which only happened to be enough because the nested VM
has 512 MB.

```bash
cd driver && make clean && make VNIC_BACKEND=pci && make raw_flood
cd ../nested-vm && ./build-initramfs.sh ../driver/vnic.ko
./run.sh -device vnic              # 512 MB
./run.sh -device vnic -m 3G        # about 1 GB of guest RAM above 4 GiB
```

```bash
# guest
insmod /vnic.ko
for d in /sys/bus/pci/devices/*; do [ "$(cat $d/device)" = 0x11e9 ] && \
  echo "$d $(cat $d/dma_mask_bits) $(cat $d/consistent_dma_mask_bits)"; done
# then the step 4 setup, ping -c 5 and /raw_flood 5000
```

The VN1000's PCI slot is not fixed (`00:02.0` in some runs, `00:03.0` in
others), so it is looked up by device ID.

Expected: both masks read 64 at both memory sizes; ping and flood pass.

Result:

```
             dma_mask_bits / coherent   ping   raw_flood 5000
512 MB       64 / 64                    5/5    flood_rc=0, 5013 RX == 5013 TX, 0 drops
3 GB         64 / 64                    5/5    flood_rc=0, 5013 RX == 5013 TX, 0 drops
```

At 3 GB the guest kernel logs
`PCI-DMA: Using software bounce buffering for IO (SWIOTLB)`: memory above
4 GiB exists, which is exactly where a 32-bit mask would force bounce
copies. Not verified directly: that a given skb sat above 4 GiB and was
not bounced (would need DMA addresses logged with `make DEBUG=y`).

PASSED.

## Not rerun this phase

Phase 3 steps 9 to 11 (rmmod while up, double insmod, load/unload x5) were
not rerun on the PCI backend; the only PCI driver change was the DMA mask
(step 8). Step 6's T5 / T6 were last run after step 5 (TX path), not after
the RX change; the RX change only touches the backend path, and T3 / T7 /
T12 were rerun after it (step 4).

## Conclusion

The device now has two working datapaths behind one property. With the
default `loopback=on`, the Phase 3 suite still passes unchanged. With
`loopback=off,netdev=n0`, frames cross from the guest to a real host network
stack and back: ARP resolves for real, ping works in both directions,
a 2000-frame flood loses nothing, and a queued backlog larger than the RX
ring is delivered without drops. The one bug found (step 6) was in the RX
backpressure design, caught by the drop counter rather than by ping loss.
Both backends now raise `RXEMPTY` per the datasheet (steps 4 and 7), and the
PCI backend declares its 64-bit DMA capability (step 8).
