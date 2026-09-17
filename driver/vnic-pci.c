// SPDX-License-Identifier: GPL-2.0
/* vnic-pci.c - the real PCI build of the VN1000 driver
 *
 * The build-specific half for VNIC_BACKEND=pci. Talks to the emulated PCI
 * device (qemu-device/vnic.c) over BAR0 MMIO and a single MSI. Contains
 * struct vnic_hal (opaque to vnic-core.c), the vnic-hal.h implementation, and
 * this build's module entry (a pci_driver whose probe() runs when the bus
 * enumerates a 1234:11e9 device).
 *
 * Scope right now (Phase 2, the keystone). Prove the round trip:
 *   1. lspci shows 1234:11e9
 *   2. readl(VN_REG_ID) returns 0x564E3130 over real MMIO
 *   3. a device-raised MSI reaches vnic_pci_isr()
 * NO descriptor rings, NO DMA, NO netdev yet. Phase 3 adds those and wires the
 * shared vnic-core.c datapath over real DMA/MSI.
 *
 * Every function below has a signature + "what / why / how to implement";
 * the bodies are TODO(phase2) / TODO(phase3) for you to fill.
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/io.h>			/* readl, writel */
#include <linux/interrupt.h>		/* request_irq, IRQ_HANDLED */
#include <linux/delay.h>		/* usleep_range */
#include <linux/errno.h>

#include "vnic.h"
#include "vnic-hal.h"

/**
 * struct vnic_hal - private state of the PCI backend
 * @priv: back-pointer to the core (NULL in Phase 2: no netdev yet)
 * @pdev: the PCI device
 * @mmio: BAR0, from pcim_iomap(); every vnic_hal_rd32/wr32 is readl/writel here
 * @msi_seen: set by vnic_pci_isr(), polled by the Phase 2 MSI self-test
 *
 * Phase 3 adds: dma_addr_t tx_ring_dma / rx_ring_dma, and whatever the DMA
 * mapping path needs. Opaque to vnic-core.c (it only holds a struct vnic_hal *).
 */
struct vnic_hal {
	struct vnic_priv *priv;
	struct pci_dev   *pdev;
	void __iomem     *mmio;
	bool              msi_seen;
};

/* the one instance, like vnic-engine.c's vnic_netdev */
static struct vnic_hal *vnic_pci_hal;

static irqreturn_t vnic_pci_isr(int irq, void *data);
static int vnic_pci_msi_selftest(struct vnic_hal *hal);

/* register access: the HAL primitives, now real MMIO */

/*
 * vnic_hal_rd32 - read a BAR0 register
 *
 * Simpler than the engine version: the ICR read-to-clear happens in the DEVICE
 * (qemu-device/vnic.c vnic_mmio_read), not here. readl() also carries the
 * ordering barrier the engine's plain array read did not.
 */
u32 vnic_hal_rd32(struct vnic_hal *hal, u32 off)
{
	return readl(hal->mmio + off);
}

/*
 * vnic_hal_wr32 - write a BAR0 register
 *
 * The ICS/IMS/IMC and TDT/RDT-doorbell side effects all happen in the device
 * now, so this is a bare writel().
 */
void vnic_hal_wr32(struct vnic_hal *hal, u32 off, u32 val)
{
	writel(val, hal->mmio + off);
}

/*
 * vnic_hal_wr32_raw - write with no doorbell side effect
 *
 * On real hardware there is no "raw" write: writel() to TDT/RDT always rings
 * the doorbell. The drain paths that use this will move to a CTRL.TXEN/RXEN
 * gate in Phase 3. No datapath in Phase 2, so: same as vnic_hal_wr32() for now.
 */
void vnic_hal_wr32_raw(struct vnic_hal *hal, u32 off, u32 val)
{
	writel(val, hal->mmio + off);
}

/*
 * datapath HAL: stubs until Phase 3
 *
 * vnic-core.o is linked in, so these symbols must exist. Phase 2 registers no
 * netdev, so none is ever called; WARN if one is, to catch a mistake early.
 */

int vnic_hal_ring_alloc(struct vnic_hal *hal, struct vnic_ring *ring, u32 count)
{
	WARN_ONCE(1, "vnic-pci: ring_alloc in phase 2\n");
	return -EOPNOTSUPP;   /* TODO(phase3): dma_alloc_coherent + program TDBAL/H etc. */
}

void vnic_hal_ring_free(struct vnic_hal *hal, struct vnic_ring *ring)
{
	/* TODO(phase3): dma_free_coherent */
}

dma_addr_t vnic_hal_map(struct vnic_hal *hal, void *va, size_t len,
			enum dma_data_direction dir)
{
	WARN_ONCE(1, "vnic-pci: map in phase 2\n");
	return 0;   /* TODO(phase3): dma_map_single(&hal->pdev->dev, va, len, dir) */
}

void vnic_hal_unmap(struct vnic_hal *hal, dma_addr_t dma, size_t len,
		    enum dma_data_direction dir)
{
	/* TODO(phase3): dma_unmap_single */
}

int vnic_hal_mapping_error(struct vnic_hal *hal, dma_addr_t dma)
{
	return 1;   /* TODO(phase3): dma_mapping_error(&hal->pdev->dev, dma) */
}

void vnic_hal_quiesce(struct vnic_hal *hal)
{
	/* TODO(phase3): synchronize_irq(pci_irq_vector(hal->pdev, 0)) once TX/RX
	 * are disabled
	 */
}

/*
 * vnic_hal_irq_request - arm the completion interrupt (ndo_open)
 *
 * pci_alloc_irq_vectors() (which reserves the MSI) is done once in probe();
 * request_irq()/free_irq() are per ndo_open/ndo_stop, the same shape as the
 * engine's tasklet_setup/tasklet_kill. In Phase 2 the self-test calls this
 * directly (there is no ndo_open yet).
 */
int vnic_hal_irq_request(struct vnic_hal *hal)
{
	int irq = pci_irq_vector(hal->pdev, 0);   /* Linux IRQ number for our one MSI vector */

	return request_irq(irq, vnic_pci_isr, 0, "vnic", hal);
}

/*
 * vnic_hal_irq_free - disarm the completion interrupt (ndo_stop)
 */
void vnic_hal_irq_free(struct vnic_hal *hal)
{
	int irq = pci_irq_vector(hal->pdev, 0);

	free_irq(irq, hal);
}

/* MSI */

/**
 * vnic_pci_isr - the MSI handler (hardirq context)
 * @irq:  the vector, from pci_irq_vector(pdev, 0)
 * @data: the struct vnic_hal * passed to request_irq()
 *
 * Phase 3: this is where napi_schedule() goes. A real hardirq handler can
 * schedule NAPI directly, so the engine's tasklet indirection is gone.
 *
 * Return: IRQ_HANDLED if this was our interrupt, IRQ_NONE otherwise.
 */
static irqreturn_t vnic_pci_isr(int irq, void *data)
{
	struct vnic_hal *hal = data;
	u32 icr = vnic_hal_rd32(hal, VN_REG_ICR);   /* read-to-clear happens device-side */

	if (!icr)
		return IRQ_NONE;                    /* not ours - shared-irq etiquette */

	WRITE_ONCE(hal->msi_seen, true);
	return IRQ_HANDLED;
}

/*
 * vnic_pci_msi_selftest - prove the device can interrupt us
 *
 * Unmask TXDONE, raise it via ICS (the device ORs it into ICR, sees
 * ICR & IMS != 0, calls msi_notify), and check vnic_pci_isr() saw it.
 */
static int vnic_pci_msi_selftest(struct vnic_hal *hal)
{
	WRITE_ONCE(hal->msi_seen, false);

	writel(VN_INT_TXDONE, hal->mmio + VN_REG_IMS);   /* unmask */
	writel(VN_INT_TXDONE, hal->mmio + VN_REG_ICS);   /* raise -> device fires the MSI */

	usleep_range(1000, 2000);                        /* give the interrupt time to arrive */

	writel(VN_INT_TXDONE, hal->mmio + VN_REG_IMC);   /* mask back off */

	return READ_ONCE(hal->msi_seen) ? 0 : -EIO;
}

/* module entry: pci_driver probe / remove */

/**
 * vnic_pci_probe - the bus enumerated a 1234:11e9 device
 * @pdev: the PCI device
 * @id:   the matching vnic_pci_ids[] entry
 *
 * Maps BAR0, allocates one MSI vector, verifies the ID magic over real MMIO,
 * and proves the MSI path with vnic_pci_msi_selftest(). No netdev yet - Phase
 * 3 continues past the final "return 0" with alloc_etherdev() and
 * register_netdev(), the vnic-engine.c shape.
 *
 * Everything but @hal itself is devres-managed (pcim_*, pci_alloc_irq_vectors
 * once pcim_enable_device() has run), so err_free only has to free @hal.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int vnic_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct vnic_hal *hal;
	u32 magic;
	int ret;

	hal = kzalloc(sizeof(*hal), GFP_KERNEL);
	if (!hal)
		return -ENOMEM;
	hal->pdev = pdev;
	pci_set_drvdata(pdev, hal);
	vnic_pci_hal = hal;

	ret = pcim_enable_device(pdev);        /* devres-managed pci_enable_device() */
	if (ret)
		goto err_free;

	/* BIT(0): map BAR0 only. devres-managed pci_request_region() + pci_iomap();
	 * KBUILD_MODNAME ("vnic") is the name that shows up in /proc/iomem.
	 */
	ret = pcim_iomap_regions(pdev, BIT(0), KBUILD_MODNAME);
	if (ret)
		goto err_free;
	hal->mmio = pcim_iomap_table(pdev)[0];   /* the kernel virtual address for BAR0 */

	pci_set_master(pdev);           /* bus-master enable: needed for MSI (+ Phase 3 DMA) */
	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);   /* exactly 1 vector, MSI only */
	if (ret < 0)                            /* success returns the count (1), not 0 */
		goto err_free;

	magic = readl(hal->mmio + VN_REG_ID);
	if (magic != VN_ID_MAGIC) {
		dev_err(&pdev->dev, "bad ID 0x%08x (want 0x%08x)\n", magic, VN_ID_MAGIC);
		ret = -ENODEV;
		goto err_free;
	}
	dev_info(&pdev->dev, "VN1000 found over MMIO, ID=0x%08x\n", magic);

	ret = vnic_hal_irq_request(hal);
	if (ret)
		goto err_free;
	ret = vnic_pci_msi_selftest(hal);
	vnic_hal_irq_free(hal);         /* only borrowed for the test; Phase 3's ndo_open owns it */
	if (ret) {
		dev_err(&pdev->dev, "MSI self-test failed: %d\n", ret);
		goto err_free;
	}
	dev_info(&pdev->dev, "MSI self-test passed\n");

	return 0;   /* Phase 3 continues here: alloc_etherdev + register_netdev */

err_free:
	vnic_pci_hal = NULL;
	kfree(hal);
	return ret;
}

/*
 * vnic_pci_remove - device gone / driver unloaded
 *
 * Phase 3 prepends: unregister_netdev, quiesce, ring free, free_netdev.
 */
static void vnic_pci_remove(struct pci_dev *pdev)
{
	struct vnic_hal *hal = pci_get_drvdata(pdev);

	vnic_pci_hal = NULL;
	kfree(hal);
	/* pcim regions, irq vectors, and device-enable are released automatically */
}

static const struct pci_device_id vnic_pci_ids[] = {
	{ PCI_DEVICE(VN_PCI_VENDOR_ID, VN_PCI_DEVICE_ID) },
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, vnic_pci_ids);

static struct pci_driver vnic_pci_driver = {
	.name     = "vnic",
	.id_table = vnic_pci_ids,
	.probe    = vnic_pci_probe,
	.remove   = vnic_pci_remove,
};
module_pci_driver(vnic_pci_driver);

MODULE_AUTHOR("Yu An Chen");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("VN1000 virtual NIC, real PCI/MMIO/MSI backend");
