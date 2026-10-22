# Phase 5 Test Log

## Background

Phase 5 goal: finish the netdev so it behaves like a real NIC's driver
across resets, reboots and interface cycles.

- **Full device reset.** `CTRL.RST` used to zero only `TDH`/`RDH`. It now
  returns the device to its power-on state (`vnic_reset()` in the QEMU
  device, `engine_reset_regs()` in the engine backend), and the QEMU device
  also resets on a QEMU system reset (`vnic_reset_hold()`), e.g. a guest
  reboot. The driver no longer relies on anything surviving a reset:
  `vnic_hw_config()` writes the ring bases, `MAXFRM` and the MAC on every
  open.
- **Stable MAC.** The device loads `conf.macaddr` into `MAC_LO`/`MAC_HI`
  with `AV` set at power-on, playing the role of a NIC's EEPROM; the PCI
  backend reads it at probe instead of picking a random address. The
  driver's `dev_addr` stays the source of truth and is written back on open.
- **64-bit stats.** The device's `STAT_*` registers are 32-bit and now
  cleared by a reset, so the driver keeps 64-bit totals: it adds the u32
  delta since its previous read, once a second (a delayed work, so a
  register can't wrap twice unseen), on every `ndo_get_stats64`, and right
  before a reset. `/proc/net/dev` therefore only starts from zero when the
  netdev is created.
- **`nested-vm/test-tap.sh`**, a repeatable version of Phase 4's tap tests.

The order was chosen so each step could be tested alone: the driver was
made reset-proof (5.1, 5.2) before the device's reset became destructive
(5.3), and the MAC readout (5.4) came last.

## Environment

```
guest:   nested-VM harness (nested-vm/), busybox init
host:    kernel 6.8.0-147-generic (updated from 142 during this phase)
QEMU:    self-built 9.1.3 at ~/qemu (-Werror); hw/net/vnic.c re-synced from
         qemu-device/vnic.c before every build
driver:  VNIC_BACKEND=pci unless noted; engine backend rebuilt and tested
         after each driver change, then the PCI build restored
network: tap0 on the host, 10.1.0.2/24 (setup-tap.sh); guest 10.1.0.1/24
```

The host kernel update meant re-copying the VM's kernel image, otherwise a
module built for 147 will not insmod under the old 142 `vmlinuz`. The
images under `/boot` are root-only (mode 0600), so:

```bash
sudo cp /boot/vmlinuz-$(uname -r) nested-vm/vmlinuz && sudo chown $USER: nested-vm/vmlinuz
```

The host also rebooted several times; `setup-tap.sh` recreated tap0 each time.

Register values below were read inside the guest with busybox `devmem` on
BAR0, found by PCI ID rather than slot:

```bash
for d in /sys/bus/pci/devices/*; do [ "$(cat $d/device)" = 0x11e9 ] && \
  B=$(head -1 $d/resource | cut -d' ' -f1); done
devmem $((B+0x18)) 32      # MAC_LO; 0x1C MAC_HI, 0x10 CTRL, 0x100 TDBAL, 0x408 STAT_RXPKTS
```

The stats registers are read-only counters with no read side effects, so
reading them this way does not disturb the driver's totals.

"Loopback suite" below means Phase 3's T3 (ping + counter parity), T7
(down/up, then ping) and T12 (`/raw_flood 5000`, then ping), as in
`Phase4_test.md` step 4.

## Test Steps

### 1. Build

```bash
cp qemu-device/vnic.c ~/qemu/hw/net/vnic.c && ninja -C ~/qemu/build
cd driver && make clean && make && make clean && make VNIC_BACKEND=pci && make raw_flood
```

Expected and result: zero warnings for QEMU and both backends after every
step below.

PASSED.

### 2. 5.1: the driver programs the device on every open

`vnic_hw_config()` in `vnic_open()` writes `TDBAL/TDBAH/TDLEN`,
`RDBAL/RDBAH/RDLEN`, `MAXFRM` and `MAC_LO`/`MAC_HI` (with `AV`) before the
TX/RX gate opens; the probe no longer writes the ring bases.

```bash
./nested-vm/run.sh -device vnic
# guest: insmod, read registers, then the loopback suite setup and ping
```

Expected: ring bases still 0 after insmod, set after up; `MAC_LO`/`MAC_HI`
match `dev_addr`.

Result:

```
after insmod: MAC_LO=0x00000000 MAC_HI=0x00000000 TDBAL=0x00000000 RDBAL=0x00000000
after up:     MAC_LO=0x404CC02E MAC_HI=0x8000CF22 TDBAL=0x02AD2000 RDBAL=0x02AD1000
dev_addr:     2e:c0:4c:40:22:cf
```

`2E C0 4C 40` / `CF 22` are the address bytes in little-endian order, bit 31
of `MAC_HI` is `AV`. Loopback suite, tap mode (both directions, flood 2000,
down/up) and the engine backend's loopback suite plus `rmmod` all passed.

PASSED.

### 3. 5.2: 64-bit software stats

The device did not clear its counters on reset yet, so the driver's totals
should equal the registers, and keep counting across down/up. After an
rmmod/insmod the new netdev should start from zero even though the
registers do not (`vnic_init()` resyncs at registration).

```bash
# guest, after each stage
devmem $((B+0x408)) 32                     # STAT_RXPKTS
grep vnic0 /proc/net/dev
# stages: T3, T7, T12, then: ip link set vnic0 down; rmmod vnic; insmod /vnic.ko; ip link set vnic0 up
```

Result:

```
stage               register RXPKTS   /proc/net/dev RX packets
after T3            16                16
after T7            32                32
after T12           5041              5041
after rmmod+insmod  5047              6
```

The last row is the 6 packets seen since the new registration
(5047 minus 5041). Tap mode: RX 2017 before a down/up, 2021 after, 0 drops;
both directions and a 2000 frame flood at 0% loss. Engine backend loopback
suite and `rmmod` passed.

Not exercised: an actual 32-bit wrap, which needs over 4 GiB through the
device; the guest has no traffic generator that large.

PASSED.

### 4. 5.3: full device reset

```bash
./nested-vm/run.sh -device vnic
# guest: register snapshot after insmod, after up + ping, after down,
#        after up + ping, after /raw_flood 5000
```

Expected: after down, every register back to power-on (MAC from the
device, ring bases and counters 0) while `/proc/net/dev` keeps counting;
traffic works again after up.

Result:

```
stage             CTRL  MAC_LO / MAC_HI           TDBAL       reg RXPKTS  netdev RX
after insmod      0x0   0x12005452 / 0x80005734   0x00000000  0           0
up, after ping    0xC   0xCBEF682A / 0x80003C25   0x02B40000  16          16
after down        0x0   0x12005452 / 0x80005734   0x00000000  0           16
up, after ping    0xC   0xCBEF682A / 0x80003C25   0x02B40000  16          32
after flood       0xC   0xCBEF682A / 0x80003C25   0x02B40000  5025        5041
```

The power-on MAC here is `52:54:00:12:34:57`, not `:56`; see step 6.
Loopback suite passed.

System reset: `-action reboot=reset` overrides `run.sh`'s `-no-reboot`, so a
guest reboot resets the machine instead of exiting QEMU.

```bash
./nested-vm/run.sh -device vnic -action reboot=reset
# guest: insmod, up, ping, register snapshot, then: reboot -f
# second boot: register snapshot before insmod, then insmod, up, ping
```

Result:

```
boot 1, up:              CTRL=0xC  TDBAL=0x02C3D000  RXPKTS=12
boot 2, before insmod:   CTRL=0x0  TDBAL=0x00000000  RXPKTS=0   MAC_HI=0x80005734
boot 2, up:              ping 3/3
```

`reboot -f` does not bring the interface down, so the driver never wrote
`RST`; the clean registers on the second boot come from `vnic_reset_hold()`.
Without it the device would still have `RXEN` set and the old ring bases,
and in tap mode could DMA host frames into memory the new kernel owns.

Tap mode: MAC `52:54:00:12:34:56` with `AV` after insmod; both directions,
down/up and a 2000 frame flood at 0% loss, 0 drops. Engine backend
loopback suite and `rmmod` passed.

PASSED.

### 5. 5.4: MAC from the device

```bash
./nested-vm/run.sh -device vnic
./nested-vm/run.sh -device vnic,mac=52:54:00:aa:bb:cc
./nested-vm/run.sh -device vnic,mac=01:00:5e:00:00:01      # multicast: not a valid station address
# guest: insmod /vnic.ko; cat /sys/class/net/vnic0/address
```

Result:

```
default         52:54:00:12:34:57
mac= override   52:54:00:aa:bb:cc
multicast       random address, and in dmesg:
                vnic 0000:00:03.0: no valid MAC in the device, using a random one
```

Loopback suite passed.

Across a guest reboot in tap mode (`-action reboot=reset`), with the host's
neighbor table flushed first:

```
boot 1: guest MAC 52:54:00:12:34:56; host: 10.1.0.1 lladdr 52:54:00:12:34:56 REACHABLE, ping 2/2
boot 2: guest MAC 52:54:00:12:34:56; host: 10.1.0.1 lladdr 52:54:00:12:34:56 DELAY, ping 2/2
```

Same address both boots, so the host's existing neighbor entry stays valid.

PASSED.

### 6. `-nic none` in run.sh

Step 4 and 5 showed `:57` in loopback mode but `:56` in tap mode. The guest's
PCI list explained it:

```
-device vnic                      00:02.0 8086:10d3 (e1000e)   00:03.0 1234:11e9 (VN1000)
-netdev tap,... -device vnic,...  00:02.0 1234:11e9 (VN1000)
```

Without a `-netdev`, QEMU adds its default NIC, an e1000e, which takes the
first default MAC (`:56`) and slot `02.0`. This is also why VN1000's slot
had been moving between `02.0` and `03.0` since Phase 3. The e1000e had no
driver in the guest (`CONFIG_E1000E=m`, not in the initramfs), so it did
nothing. `run.sh` now passes `-nic none`.

Result: in both modes the guest sees only `00:02.0 1234:11e9`, MAC
`52:54:00:12:34:56`. Loopback T3 and T12 passed. Loopback mode now prints
`warning: nic vnic.0 has no peer` at startup: correct (loopback needs no
backend) and harmless; the default NIC used to mask it.

PASSED.

### 7. 5.5: test-tap.sh

`nested-vm/test-tap.sh` boots the VM in tap mode with `TEST=tap`, which
makes `/init` run the guest half instead of a shell. The two halves sync on
`=== VNIC ...` lines on the serial console; the host tells the guest to
continue by connecting to TCP port 9000 (`nc` on both sides), and ends the
VM through a QEMU monitor socket.

```bash
cd driver && make VNIC_BACKEND=pci && make raw_flood
cd ../nested-vm && ./build-initramfs.sh
./test-tap.sh
```

Result:

```
[PASS] MAC     vnic0 = 52:54:00:12:34:56
[PASS] A       L2 -> L1 ping 0% loss
[PASS] B       L2 down/up -> ping 0% loss
[PASS] C       L1 -> L2 ping 0% loss
[PASS] D       L1 -> L2 flood: 2000 packets transmitted, 2000 received, 0% packet loss
[PASS] NEIGH   L1 neighbor 10.1.0.1 = 52:54:00:12:34:56
[PASS] F       backlog of 590 frames across down/up: 590 received, rx_dropped 0
7/7 passed.
```

About 21 seconds end to end. Failure path, with a timeout shorter than the
guest's 13 second down window:

```bash
TIMEOUT=5 ./test-tap.sh
```

```
[FAIL] F       timed out waiting for the guest (UP)
6/7 passed.
exit=1
```

No QEMU process was left running afterwards.

PASSED.

## Conclusion

The device now has one definition of its power-on state, used for
`CTRL.RST` and for a QEMU system reset, and the driver programs everything
it needs on open, so nothing depends on what a reset happens to keep. The
MAC comes from the device and is the same on every boot, and interface
counters are 64-bit and survive down/up and device resets. All of Phase 4's
tap checks now run as one command, `nested-vm/test-tap.sh`.
