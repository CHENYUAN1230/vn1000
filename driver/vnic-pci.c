// SPDX-License-Identifier: GPL-2.0
/* vnic-pci.c - the real PCI build of the VN1000 driver
 *
 * The build-specific half for VNIC_BACKEND=pci. Talks to the emulated PCI
 * device (qemu-device/vnic.c) over BAR0 MMIO, DMA rings and a single MSI.
 * Contains struct vnic_hal (opaque to vnic-core.c), the vnic-hal.h
 * implementation, and this build's module entry (a pci_driver whose probe()
 * runs when the bus enumerates a 1234:11e9 device).
 *
 * Shape mirrors vnic-engine.c: alloc_etherdev() first, then
 * vnic_pci_hal_create() brings the hardware up (BAR0, MSI vector, ID check,
 * DMA rings) and wires priv->hal, the PCI counterpart to engine_create().
 * register_netdev() last, and the shared vnic-core.c datapath takes over from
 * there over real MMIO/DMA/MSI.
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/io.h>			/* readl, writel */
#include <linux/interrupt.h>		/* request_irq, IRQ_HANDLED */
#include <linux/kernel.h>		/* lower_32_bits, upper_32_bits */
#include <linux/string.h>		/* strscpy */
#include <linux/netdevice.h>
#include <linux/etherdevice.h>		/* alloc_etherdev, eth_hw_addr_random */
#include <linux/errno.h>
#include <linux/dma-mapping.h>		/* dma_set_mask_and_coherent, DMA_BIT_MASK */

#include "vnic.h"
#include "vnic-hal.h"

/**
 * struct vnic_hal - private state of the PCI backend
 * @priv: back-pointer to the core
 * @pdev: the PCI device
 * @mmio: BAR0, from pcim_iomap(); every vnic_hal_rd32/wr32 is readl/writel here
 *
 * Opaque to vnic-core.c (it only holds a struct vnic_hal *).
 */
struct vnic_hal {
	struct vnic_priv *priv;
	struct pci_dev   *pdev;
	void __iomem     *mmio;
};

static irqreturn_t vnic_pci_isr(int irq, void *data);

/* register access: the HAL primitives, now real MMIO */

/**
 * vnic_hal_rd32() - read a BAR0 register
 * @hal: backend handle
 * @off: register byte offset (VN_REG_*)
 *
 * Simpler than the engine version: the ICR read-to-clear happens in the DEVICE
 * (qemu-device/vnic.c vnic_mmio_read), not here. readl() also carries the
 * ordering barrier the engine's plain array read did not.
 *
 * Return: the register value.
 */
u32 vnic_hal_rd32(struct vnic_hal *hal, u32 off)
{
	return readl(hal->mmio + off);
}

/**
 * vnic_hal_wr32() - write a BAR0 register
 * @hal: backend handle
 * @off: register byte offset (VN_REG_*)
 * @val: value to write
 *
 * The ICS/IMS/IMC and TDT/RDT-doorbell side effects all happen in the device
 * now, so this is a bare writel().
 */
void vnic_hal_wr32(struct vnic_hal *hal, u32 off, u32 val)
{
	writel(val, hal->mmio + off);
}

/*
 * datapath HAL: per-ring and per-packet DMA
 *
 * vnic_hal_ring_alloc/free() back the TX/RX descriptor rings (dma_alloc/
 * free_coherent()); vnic_hal_map/unmap/mapping_error() back each packet
 * buffer (dma_map/unmap_single(), dma_mapping_error()). Called from
 * vnic_pci_hal_create()/vnic_pci_hal_destroy() and from vnic-core.c's TX/RX
 * paths respectively.
 */

/**
 * vnic_hal_ring_alloc() - allocate one TX or RX descriptor ring
 * @hal: backend handle
 * @ring: ring to populate (->desc, ->desc_dma, ->bufs, ->count, ->lock)
 * @count: number of descriptors
 *
 * Unlike the engine backend's kcalloc, this memory must be DMA-coherent: the
 * device reads/writes it directly, so dma_alloc_coherent() gives both a CPU
 * pointer (->desc) and the bus address the device needs (->desc_dma).
 *
 * Return: 0 on success, -ENOMEM on failure (the ring is left freed).
 */
int vnic_hal_ring_alloc(struct vnic_hal *hal, struct vnic_ring *ring, u32 count)
{
	ring->desc = dma_alloc_coherent(&hal->pdev->dev, count * sizeof(struct vn_desc), &ring->desc_dma, GFP_KERNEL);
	if (!ring->desc)
		return -ENOMEM;

	ring->count = count;
	ring->next_to_use = 0;
	ring->next_to_clean = 0;

	ring->bufs = kcalloc(count, sizeof(struct sk_buff *), GFP_KERNEL);
	if (!ring->bufs)
		goto err_free;

	spin_lock_init(&ring->lock);
	return 0;

err_free:
	vnic_hal_ring_free(hal, ring);
	return -ENOMEM;
}

/**
 * vnic_hal_ring_free() - free a ring allocated by vnic_hal_ring_alloc()
 * @hal: backend handle
 * @ring: ring to free; ->desc and ->bufs are set to NULL
 */
void vnic_hal_ring_free(struct vnic_hal *hal, struct vnic_ring *ring)
{
	/* free ring->desc */
	if (ring->desc) {
		dma_free_coherent(&hal->pdev->dev, ring->count * sizeof(struct vn_desc), ring->desc, ring->desc_dma);
		ring->desc = NULL;
	}

	/* free ring->bufs */
	if (ring->bufs) {
		kfree(ring->bufs);
		ring->bufs = NULL;
	}
}

/**
 * vnic_hal_map() - lend a buffer to the device
 * @hal: backend handle
 * @va: kernel virtual address of the buffer
 * @len: buffer length
 * @dir: DMA direction
 *
 * The buffer becomes device-owned until vnic_hal_unmap(): the CPU must not
 * touch it.
 *
 * Return: the DMA (bus) address the device should use; test it with
 * vnic_hal_mapping_error().
 */
dma_addr_t vnic_hal_map(struct vnic_hal *hal, void *va, size_t len, enum dma_data_direction dir)
{
	return dma_map_single(&hal->pdev->dev, va, len, dir);
}

/**
 * vnic_hal_unmap() - undo vnic_hal_map()
 * @hal: backend handle
 * @dma: address returned by vnic_hal_map()
 * @len: length passed to vnic_hal_map()
 * @dir: direction passed to vnic_hal_map()
 *
 * Hands the buffer back to the CPU; safe to read/write only after this
 * returns.
 */
void vnic_hal_unmap(struct vnic_hal *hal, dma_addr_t dma, size_t len, enum dma_data_direction dir)
{
	dma_unmap_single(&hal->pdev->dev, dma, len, dir);
}

/**
 * vnic_hal_mapping_error() - test a vnic_hal_map() result
 * @hal: backend handle
 * @dma: address returned by vnic_hal_map()
 *
 * Return: non-zero if the mapping failed.
 */
int vnic_hal_mapping_error(struct vnic_hal *hal, dma_addr_t dma)
{
	return dma_mapping_error(&hal->pdev->dev, dma);
}

/**
 * vnic_hal_quiesce() - block until no in-flight work will touch the rings
 * @hal: backend handle
 *
 * Technically redundant with free_irq()'s own ISR wait, kept explicit so
 * this function stays correct on its own, independent of vnic_release()'s
 * call order (the engine backend's quiesce does real work).
 */
void vnic_hal_quiesce(struct vnic_hal *hal)
{
	synchronize_irq(pci_irq_vector(hal->pdev, 0));
}

/**
 * vnic_hal_irq_request() - arm the completion interrupt (ndo_open)
 * @hal: backend handle
 *
 * pci_alloc_irq_vectors() (which reserves the MSI) is done once in
 * vnic_pci_hal_create(); request_irq()/free_irq() are per ndo_open/ndo_stop,
 * the same shape as the engine's tasklet_setup/tasklet_kill.
 *
 * Return: 0 on success, negative errno from request_irq() on failure.
 */
int vnic_hal_irq_request(struct vnic_hal *hal)
{
	int irq = pci_irq_vector(hal->pdev, 0);   /* Linux IRQ number for our one MSI vector */

	return request_irq(irq, vnic_pci_isr, 0, "vnic", hal);
}

/**
 * vnic_hal_irq_free() - disarm the completion interrupt (ndo_stop)
 * @hal: backend handle
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
 * A real hardirq handler can schedule NAPI directly, so the engine's tasklet
 * indirection is gone. request_irq() only ever happens from ndo_open (after
 * register_netdev()), so hal->priv is always valid by the time this can fire.
 *
 * Return: IRQ_HANDLED if this was our interrupt, IRQ_NONE otherwise.
 */
static irqreturn_t vnic_pci_isr(int irq, void *data)
{
	struct vnic_hal *hal = data;
	u32 icr = vnic_hal_rd32(hal, VN_REG_ICR);   /* read-to-clear happens device-side */

	if (!icr)
		return IRQ_NONE;                    /* not ours - shared-irq etiquette */

	napi_schedule(&hal->priv->napi);
	return IRQ_HANDLED;
}

/* module entry: pci_driver probe / remove */

/**
 * vnic_pci_hal_create() - bring the hardware up and wire priv->hal
 * @priv: netdev private data; priv->hal is set here once @hal is usable
 * @pdev: the PCI device passed to probe()
 *
 * Enables the device, maps BAR0, reserves one MSI vector, verifies the ID
 * magic over real MMIO, then allocates the TX/RX rings and programs their
 * base/length registers. The PCI counterpart to engine_create().
 *
 * Everything up through pci_alloc_irq_vectors() is devres-managed (pcim_*),
 * so it unwinds itself automatically if this returns an error; only @hal and
 * the two rings need a manual ladder here.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int vnic_pci_hal_create(struct vnic_priv *priv, struct pci_dev *pdev)
{
	struct vnic_hal *hal;
	u32 magic;
	int ret;

	hal = kzalloc(sizeof(*hal), GFP_KERNEL);
	if (!hal)
		return -ENOMEM;
	hal->pdev = pdev;

	ret = pcim_enable_device(pdev);        /* devres-managed pci_enable_device() */
	if (ret)
		goto err_free_hal;

	/* BIT(0): map BAR0 only. devres-managed pci_request_region() + pci_iomap();
	 * KBUILD_MODNAME ("vnic") is the name that shows up in /proc/iomem.
	 */
	ret = pcim_iomap_regions(pdev, BIT(0), KBUILD_MODNAME);
	if (ret)
		goto err_free_hal;
	hal->mmio = pcim_iomap_table(pdev)[0];   /* the kernel virtual address for BAR0 */

	pci_set_master(pdev);           /* bus-master enable: needed for MSI + DMA */

	/* descriptors carry 64-bit addresses; fall back to 32-bit if the platform can't */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "no usable DMA mask\n");
		goto err_free_hal;
	}

	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);   /* exactly 1 vector, MSI only */
	if (ret < 0)                            /* success returns the count (1), not 0 */
		goto err_free_hal;

	magic = readl(hal->mmio + VN_REG_ID);
	if (magic != VN_ID_MAGIC) {
		dev_err(&pdev->dev, "bad ID 0x%08x (want 0x%08x)\n", magic, VN_ID_MAGIC);
		ret = -ENODEV;
		goto err_free_hal;
	}
	dev_info(&pdev->dev, "VN1000 found over MMIO, ID=0x%08x\n", magic);

	hal->priv = priv;
	priv->hal = hal;

	ret = vnic_hal_ring_alloc(hal, &priv->tx_ring, VNIC_TX_RING_SIZE);
	if (ret)
		goto err_free_hal;
	vnic_hal_wr32(hal, VN_REG_TDBAL, lower_32_bits(priv->tx_ring.desc_dma));
	vnic_hal_wr32(hal, VN_REG_TDBAH, upper_32_bits(priv->tx_ring.desc_dma));
	vnic_hal_wr32(hal, VN_REG_TDLEN, priv->tx_ring.count * sizeof(struct vn_desc));

	ret = vnic_hal_ring_alloc(hal, &priv->rx_ring, VNIC_RX_RING_SIZE);
	if (ret)
		goto err_free_tx_ring;
	vnic_hal_wr32(hal, VN_REG_RDBAL, lower_32_bits(priv->rx_ring.desc_dma));
	vnic_hal_wr32(hal, VN_REG_RDBAH, upper_32_bits(priv->rx_ring.desc_dma));
	vnic_hal_wr32(hal, VN_REG_RDLEN, priv->rx_ring.count * sizeof(struct vn_desc));

	return 0;

err_free_tx_ring:
	vnic_hal_ring_free(hal, &priv->tx_ring);
err_free_hal:
	priv->hal = NULL;
	kfree(hal);
	return ret;
}

/**
 * vnic_pci_hal_destroy() - undo vnic_pci_hal_create(): free both rings and hal
 * @priv: netdev private data; priv->hal is cleared here
 *
 * BAR0, the IRQ vector and the enabled-device state are devres-managed and
 * released automatically when the driver detaches; only the rings and @hal
 * itself need freeing here.
 */
static void vnic_pci_hal_destroy(struct vnic_priv *priv)
{
	struct vnic_hal *hal = priv->hal;

	vnic_hal_ring_free(hal, &priv->rx_ring);
	vnic_hal_ring_free(hal, &priv->tx_ring);
	priv->hal = NULL;
	kfree(hal);
}

/**
 * vnic_pci_probe - the bus enumerated a 1234:11e9 device
 * @pdev: the PCI device
 * @id:   the matching vnic_pci_ids[] entry
 *
 * alloc_etherdev() first, the vnic-engine.c shape. vnic_pci_hal_create() then
 * brings the hardware up and wires priv->hal; register_netdev() last.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int vnic_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct net_device *netdev;
	struct vnic_priv *priv;
	int ret;

	netdev = alloc_etherdev(sizeof(struct vnic_priv));
	if (!netdev)
		return -ENOMEM;

	strscpy(netdev->name, "vnic%d", sizeof(netdev->name));
	netdev->netdev_ops = &vnic_netdev_ops;
	eth_hw_addr_random(netdev);
	netdev->min_mtu = VN_MTU_MIN;
	netdev->max_mtu = VN_MTU_MAX;

	priv = netdev_priv(netdev);
	priv->dev = netdev;
	netif_napi_add(netdev, &priv->napi, vnic_poll);

	pci_set_drvdata(pdev, netdev);

	ret = vnic_pci_hal_create(priv, pdev);
	if (ret)
		goto err_free_netdev;

	ret = register_netdev(netdev);
	if (ret) {
		netdev_err(netdev, "failed to register netdev: %d\n", ret);
		goto err_destroy_hal;
	}

	netdev_info(netdev, "VN1000 loaded, real PCI/MMIO/MSI backend (ID=0x%08x)\n",
		    vnic_hal_rd32(priv->hal, VN_REG_ID));
	return 0;

err_destroy_hal:
	vnic_pci_hal_destroy(priv);
err_free_netdev:
	free_netdev(netdev);		/* also calls netif_napi_del() */
	return ret;
}

/**
 * vnic_pci_remove() - device gone / driver unloaded
 * @pdev: the PCI device being removed
 *
 * Reverse of vnic_pci_probe(): unregister the netdev (closes it first if
 * still up), tear down the hal and rings, free the netdev.
 */
static void vnic_pci_remove(struct pci_dev *pdev)
{
	struct net_device *netdev = pci_get_drvdata(pdev);
	struct vnic_priv *priv = netdev_priv(netdev);

	unregister_netdev(netdev);   /* closes the device first if it is up */
	vnic_pci_hal_destroy(priv);
	free_netdev(netdev);
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
