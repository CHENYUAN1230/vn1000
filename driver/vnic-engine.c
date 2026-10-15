// SPDX-License-Identifier: GPL-2.0
/* vnic-engine.c - the in-kernel engine build of the VN1000 driver
 *
 * The build-specific half for VNIC_BACKEND=engine: a kthread plays the role of
 * the NIC. Contains struct vnic_hal (opaque to vnic-core.c), the vnic-hal.h
 * implementation, and this build's module entry point. The PCI build uses
 * vnic-pci.c instead.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>          /* kzalloc, kcalloc, kfree */
#include <linux/errno.h>
#include <linux/err.h>           /* ERR_PTR, IS_ERR, PTR_ERR */
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/interrupt.h>     /* tasklet */
#include <linux/string.h>        /* strscpy */
#include <linux/netdevice.h>
#include <linux/etherdevice.h>   /* alloc_etherdev, eth_hw_addr_random, ETH_ALEN */
#include <linux/ip.h>            /* struct iphdr */
#include <linux/skbuff.h>

#include "vnic.h"
#include "vnic-hal.h"

/**
 * struct vnic_hal - private state of the in-kernel engine backend
 * @priv: back-pointer to the core; engine_run() needs its rings
 * @regs: fake BAR0 register file
 * @reg_lock: guards the read-modify-write on ICR / IMS
 * @wq: the engine kthread waits here for the doorbell
 * @engine: the kthread that plays "the device"
 * @run_lock: serialises one engine run against vnic_hal_quiesce()
 * @doorbell: set when TDT / RDT is written
 * @intr_tasklet: the fake ISR bottom half; engine_run() schedules it, it
 *                schedules NAPI. The PCI backend has a real hardirq ISR instead.
 *
 * Opaque to vnic-core.c: it only ever holds a struct vnic_hal *, so every field
 * that is really "the hardware" lives here rather than in struct vnic_priv.
 */
struct vnic_hal {
	struct vnic_priv     *priv;
	u32                   regs[VN_BAR0_SIZE / 4];
	spinlock_t            reg_lock;
	wait_queue_head_t     wq;
	struct task_struct   *engine;
	struct mutex          run_lock;
	bool                  doorbell;
	struct tasklet_struct intr_tasklet;
};

/* internal: kthread body, tasklet, loopback rewrite, lifecycle helpers */
static void engine_run(struct vnic_hal *hal);
static int  engine_thread(void *data);
static void engine_intr_tasklet(struct tasklet_struct *t);
static void engine_flip_addr(struct sk_buff *skb, u32 len);
static struct vnic_hal *engine_create(struct vnic_priv *priv);
static void engine_stop(struct vnic_hal *hal);
static void engine_destroy(struct vnic_hal *hal);

/* register access */

/**
 * vnic_hal_rd32() - read a 32-bit device register
 * @hal: backend handle
 * @off: register byte offset (VN_REG_*)
 *
 * The core's only path to a register read. This backend backs registers with a
 * plain array; ICR is read-to-clear, emulated here under @hal->reg_lock.
 *
 * Return: the register value.
 */
u32 vnic_hal_rd32(struct vnic_hal *hal, u32 off)
{
	unsigned long flags;
	u32 val;

	if (off == VN_REG_ICR) {
		spin_lock_irqsave(&hal->reg_lock, flags);
		val = hal->regs[off / 4];
		hal->regs[off / 4] = 0;
		spin_unlock_irqrestore(&hal->reg_lock, flags);
		return val;
	}

	val = hal->regs[off / 4];
	return val;
}

/**
 * vnic_hal_wr32() - write a 32-bit device register
 * @hal: backend handle
 * @off: register byte offset (VN_REG_*)
 * @val: value to write
 *
 * The core's only path to a register write, emulating what real hardware does
 * on the bus write: ICS ORs into ICR; IMS / IMC set / clear the interrupt
 * mask; CTRL.RST zeroes TDH/RDH and self-clears; TDT / RDT ring the doorbell
 * and wake the engine; anything else is a plain store.
 */
void vnic_hal_wr32(struct vnic_hal *hal, u32 off, u32 val)
{
	unsigned long flags;

	if (off == VN_REG_ICS) {
		spin_lock_irqsave(&hal->reg_lock, flags);
		hal->regs[VN_REG_ICR / 4] |= val;
		spin_unlock_irqrestore(&hal->reg_lock, flags);
	} else if (off == VN_REG_IMS) {
		spin_lock_irqsave(&hal->reg_lock, flags);
		hal->regs[VN_REG_IMS / 4] |= val;
		spin_unlock_irqrestore(&hal->reg_lock, flags);
	} else if (off == VN_REG_IMC) {
		spin_lock_irqsave(&hal->reg_lock, flags);
		hal->regs[VN_REG_IMS / 4] &= ~val;
		spin_unlock_irqrestore(&hal->reg_lock, flags);
	} else if (off == VN_REG_CTRL) {
		spin_lock_irqsave(&hal->reg_lock, flags);
		if (val & VN_CTRL_RST) {
			hal->regs[VN_REG_TDH / 4] = 0;
			hal->regs[VN_REG_RDH / 4] = 0;
			val &= ~VN_CTRL_RST;        /* self-clearing */
		}
		hal->regs[VN_REG_CTRL / 4] = val;
		spin_unlock_irqrestore(&hal->reg_lock, flags);
	} else {
		hal->regs[off / 4] = val;
		if (off == VN_REG_TDT || off == VN_REG_RDT) {
			hal->doorbell = true;
			wake_up_interruptible(&hal->wq);
		}
	}
}

/* descriptor ring memory */

/**
 * vnic_hal_ring_alloc() - allocate one TX or RX descriptor ring
 * @hal: backend handle (unused by this backend; kept for the PCI backend)
 * @ring: ring to populate (->desc, ->bufs, ->count, ->lock)
 * @count: number of descriptors
 *
 * How the ring memory is obtained is the backend's business - this backend uses
 * kcalloc, the PCI backend will use dma_alloc_coherent().
 *
 * Return: 0 on success, -ENOMEM on failure (the ring is left freed).
 */
int vnic_hal_ring_alloc(struct vnic_hal *hal, struct vnic_ring *ring, u32 count)
{
	int err = 0;

	ring->desc = kcalloc(count, sizeof(struct vn_desc), GFP_KERNEL);
	if (!ring->desc) {
		err = -ENOMEM;
		goto rollback;
	}

	ring->bufs = kcalloc(count, sizeof(struct sk_buff *), GFP_KERNEL);
	if (!ring->bufs) {
		err = -ENOMEM;
		goto rollback;
	}

	ring->count = count;
	ring->next_to_use = 0;
	ring->next_to_clean = 0;
	spin_lock_init(&ring->lock);
	return 0;

rollback:
	vnic_hal_ring_free(hal, ring);
	return err;
}

/**
 * vnic_hal_ring_free() - free a ring allocated by vnic_hal_ring_alloc()
 * @hal: backend handle
 * @ring: ring to free; ->desc and ->bufs are set to NULL
 */
void vnic_hal_ring_free(struct vnic_hal *hal, struct vnic_ring *ring)
{
	if (ring->desc) {
		kfree(ring->desc);
		ring->desc = NULL;
	}
	if (ring->bufs) {
		kfree(ring->bufs);
		ring->bufs = NULL;
	}
}

/* per-packet DMA mapping
 *
 * This backend has no bus and no IOMMU, so these are trivial - but the
 * signatures match what the PCI backend needs (a direction, a failure path) so
 * Phase 3 can swap only the backend.
 */

/**
 * vnic_hal_map() - lend a buffer to the device
 * @hal: backend handle
 * @va: kernel virtual address of the buffer
 * @len: buffer length
 * @dir: DMA direction
 *
 * This backend returns the pointer as an identity address; the PCI backend
 * uses dma_map_single().
 *
 * Return: the address the device should use; test it with
 * vnic_hal_mapping_error().
 */
dma_addr_t vnic_hal_map(struct vnic_hal *hal, void *va, size_t len,
			enum dma_data_direction dir)
{
	return (dma_addr_t)(uintptr_t)va;
}

/**
 * vnic_hal_unmap() - undo vnic_hal_map()
 * @hal: backend handle
 * @dma: address returned by vnic_hal_map()
 * @len: length passed to vnic_hal_map()
 * @dir: direction passed to vnic_hal_map()
 *
 * No-op in this backend; dma_unmap_single() in the PCI backend.
 */
void vnic_hal_unmap(struct vnic_hal *hal, dma_addr_t dma, size_t len,
		    enum dma_data_direction dir)
{
}

/**
 * vnic_hal_mapping_error() - test a vnic_hal_map() result
 * @hal: backend handle
 * @dma: address returned by vnic_hal_map()
 *
 * Return: non-zero if the mapping failed; always 0 in this backend.
 */
int vnic_hal_mapping_error(struct vnic_hal *hal, dma_addr_t dma)
{
	return 0;
}

/* lifecycle */

/**
 * vnic_hal_quiesce() - wait until the device is idle and off the rings
 * @hal: backend handle
 *
 * Called by vnic_release() before draining the rings. Waits out an engine run
 * already in progress; the caller must have already stopped feeding the
 * doorbell (TX and NAPI disabled), so no new run can start.
 *
 * Context: process context, may sleep.
 */
void vnic_hal_quiesce(struct vnic_hal *hal)
{
	mutex_lock(&hal->run_lock);
	mutex_unlock(&hal->run_lock);
}

/**
 * vnic_hal_irq_request() - arm the completion interrupt (ndo_open)
 * @hal: backend handle
 *
 * Sets up the tasklet that engine_run() schedules as the fake ISR bottom half.
 * The PCI backend does request_irq() here instead.
 *
 * Return: 0 (this backend cannot fail).
 */
int vnic_hal_irq_request(struct vnic_hal *hal)
{
	tasklet_setup(&hal->intr_tasklet, engine_intr_tasklet);
	return 0;
}

/**
 * vnic_hal_irq_free() - disarm the completion interrupt (ndo_stop)
 * @hal: backend handle
 *
 * tasklet_kill() flushes a bottom half already scheduled. Called after
 * vnic_hal_quiesce() so engine_run() will not reschedule it.
 */
void vnic_hal_irq_free(struct vnic_hal *hal)
{
	tasklet_kill(&hal->intr_tasklet);
}

/* engine_create - allocate the backend, seed the RO registers, allocate the
 * rings, start the kthread
 *
 * Called from this file's module_init, once netdev/priv already exist. Fills
 * ID/VERSION/CAP + the initial IMS mask, allocates the TX/RX rings, then
 * starts the engine kthread last: it parks on the doorbell wq until the first
 * TDT / RDT write, so nothing above it can race it, and if kthread_run()
 * fails there is nothing left running that needs to be torn back down.
 *
 * Return: the handle, or an ERR_PTR() on failure. Context: may sleep.
 */
static struct vnic_hal *engine_create(struct vnic_priv *priv)
{
	int ret;
	struct vnic_hal *hal;

	hal = kzalloc(sizeof(*hal), GFP_KERNEL);
	if (!hal)
		return ERR_PTR(-ENOMEM);

	hal->priv = priv;
	spin_lock_init(&hal->reg_lock);
	mutex_init(&hal->run_lock);
	init_waitqueue_head(&hal->wq);

	/* RO identity + initial interrupt mask, before the engine ever runs */
	vnic_hal_wr32(hal, VN_REG_ID, VN_ID_MAGIC);
	vnic_hal_wr32(hal, VN_REG_VERSION, VN_VERSION_1_0);
	vnic_hal_wr32(hal, VN_REG_CAP, VN_CAP_MSI | VN_CAP_MODER);
	vnic_hal_wr32(hal, VN_REG_IMS,
		      VN_INT_TXDONE | VN_INT_RXAVAIL | VN_INT_RXEMPTY | VN_INT_ERR);

	ret = vnic_hal_ring_alloc(hal, &priv->tx_ring, VNIC_TX_RING_SIZE);
	if (ret)
		goto err_free_hal;

	ret = vnic_hal_ring_alloc(hal, &priv->rx_ring, VNIC_RX_RING_SIZE);
	if (ret)
		goto err_free_tx_ring;

	hal->engine = kthread_run(engine_thread, hal, "vnic-engine");
	if (IS_ERR(hal->engine)) {
		ret = PTR_ERR(hal->engine);
		goto err_free_rx_ring;
	}

	return hal;

err_free_rx_ring:
	vnic_hal_ring_free(hal, &priv->rx_ring);
err_free_tx_ring:
	vnic_hal_ring_free(hal, &priv->tx_ring);
err_free_hal:
	kfree(hal);
	return ERR_PTR(ret);
}

/* engine_stop - kthread_stop the engine; the handle stays valid
 *
 * Teardown order is engine_stop() -> engine_destroy(): the engine must be off
 * the rings before engine_destroy() frees their memory.
 */
static void engine_stop(struct vnic_hal *hal)
{
	if (hal->engine)
		kthread_stop(hal->engine);
}

/* engine_destroy - free the rings and the handle. Last teardown step.
 *
 * Only ever called on a fully-built hal (past engine_create()'s own internal
 * ladder), so both rings are always present here.
 */
static void engine_destroy(struct vnic_hal *hal)
{
	vnic_hal_ring_free(hal, &hal->priv->rx_ring);
	vnic_hal_ring_free(hal, &hal->priv->tx_ring);
	kfree(hal);
}

/* internal: the fake device */

/**
 * engine_intr_tasklet() - the fake ISR bottom half
 * @t: the tasklet embedded in struct vnic_hal as @intr_tasklet;
 *     from_tasklet() recovers the containing struct vnic_hal *
 *
 * engine_run() schedules this after a pass. It reads the interrupt cause,
 * checks it against the mask, and (if a TX/RX-relevant cause is set and
 * unmasked) closes those mask bits and schedules NAPI. ERR alone does not wake
 * NAPI: an OOM is not something a poll can fix.
 */
static void engine_intr_tasklet(struct tasklet_struct *t)
{
	struct vnic_hal *hal = from_tasklet(hal, t, intr_tasklet);
	struct vnic_priv *priv = hal->priv;
	u32 cause = vnic_hal_rd32(hal, VN_REG_ICR);
	u32 mask = vnic_hal_rd32(hal, VN_REG_IMS);

	if (cause & mask & ~VN_INT_ERR) {
		if (napi_schedule_prep(&priv->napi)) {
			vnic_hal_wr32(hal, VN_REG_IMC,
				      VN_INT_TXDONE | VN_INT_RXAVAIL | VN_INT_RXEMPTY);
			PDEBUG("engine_intr_tasklet: masked TX/RX/RXEMPTY, mask now=0x%x\n",
			       vnic_hal_rd32(hal, VN_REG_IMS));
			__napi_schedule(&priv->napi);
		}
		PDEBUG("engine_intr_tasklet: cause=0x%x, mask=0x%x\n", cause, mask);
	}
}

/* engine_run - loop [TDH, TDT) TX frames back into the RX ring
 *
 * This is "the device". For each pending TX descriptor: copy the frame into the
 * next free RX slot, swap the MAC (and the IPv4 addresses), set DD on both
 * sides, update the byte / packet stats, and raise TXDONE / RXAVAIL / RXEMPTY
 * via ICS. A full RX ring stalls on that TX descriptor - backpressure, no drop.
 */
static void engine_run(struct vnic_hal *hal)
{
	u32 ctrl = vnic_hal_rd32(hal, VN_REG_CTRL);
	if ((ctrl & (VN_CTRL_TXEN | VN_CTRL_RXEN)) != (VN_CTRL_TXEN | VN_CTRL_RXEN))
		return;

	struct vnic_priv *priv;
	struct vnic_ring *tx_ring;
	struct vnic_ring *rx_ring;
	u32 th, tt, txp, txb;
	u32 rh, rt, rxp, rxb;
	u32 rxdrop;
	u32 th_start, rh_start;

	priv = hal->priv;

	tx_ring = &priv->tx_ring;
	th = vnic_hal_rd32(hal, VN_REG_TDH);
	tt = vnic_hal_rd32(hal, VN_REG_TDT);
	txp = vnic_hal_rd32(hal, VN_REG_STAT_TXPKTS);
	txb = vnic_hal_rd32(hal, VN_REG_STAT_TXBYTES);

	rx_ring = &priv->rx_ring;
	rxp = vnic_hal_rd32(hal, VN_REG_STAT_RXPKTS);
	rxb = vnic_hal_rd32(hal, VN_REG_STAT_RXBYTES);
	rxdrop = vnic_hal_rd32(hal, VN_REG_STAT_RXDROP);
	th_start = th;

	/* No tx_ring->lock here: that lock only serialises vnic_tx() against
	 * vnic_tx_poll(), not the engine, which is excluded from tx_ring only by
	 * the run_lock quiesce in vnic_release(). Per-descriptor handoff is
	 * lock-free: DD bit + dma_wmb() / dma_rmb().
	 */
	spin_lock_bh(&rx_ring->lock); /* excludes this section from vnic_rx_drain() */
	rh = vnic_hal_rd32(hal, VN_REG_RDH);
	rt = vnic_hal_rd32(hal, VN_REG_RDT);
	rh_start = rh;

	while (th != tt) {
		/* no posted buffer (rh == rt) or not yet consumed (DD): RXEMPTY */
		if (rh == rt || (READ_ONCE(rx_ring->desc[rh].status) & VN_DESC_DD)) {
			PDEBUG("engine_run: rx ring full at rh=%u, stalling th=%u\n", rh, th);
			vnic_hal_wr32(hal, VN_REG_ICS, VN_INT_RXEMPTY);
			break;
		}
		dma_rmb(); /* pairs with wmb() in vnic_rx_poll() before it clears DD */

		/* frames too short to hold an Ethernet header can't be looped
		 * back; skip th without touching rh
		 */
		if (tx_ring->bufs[th]->len < sizeof(struct ethhdr)) {
			PDEBUG("engine_run: loopback dropped frame at th=%u (len=%u too short)\n", th, tx_ring->bufs[th]->len);
			rxdrop++;
		} else {
			/* real hardware DMAs from desc->addr; the PCI backend replaces this memcpy */
			memcpy(rx_ring->bufs[rh]->data, tx_ring->bufs[th]->data, tx_ring->bufs[th]->len);
			engine_flip_addr(rx_ring->bufs[rh], tx_ring->bufs[th]->len);
			rx_ring->desc[rh].len = tx_ring->desc[th].len;
			rxp++;
			rxb += rx_ring->desc[rh].len;
			dma_wmb(); /* data + len visible before DD says "ready" */
			WRITE_ONCE(rx_ring->desc[rh].status, rx_ring->desc[rh].status | VN_DESC_DD);
			rh = (rh + 1) % rx_ring->count;
		}

		dma_wmb(); /* pairs with rmb() in vnic_tx_poll() */
		WRITE_ONCE(tx_ring->desc[th].status, tx_ring->desc[th].status | VN_DESC_DD);
		txp++;
		txb += tx_ring->desc[th].len;
		th = (th + 1) % tx_ring->count;
	}

	vnic_hal_wr32(hal, VN_REG_RDH, rh);
	vnic_hal_wr32(hal, VN_REG_STAT_RXPKTS, rxp);
	vnic_hal_wr32(hal, VN_REG_STAT_RXBYTES, rxb);
	vnic_hal_wr32(hal, VN_REG_STAT_RXDROP, rxdrop);
	spin_unlock_bh(&rx_ring->lock);

	if (th != th_start) {
		PDEBUG("engine_run: processed th %u->%u, txp=%u rxp=%u rxdrop=%u\n", th_start, th, txp, rxp, rxdrop);
		vnic_hal_wr32(hal, VN_REG_ICS, VN_INT_TXDONE);
	}
	if (rh != rh_start)
		vnic_hal_wr32(hal, VN_REG_ICS, VN_INT_RXAVAIL);

	/* tx ring: no lock needed (see above); run_lock excludes vnic_tx_drain() */
	vnic_hal_wr32(hal, VN_REG_TDH, th);
	vnic_hal_wr32(hal, VN_REG_STAT_TXPKTS, txp);
	vnic_hal_wr32(hal, VN_REG_STAT_TXBYTES, txb);
	tasklet_schedule(&hal->intr_tasklet);
}

/* engine_thread - kthread main loop: wait for the doorbell, run one pass,
 * repeat until kthread_stop(). run_lock lets vnic_hal_quiesce() wait out a
 * pass in progress.
 */
static int engine_thread(void *data)
{
	struct vnic_hal *hal = data;

	while (!kthread_should_stop()) {
		wait_event_interruptible(hal->wq, hal->doorbell || kthread_should_stop());
		hal->doorbell = false;
		if (kthread_should_stop())
			break;
		mutex_lock(&hal->run_lock);
		engine_run(hal);
		mutex_unlock(&hal->run_lock);
	}
	return 0;
}

/* engine_flip_addr - loopback header rewrite: swap Ethernet src / dst (so
 * eth_type_trans() sees PACKET_HOST) and, for IPv4, swap the addresses and
 * rebuild the header checksum. Non-IPv4 (ARP, IPv6 ND) gets only the MAC swap.
 *
 * engine_run() calls this before skb_put(skb, len): skb_put() itself happens
 * later, in the shared vnic_rx_poll(), so skb->len is still 0 here and can't
 * be used to tell how much of the buffer is real frame data. @len is that
 * same length, passed in directly instead.
 */
static void engine_flip_addr(struct sk_buff *skb, u32 len)
{
	struct ethhdr *eth = (struct ethhdr *)skb->data;
	u8 eth_tmp[ETH_ALEN];

	memcpy(eth_tmp, eth->h_source, ETH_ALEN);
	memcpy(eth->h_source, eth->h_dest, ETH_ALEN);
	memcpy(eth->h_dest, eth_tmp, ETH_ALEN);

	if (eth->h_proto == htons(ETH_P_IP) &&
	    len >= sizeof(struct ethhdr) + sizeof(struct iphdr)) {
		struct iphdr *iph = (struct iphdr *)(skb->data + sizeof(struct ethhdr));
		u32 ip_tmp = iph->saddr;

		iph->saddr = iph->daddr;
		iph->daddr = ip_tmp;
		iph->check = 0;
		iph->check = ip_fast_csum((unsigned char *)iph, iph->ihl);
	}
}

/* ======================================================================
 * module entry: build the vnic0 netdev on the in-kernel engine backend
 * ==================================================================== */

/* Non-NULL iff a registered vnic0 exists (set only after init fully succeeds). */
static struct net_device *vnic_netdev;

/*
 * __init / __exit: section markers. __init code lands in .init.text, which the
 * kernel frees once the module has finished loading (the init function runs
 * once). __exit code lands in .exit.text, kept for rmmod but dropped entirely
 * if the kernel is built without module-unload support. module_init() pairs
 * with an __init function, module_exit() with __exit.
 */

/**
 * vnic_engine_init() - module_init: build and register vnic0
 *
 * alloc_etherdev() the netdev, engine_create() the backend, register_netdev().
 * On any failure, unwinds exactly what had already succeeded.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int __init vnic_engine_init(void)
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

	priv->hal = engine_create(priv);
	if (IS_ERR(priv->hal)) {
		ret = PTR_ERR(priv->hal);
		goto err_free_netdev;
	}

	ret = register_netdev(netdev);
	if (ret) {
		netdev_err(netdev, "failed to register netdev: %d\n", ret);
		goto err_destroy_hal;
	}

	vnic_netdev = netdev;
	netdev_info(netdev, "VN1000 loaded, in-kernel engine backend (ID=0x%08x)\n",
		    vnic_hal_rd32(priv->hal, VN_REG_ID));
	return 0;

err_destroy_hal:
	engine_stop(priv->hal);
	engine_destroy(priv->hal);
err_free_netdev:
	free_netdev(netdev);		/* also calls netif_napi_del() */
	return ret;
}

/**
 * vnic_engine_exit() - module_exit: tear vnic0 back down
 *
 * Only reached if vnic_engine_init() returned 0, so vnic_netdev is non-NULL.
 * Reverse of vnic_engine_init(): unregister, stop the kthread, free the
 * handle and rings, free the netdev.
 */
static void __exit vnic_engine_exit(void)
{
	struct vnic_priv *priv = netdev_priv(vnic_netdev);

	unregister_netdev(vnic_netdev);
	engine_stop(priv->hal);
	engine_destroy(priv->hal);
	free_netdev(vnic_netdev);
}

module_init(vnic_engine_init);
module_exit(vnic_engine_exit);

MODULE_AUTHOR("Yu An Chen");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("VN1000 virtual NIC, in-kernel engine backend");
