// SPDX-License-Identifier: GPL-2.0
/* vnic-hal-engine.c - in-kernel engine HAL backend
 *
 * The baseline backend: a kthread plays the role of the NIC. The core driver
 * reaches it only through the vnic-hal.h interface; struct vnic_hal is defined
 * here and is opaque to the core. The PCI/DMA/MSI backend replaces this file
 * from Phase 2 on.
 */

#include <linux/kernel.h>
#include <linux/slab.h>          /* kzalloc, kcalloc, kfree */
#include <linux/errno.h>
#include <linux/err.h>           /* ERR_PTR, IS_ERR, PTR_ERR */
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/interrupt.h>     /* tasklet_schedule */
#include <linux/netdevice.h>
#include <linux/etherdevice.h>   /* eth_type_trans, ETH_ALEN */
#include <linux/ip.h>            /* struct iphdr */
#include <linux/skbuff.h>

#include "vnic.h"
#include "vnic-hal.h"

/**
 * struct vnic_hal - private state of the in-kernel engine backend
 * @priv: back-pointer to the core; engine_run() needs its rings and tasklet
 * @regs: fake BAR0 register file
 * @reg_lock: guards the read-modify-write on ICR / IMS
 * @wq: the engine kthread waits here for the doorbell
 * @engine: the kthread that plays "the device"
 * @run_lock: serialises one engine run against vnic_hal_quiesce()
 * @doorbell: set when TDT / RDT is written
 *
 * The core only passes this pointer around, never looks inside, so every field
 * that is really "the hardware" lives here rather than in struct vnic_priv.
 */
struct vnic_hal {
	struct vnic_priv   *priv;
	u32                 regs[VN_BAR0_SIZE / 4];
	spinlock_t          reg_lock;
	wait_queue_head_t   wq;
	struct task_struct *engine;
	struct mutex        run_lock;
	bool                doorbell;
};

/* internal: kthread body + loopback header rewrite */
static void engine_run(struct vnic_hal *hal);
static int  engine_thread(void *data);
static void vnic_tx_flip_addr(struct sk_buff *skb);

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
 * on the bus write: ICS ORs into ICR; IMS / IMC set / clear the interrupt mask;
 * TDT / RDT ring the doorbell and wake the engine; anything else is a plain
 * store.
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
	} else {
		hal->regs[off / 4] = val;
		if (off == VN_REG_TDT || off == VN_REG_RDT) {
			hal->doorbell = true;
			wake_up_interruptible(&hal->wq);
		}
	}
}

/**
 * vnic_hal_wr32_raw() - write a register with no side effects
 * @hal: backend handle
 * @off: register byte offset (VN_REG_*)
 * @val: value to write
 *
 * Used by the drain paths to reset TDH / TDT / RDH / RDT without ringing the
 * TDT / RDT doorbell. Phase 1 approach; a CTRL.TXEN / RXEN gate is deferred to
 * Phase 3.
 */
void vnic_hal_wr32_raw(struct vnic_hal *hal, u32 off, u32 val)
{
	hal->regs[off / 4] = val;
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
 * vnic_hal_create() - allocate and start the in-kernel engine backend
 * @priv: owning core device, stored as a back-pointer
 *
 * The one place the core names a concrete backend. Fills the RO identity
 * registers and the initial interrupt mask, then starts the engine kthread,
 * which parks on the doorbell until the interface is brought up.
 *
 * Context: process context, may sleep.
 * Return: the backend handle, or an ERR_PTR() on failure.
 */
struct vnic_hal *vnic_hal_create(struct vnic_priv *priv)
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

	/* start the engine last: it parks on the doorbell wq until the first
	 * TDT / RDT write, so nothing above can race it
	 */
	hal->engine = kthread_run(engine_thread, hal, "vnic-engine");
	if (IS_ERR(hal->engine)) {
		ret = PTR_ERR(hal->engine);
		goto err_free_hal;
	}

	return hal;

err_free_hal:
	kfree(hal);
	return ERR_PTR(ret);
}

/**
 * vnic_hal_stop() - stop the engine without freeing the handle
 * @hal: backend handle
 *
 * Teardown order is stop -> vnic_hal_ring_free() -> vnic_hal_destroy(): the
 * engine must be off the rings before their memory goes away, and @hal must
 * stay valid for the ring_free calls.
 */
void vnic_hal_stop(struct vnic_hal *hal)
{
	if (hal->engine)
		kthread_stop(hal->engine);
}

/**
 * vnic_hal_destroy() - free the backend handle
 * @hal: backend handle
 *
 * Final teardown step; call only after vnic_hal_stop() and any
 * vnic_hal_ring_free().
 */
void vnic_hal_destroy(struct vnic_hal *hal)
{
	kfree(hal);
}

/* internal: the fake device */

/* engine_run - loop [TDH, TDT) TX frames back into the RX ring
 *
 * This is "the device". For each pending TX descriptor: copy the frame into the
 * next free RX slot, swap the MAC (and the IPv4 addresses), set DD on both
 * sides, update the byte / packet stats, and raise TXDONE / RXAVAIL / RXEMPTY
 * via ICS. A full RX ring stalls on that TX descriptor - backpressure, no drop.
 */
static void engine_run(struct vnic_hal *hal)
{
	struct vnic_priv *priv = hal->priv;
	struct vnic_ring *tx_ring = &priv->tx_ring;
	u32 th = vnic_hal_rd32(hal, VN_REG_TDH);
	u32 tt = vnic_hal_rd32(hal, VN_REG_TDT);
	u32 txp = vnic_hal_rd32(hal, VN_REG_STAT_TXPKTS);
	u32 txb = vnic_hal_rd32(hal, VN_REG_STAT_TXBYTES);

	struct vnic_ring *rx_ring = &priv->rx_ring;
	u32 rh, rt;
	u32 rxp = vnic_hal_rd32(hal, VN_REG_STAT_RXPKTS);
	u32 rxb = vnic_hal_rd32(hal, VN_REG_STAT_RXBYTES);
	u32 rxdrop = vnic_hal_rd32(hal, VN_REG_STAT_RXDROP);
	u32 th_start = th;
	u32 rh_start;

	/* No tx_ring->lock here: that lock only serialises vnic_tx() against
	 * vnic_tx_poll(), not the engine, which is excluded from tx_ring only by
	 * the run_lock quiesce in vnic_release(). Per-descriptor handoff is
	 * lock-free: DD bit + dma_wmb() / dma_rmb().
	 */
	spin_lock_bh(&rx_ring->lock); /* excludes this section from vnic_rx_drain() */
	rh = vnic_hal_rd32(hal, VN_REG_RDH);
	rt = vnic_hal_rd32(hal, VN_REG_RDT);
	rh_start = rh;

	while (th != tt && rh != rt) { /* one slot reserved for full vs. empty */
		if (READ_ONCE(rx_ring->desc[rh].status) & VN_DESC_DD) {
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
			skb_put(rx_ring->bufs[rh], tx_ring->bufs[th]->len);
			vnic_tx_flip_addr(rx_ring->bufs[rh]);
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
	tasklet_schedule(&priv->intr_tasklet);
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

/* vnic_tx_flip_addr - loopback header rewrite: swap Ethernet src / dst (so
 * eth_type_trans() sees PACKET_HOST) and, for IPv4, swap the addresses and
 * rebuild the header checksum. Non-IPv4 (ARP, IPv6 ND) gets only the MAC swap.
 */
static void vnic_tx_flip_addr(struct sk_buff *skb)
{
	struct ethhdr *eth = (struct ethhdr *)skb->data;
	u8 eth_tmp[ETH_ALEN];

	memcpy(eth_tmp, eth->h_source, ETH_ALEN);
	memcpy(eth->h_source, eth->h_dest, ETH_ALEN);
	memcpy(eth->h_dest, eth_tmp, ETH_ALEN);

	if (eth->h_proto == htons(ETH_P_IP) &&
	    skb->len >= sizeof(struct ethhdr) + sizeof(struct iphdr)) {
		struct iphdr *iph = (struct iphdr *)(skb->data + sizeof(struct ethhdr));
		u32 ip_tmp = iph->saddr;

		iph->saddr = iph->daddr;
		iph->daddr = ip_tmp;
		iph->check = 0;
		iph->check = ip_fast_csum((unsigned char *)iph, iph->ihl);
	}
}
