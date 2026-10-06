/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vnic.c - VN1000 emulated PCIe NIC for QEMU
 *
 * The host-side counterpart of driver/vnic-engine.c: a real PCI device the
 * guest reaches over MMIO, DMA and MSI. Built into a QEMU tree at
 * hw/net/vnic.c (see the "Building into QEMU" note at the bottom).
 *
 * Implements the full register/interrupt model plus the DMA datapath:
 *   - an enumerable PCI device (shows up in lspci)
 *   - BAR0: a register file mirroring include/vnic_regs.h. ID/VERSION/CAP are
 *     read-only; CTRL/IMS/IMC/ICR/ICS behave per the spec; TDH/RDH are
 *     read-only to the guest, TDT/RDT ring the doorbell (vnic_run())
 *   - a single MSI raised when an unmasked interrupt cause is set
 *   - vnic_run(): loops pending TX descriptors back into the RX ring over
 *     pci_dma_read()/pci_dma_write(), rewriting each frame in place
 *     (vnic_loopback()) so the guest sees its own traffic reflected back -
 *     the QEMU counterpart of driver/vnic-engine.c's engine_run()
 *
 * include/vnic_regs.h (copied next to this file) is the SHARED spec with the
 * driver, so the two sides cannot drift apart.
 *
 * QEMU API note: written against QEMU 9.1. class_init's second parameter is
 * "void *data" here (it became "const void *data" in QEMU 9.2); the reset
 * hook and qemu_new_nic() argument count have also shifted across versions.
 * Pin QEMU to one version and adjust once if you bump it.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msi.h"
#include "hw/qdev-properties.h"
#include "net/eth.h"

#include "vnic_regs.h"   /* shared spec, copied into hw/net/ alongside this file */

/* ---------------------------------------------------------------------------
 * QOM boilerplate
 *
 * Every QEMU device is a QOM object. TYPE_VNIC is the type name used on the
 * command line (-device vnic) and in Kconfig. OBJECT_DECLARE_SIMPLE_TYPE
 * declares the instance struct (VNICState) and the VNIC() cast macro that
 * safely downcasts an Object* to VNICState*.
 * ------------------------------------------------------------------------- */
#define TYPE_VNIC "vnic"
OBJECT_DECLARE_SIMPLE_TYPE(VNICState, VNIC)

/**
 * struct VNICState - one emulated VN1000 instance
 * @pdev: embedded PCIDevice (the QOM parent). MUST be the first member so the
 *        VNIC() / PCI_DEVICE() casts line up.
 * @mmio: the BAR0 MemoryRegion; guest MMIO loads/stores into this range trap
 *        into vnic_mmio_read() / vnic_mmio_write() below.
 * @regs: the BAR0 register file, one u32 per 4-byte offset, indexed
 *        regs[offset / 4]. Mirrors include/vnic_regs.h. This is the device's
 *        entire state; the descriptor rings live in guest memory, reached
 *        only through TDBAL/TDBAH/RDBAL/RDBAH via DMA.
 */
struct VNICState {
    PCIDevice pdev;
    MemoryRegion mmio;
    uint32_t regs[VN_BAR0_SIZE / 4];
};

/* ---------------------------------------------------------------------------
 * interrupt logic
 * ------------------------------------------------------------------------- */

/*
 * vnic_irq_update - fire the MSI if there is an unmasked interrupt cause
 *
 * Assert condition: (regs[ICR] & regs[IMS]) != 0, i.e. some cause is raised and
 * not masked. MSI is edge-triggered, so this only ever *sends* a message; there
 * is no line to lower (that would be INTx). Call it only where the assert
 * condition can newly become true: a write to ICS (a cause was raised) or IMS
 * (a pending cause was unmasked). IMC and the ICR read-to-clear can only make
 * it false, so they do not call this.
 */
static void vnic_irq_update(VNICState *s)
{
    bool active = (s->regs[VN_REG_ICR / 4] & s->regs[VN_REG_IMS / 4]) != 0;

    if (active && msi_enabled(&s->pdev))
        msi_notify(&s->pdev, 0);
}

/**
 * vnic_loopback - rewrite a frame in place from "sent" to "received" form
 * @buf: frame bytes, modified in place
 * @len: frame length; the caller guarantees len >= sizeof(struct eth_header)
 *
 * Swaps the Ethernet source/destination MAC and, for IPv4, the source/
 * destination address. Swapping two equal-length fields leaves the IPv4 (and
 * any L4) checksum valid, since checksum summation is order-independent; no
 * recompute needed. The QEMU counterpart of vnic-engine.c's engine_flip_addr().
 */
static void vnic_loopback(uint8_t *buf, uint32_t len)
{
    struct eth_header *eth = (struct eth_header *)buf;
    uint8_t tmp[ETH_ALEN];

    memcpy(tmp, eth->h_dest, ETH_ALEN);
    memcpy(eth->h_dest, eth->h_source, ETH_ALEN);
    memcpy(eth->h_source, tmp, ETH_ALEN);

    if (be16_to_cpu(eth->h_proto) == ETH_P_IP && len >= sizeof(struct eth_header) + sizeof(struct ip_header)) {
        struct ip_header *ip = (struct ip_header *)(buf + sizeof(struct eth_header));
        uint32_t tmp_ip = ip->ip_src;

        ip->ip_src = ip->ip_dst;
        ip->ip_dst = tmp_ip;
    }
}

/**
 * vnic_run - the doorbell handler: loop TX descriptors back into the RX ring
 * @s: device state
 *
 * The QEMU counterpart of driver/vnic-engine.c's engine_run(). Triggered by
 * a write to TDT or RDT (either can unstall a pass blocked on a full RX
 * ring). For each pending TX descriptor: reads the descriptor and frame via
 * DMA, applies vnic_loopback(), writes the frame and descriptor into the
 * next free RX slot, and completes the TX descriptor. Stops on a full RX
 * ring (backpressure, no drop) or a failed DMA access (a misbehaving guest).
 */
static void vnic_run(VNICState *s)
{
    uint32_t ctrl = s->regs[VN_REG_CTRL / 4];
    /* Both RX/TX should be open */
    if ((ctrl & (VN_CTRL_TXEN | VN_CTRL_RXEN)) != (VN_CTRL_TXEN | VN_CTRL_RXEN))
        return;

    hwaddr tx_base, rx_base;
    uint32_t th, tt, rh, rt;
    uint32_t txp, txb, rxp, rxb, rxdrop;
    uint32_t th_start, rh_start;
    uint32_t tx_count, rx_count;
    struct vn_desc txd, rxd;
    uint8_t buf[VN_MTU_MAX + VN_FRAME_OVERHEAD];
    uint32_t cause = 0;
    hwaddr rx_desc_addr, tx_desc_addr;

    /* Snapshot the registers this pass needs. */
    tx_base = ((hwaddr)s->regs[VN_REG_TDBAH / 4] << 32) | s->regs[VN_REG_TDBAL / 4];
    rx_base = ((hwaddr)s->regs[VN_REG_RDBAH / 4] << 32) | s->regs[VN_REG_RDBAL / 4];
    tx_count = s->regs[VN_REG_TDLEN / 4] / sizeof(struct vn_desc);
    rx_count = s->regs[VN_REG_RDLEN / 4] / sizeof(struct vn_desc);
    th = s->regs[VN_REG_TDH / 4];
    tt = s->regs[VN_REG_TDT / 4];
    rh = s->regs[VN_REG_RDH / 4];
    rt = s->regs[VN_REG_RDT / 4];
    txp = s->regs[VN_REG_STAT_TXPKTS / 4];
    txb = s->regs[VN_REG_STAT_TXBYTES / 4];
    rxp = s->regs[VN_REG_STAT_RXPKTS / 4];
    rxb = s->regs[VN_REG_STAT_RXBYTES / 4];
    rxdrop = s->regs[VN_REG_STAT_RXDROP / 4];

    th_start = th;
    rh_start = rh;

    while (th != tt && rh != rt) {
        rx_desc_addr = rx_base + (hwaddr)rh * sizeof(struct vn_desc);
        tx_desc_addr = tx_base + (hwaddr)th * sizeof(struct vn_desc);

        if (pci_dma_read(&s->pdev, rx_desc_addr, &rxd, sizeof(rxd)) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR, "vnic: RX desc read failed at rh=%u\n", rh);
            break;   /* guest gave us a bad ring base; bail out of this pass */
        }
        if (rxd.status & VN_DESC_DD) {
            /* RX ring full: leave this TX descriptor pending. A later RDT
             * write (a freshly posted buffer) resumes the pass.
             */
            cause |= VN_INT_RXEMPTY;
            break;
        }

        if (pci_dma_read(&s->pdev, tx_desc_addr, &txd, sizeof(txd)) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR, "vnic: TX desc read failed at th=%u\n", th);
            break;   /* guest gave us a bad ring base; bail out of this pass */
        }

        /* Drop frames too short to hold an Ethernet header, and frames that
         * exceed either bound: the fixed host buffer capacity (sizeof(buf),
         * never guest-controlled) or the currently configured MAXFRM (which
         * the guest CAN write via MMIO, so it cannot be trusted alone).
         */
        if (txd.len < sizeof(struct eth_header) || txd.len > sizeof(buf) || txd.len > s->regs[VN_REG_MAXFRM / 4]) {
            rxdrop++;
        } else {
            if (pci_dma_read(&s->pdev, txd.addr, buf, txd.len) != MEMTX_OK) {
                qemu_log_mask(LOG_GUEST_ERROR, "vnic: TX packet read failed at th=%u\n", th);
                break;
            }

            vnic_loopback(buf, txd.len);

            /* The frame data must land before the RX descriptor's DD bit
             * does: vnic_rx_poll() trusts the data the moment it sees DD
             * set, and pci_dma_write() calls take effect in the order
             * issued, so writing the payload first is what guarantees that.
             */
            if (pci_dma_write(&s->pdev, rxd.addr, buf, txd.len) != MEMTX_OK) {
                qemu_log_mask(LOG_GUEST_ERROR, "vnic: RX packet write failed at rh=%u\n", rh);
                break;
            }

            rxd.len = txd.len;
            rxd.status |= VN_DESC_DD;

            if (pci_dma_write(&s->pdev, rx_desc_addr, &rxd, sizeof(rxd)) != MEMTX_OK) {
                qemu_log_mask(LOG_GUEST_ERROR, "vnic: RX desc write failed at rh=%u\n", rh);
                break;
            }
            rxp++;
            rxb += txd.len;
            rh = (rh + 1) % rx_count;
        }

        /* TX always completes here, whether or not the frame made it into
         * the RX ring.
         */
        txd.status |= VN_DESC_DD;
        if (pci_dma_write(&s->pdev, tx_desc_addr, &txd, sizeof(txd)) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR, "vnic: TX desc write failed at th=%u\n", th);
            break;
        }
        txp++;
        txb += txd.len;
        th = (th + 1) % tx_count;
    }

    s->regs[VN_REG_TDH / 4] = th;
    s->regs[VN_REG_RDH / 4] = rh;
    s->regs[VN_REG_STAT_TXPKTS / 4] = txp;
    s->regs[VN_REG_STAT_RXPKTS / 4] = rxp;
    s->regs[VN_REG_STAT_TXBYTES / 4] = txb;
    s->regs[VN_REG_STAT_RXBYTES / 4] = rxb;
    s->regs[VN_REG_STAT_RXDROP / 4] = rxdrop;

    if (th != th_start)
        cause |= VN_INT_TXDONE;
    if (rh != rh_start)
        cause |= VN_INT_RXAVAIL;
    if (cause) {
        s->regs[VN_REG_ICR / 4] |= cause;
        vnic_irq_update(s);
    }
}

/* ---------------------------------------------------------------------------
 * BAR0 MMIO
 *
 * These two callbacks ARE the register semantics. The guest driver's readl() on
 * BAR0+off lands in vnic_mmio_read(); its writel() lands in vnic_mmio_write().
 * ------------------------------------------------------------------------- */

/**
 * vnic_mmio_read - guest reads a BAR0 register
 * @opaque: the VNICState (the pointer passed to memory_region_init_io())
 * @addr:   byte offset into BAR0 (VN_REG_*)
 * @size:   access width in bytes; the .impl block below pins this to 4
 *
 * ICR is read-to-clear: the read-to-clear that the engine backend emulated
 * inside vnic_hal_rd32() now lives here, in the device; the driver's
 * vnic_hal_rd32() becomes a plain readl().
 */
static uint64_t vnic_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    VNICState *s = opaque;
    uint32_t val;

    if (addr + 4 > VN_BAR0_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "vnic: read out of bounds 0x%" HWADDR_PRIx "\n", addr);
        return 0;
    }

    if (addr == VN_REG_ICR) {                 /* read-to-clear */
        val = s->regs[VN_REG_ICR / 4];
        s->regs[VN_REG_ICR / 4] = 0;
        return val;                           /* MSI is edge; nothing to lower */
    }

    return s->regs[addr / 4];
}

/**
 * vnic_mmio_write - guest writes a BAR0 register
 * @opaque: the VNICState
 * @addr:   byte offset into BAR0 (VN_REG_*)
 * @val:    the value the guest wrote
 * @size:   access width (pinned to 4 by .impl)
 *
 * Mirrors vnic_hal_wr32() on the in-kernel engine backend: ICS/IMS/IMC carry
 * interrupt side effects, CTRL.RST zeroes TDH/RDH and self-clears, TDT/RDT
 * ring the doorbell and run the device, TDH/RDH are otherwise read-only, and
 * everything else is a plain store.
 */
static void vnic_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    VNICState *s = opaque;

    if (addr + 4 > VN_BAR0_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "vnic: write out of bounds 0x%" HWADDR_PRIx "\n", addr);
        return;
    }

    switch (addr) {
    case VN_REG_ID:
    case VN_REG_VERSION:
    case VN_REG_CAP:
    case VN_REG_TDH:                                 /* RO: blocks guest writel(); vnic_run() still updates s->regs[] directly */
    case VN_REG_RDH:                                 /* same: RO to the guest, not to the device */
        break;                                       /* read-only, ignore the write */

    case VN_REG_ICS:
        s->regs[VN_REG_ICR / 4] |= val;              /* write-1-to-set into ICR */
        vnic_irq_update(s);
        break;

    case VN_REG_IMS:
        s->regs[VN_REG_IMS / 4] |= val;              /* unmask: a pending cause may now fire */
        vnic_irq_update(s);
        break;

    case VN_REG_IMC:
        s->regs[VN_REG_IMS / 4] &= ~val;             /* mask: can only lower the condition */
        break;

    case VN_REG_CTRL:
        if (val & VN_CTRL_RST) {
            s->regs[VN_REG_TDH / 4] = 0;
            s->regs[VN_REG_RDH / 4] = 0;
            val &= ~VN_CTRL_RST;                     /* self-clearing */
        }
        s->regs[VN_REG_CTRL / 4] = val;
        break;

    case VN_REG_TDT:
    case VN_REG_RDT:
        s->regs[addr / 4] = val;                     /* driver's new tail value */
        vnic_run(s);                                  /* doorbell: try to make progress */
        break;

    default:
        s->regs[addr / 4] = val;                     /* TDBAL / TDBAH / TDLEN / etc: plain store */
        break;
    }
}

/*
 * The ops table wired to the BAR0 MemoryRegion. DEVICE_LITTLE_ENDIAN matches
 * "all registers are 32-bit, little-endian" in vnic_regs.h. The .impl limits
 * force every access the core sees to be exactly 4 bytes, so the callbacks
 * never have to handle byte/word sub-accesses.
 */
static const MemoryRegionOps vnic_mmio_ops = {
    .read = vnic_mmio_read,
    .write = vnic_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ---------------------------------------------------------------------------
 * realize / unrealize (device construction and teardown)
 * ------------------------------------------------------------------------- */

/**
 * vnic_realize - bring one device instance up (called for each -device vnic)
 * @pdev: the embedded PCIDevice; VNIC() back to the VNICState
 * @errp: error sink; on failure the callee sets it and we return
 *
 * Sets up BAR0 (the MMIO region), one MSI vector, and the RO identity
 * registers. PCI config space (vendor/device/class) is done in class_init.
 */
static void vnic_realize(PCIDevice *pdev, Error **errp)
{
    VNICState *s = VNIC(pdev);
    int ret;

    /* BAR0: a 4 KiB MMIO window, owner OBJECT(s) so it's freed with the device */
    memory_region_init_io(&s->mmio, OBJECT(s), &vnic_mmio_ops, s, "vnic-mmio", VN_BAR0_SIZE);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);

    /* one MSI vector; offset 0 = let QEMU place it in config space */
    ret = msi_init(pdev, 0, 1, true, false, errp);
    if (ret < 0)
        return;              /* errp already set by msi_init() */

    /* RO identity registers; CAP advertises MSI */
    s->regs[VN_REG_ID / 4]      = VN_ID_MAGIC;
    s->regs[VN_REG_VERSION / 4] = VN_VERSION_1_0;
    s->regs[VN_REG_CAP / 4]     = VN_CAP_MSI;

    /* default frame-size limit; vnic_change_mtu() keeps it in sync with MTU */
    s->regs[VN_REG_MAXFRM / 4] = VN_MAXFRM_DEFAULT;
}

/**
 * vnic_unrealize() - tear one instance down (hot-unplug, or QEMU exit)
 * @pdev: the embedded PCIDevice; VNIC() back to the VNICState
 */
static void vnic_unrealize(PCIDevice *pdev)
{
    msi_uninit(pdev);   /* the MemoryRegion is owned by OBJECT(s), freed with it */
}

/* ---------------------------------------------------------------------------
 * class_init: static per-type setup
 * ------------------------------------------------------------------------- */

/**
 * vnic_class_init() - fill in the class vtable for TYPE_VNIC
 * @klass: the ObjectClass; DEVICE_CLASS()/PCI_DEVICE_CLASS() downcast it
 * @data: QOM class-registration opaque; unused here
 *
 * Runs once, when the "vnic" type is first needed - not per instance (that is
 * vnic_realize()). Device properties (loopback on/off, itr_usecs,
 * tx_coalesce) would go here via DEFINE_PROP_*; none exist yet.
 */
static void vnic_class_init(ObjectClass *klass, void *data)
{
    DeviceClass    *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k  = PCI_DEVICE_CLASS(klass);

    k->realize   = vnic_realize;
    k->exit      = vnic_unrealize;
    k->vendor_id = VN_PCI_VENDOR_ID;
    k->device_id = VN_PCI_DEVICE_ID;
    k->revision  = 0x01;
    k->class_id  = PCI_CLASS_NETWORK_ETHERNET;

    dc->desc = "VN1000 virtual 1GbE NIC";
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

/* ---------------------------------------------------------------------------
 * type registration
 * ------------------------------------------------------------------------- */

static const TypeInfo vnic_type_info = {
    .name          = TYPE_VNIC,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(VNICState),
    .class_init    = vnic_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

/**
 * vnic_register_types() - module constructor: register TYPE_VNIC with QOM
 *
 * Run via type_init() below, at QEMU startup, before any -device option is
 * parsed; this is what makes "-device vnic" resolvable at all.
 */
static void vnic_register_types(void)
{
    type_register_static(&vnic_type_info);
}

type_init(vnic_register_types)

/*
 * Building into QEMU:
 *   1. cp qemu-device/vnic.c        <qemu>/hw/net/vnic.c
 *      cp include/vnic_regs.h       <qemu>/hw/net/vnic_regs.h
 *   2. <qemu>/hw/net/meson.build:   system_ss.add(when: 'CONFIG_VNIC', if_true: files('vnic.c'))
 *   3. <qemu>/hw/net/Kconfig:       config VNIC
 *                                       bool
 *                                       default y if PCI_DEVICES
 *                                       depends on PCI
 *   4. configure --target-list=x86_64-softmmu --enable-kvm (+ --disable-* to keep the build small)
 *   5. run:  qemu-system-x86_64 ... -device vnic
 *      check: lspci -nn | grep 1234:11e9
 */
