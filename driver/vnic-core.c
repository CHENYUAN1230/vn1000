// SPDX-License-Identifier: GPL-2.0
/* vnic-core.c - VN1000 netdev datapath
 *
 * The shared half: netdev ops, descriptor rings, NAPI, TX/RX, ownership
 * handshakes. Reaches "the device" only through vnic-hal.h. No module entry
 * point here; that lives in the build-specific half (vnic-engine.c / vnic-pci.c).
 *
 * TODO: batch refill - accumulate N completions before refilling the rx ring;
 * ring the TX doorbell only every N packets.
 */

#include <linux/kernel.h>	/* printk() */
#include <linux/errno.h>	/* error codes */
#include <linux/types.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>	/* eth_type_trans */
#include <linux/ip.h>		/* struct iphdr */
#include <linux/skbuff.h>

#include "vnic.h"
#include "vnic-hal.h"

/* Forward declarations, so functions can be grouped by role (ring, open/close,
 * tx, rx) instead of by definition order.
 */
static int vnic_open(struct net_device *dev);
static int vnic_release(struct net_device *dev);
static void vnic_tx_poll(struct vnic_priv *priv, int budget);
static int vnic_tx_drain(struct vnic_priv *priv);
static int vnic_rx_buf_alloc(struct vnic_priv *priv, struct vnic_ring *rx_ring, u32 idx);
static int vnic_rx_post(struct vnic_priv *priv);
static int vnic_rx_drain(struct vnic_priv *priv);
static int vnic_rx_poll(struct vnic_priv *priv, int budget);
static int vnic_tx(struct sk_buff *skb, struct net_device *dev);
static void vnic_get_stats64(struct net_device *dev, struct rtnl_link_stats64 *stats);
static int vnic_change_mtu(struct net_device *dev, int new_mtu);

/*
 * Open and close
 */

static int vnic_open(struct net_device *dev)
{
	int ret;
	struct vnic_priv *priv = netdev_priv(dev);

	priv->rx_buf_size = dev->mtu + VN_FRAME_OVERHEAD; /* mtu might change */

	ret = vnic_rx_post(priv);
	if (ret)
		goto err_drain_rx;

	ret = vnic_hal_irq_request(priv->hal); /* engine: tasklet_setup   pci: request_irq */
	if (ret)
		goto err_drain_rx;

	PDEBUG("vnic_open: posted %u rx buffers, opening queue\n", priv->rx_ring.next_to_use);
	napi_enable(&priv->napi);
	/* re-arm: vnic_release() masked everything off */
	vnic_hal_wr32(priv->hal, VN_REG_IMS,
		      VN_INT_TXDONE | VN_INT_RXAVAIL | VN_INT_RXEMPTY | VN_INT_ERR);
	netif_start_queue(dev);
	return 0;

err_drain_rx:
	vnic_rx_drain(priv);
	return ret;
}

static int vnic_release(struct net_device *dev)
{
	struct vnic_priv *priv = netdev_priv(dev);

	netif_tx_disable(dev); /* stop the queue and wait out any in-flight vnic_tx() */
	PDEBUG("vnic_release: closing, draining rx ring\n");
	napi_disable(&priv->napi); /* wait out any in-flight vnic_rx_poll() before draining */
	/* At this point nothing can ring TDT/RDT again (TX disabled, NAPI disabled), so
	 * this quiesce only has to wait out a run already in progress - no separate
	 * enable flag needed to block a future one.
	 */
	vnic_hal_quiesce(priv->hal);
	vnic_hal_irq_free(priv->hal); /* engine: tasklet_kill   pci: free_irq (engine parked, no run will reschedule) */
	/* no poll left to re-arm IMS */
	vnic_hal_wr32(priv->hal, VN_REG_IMC,
		      VN_INT_TXDONE | VN_INT_RXAVAIL | VN_INT_RXEMPTY | VN_INT_ERR);
	vnic_tx_drain(priv);
	vnic_rx_drain(priv);

	return 0;
}

/*
 * clean tx ring, return the skb's memory
 */
static void vnic_tx_poll(struct vnic_priv *priv, int budget)
{
	struct vnic_ring *tx_ring = &priv->tx_ring;

	spin_lock(&tx_ring->lock); /* NAPI runs in softirq context; no _bh against vnic_tx()'s spin_lock_bh() */
	while (tx_ring->next_to_clean != tx_ring->next_to_use) {
		if (!(READ_ONCE(tx_ring->desc[tx_ring->next_to_clean].status) & VN_DESC_DD))
			break;
		dma_rmb(); /* pairs with wmb() in engine_run() before it sets DD */
		napi_consume_skb(tx_ring->bufs[tx_ring->next_to_clean], budget); /* normal completion, not an error drop */
		tx_ring->bufs[tx_ring->next_to_clean] = NULL;
		dma_wmb(); /* bufs=NULL must be visible before the slot is freed */
		WRITE_ONCE(tx_ring->desc[tx_ring->next_to_clean].status, 0);
		tx_ring->next_to_clean = (tx_ring->next_to_clean + 1) % tx_ring->count;
	}
	if (netif_queue_stopped(priv->dev) &&
	    (tx_ring->next_to_use + 1) % tx_ring->count != tx_ring->next_to_clean) {
		PDEBUG("vnic_tx_poll: waking queue, next_to_use=%u next_to_clean=%u\n",
		       tx_ring->next_to_use, tx_ring->next_to_clean);
		netif_wake_queue(priv->dev);
	}
	spin_unlock(&tx_ring->lock);
}

/*
 * Unconditional sweep of every TX slot, called only at close - mirrors
 * vnic_rx_drain(). Unlike vnic_tx_poll(), this does not check the DD bit:
 * any posted-but-not-yet-completed skb still gets freed here.
 */
static int vnic_tx_drain(struct vnic_priv *priv)
{
	struct vnic_ring *tx_ring = &priv->tx_ring;
	u32 i;
	u32 drained = 0;

	spin_lock_bh(&tx_ring->lock); /* same reason as vnic_rx_drain() */
	for (i = 0; i < tx_ring->count; i++) { /* sweep all slots directly, not via the cursors */
		if (tx_ring->bufs[i]) {
			dev_kfree_skb(tx_ring->bufs[i]);
			tx_ring->bufs[i] = NULL;
			drained++;
		}
		WRITE_ONCE(tx_ring->desc[i].status, 0); /* clear stale DD bit before reopen */
	}
	tx_ring->next_to_use = 0;
	tx_ring->next_to_clean = 0;
	vnic_hal_wr32_raw(priv->hal, VN_REG_TDH, 0); /* resync register cursor with software cursor */
	vnic_hal_wr32_raw(priv->hal, VN_REG_TDT, 0); /* raw: no doorbell needed at close */
	spin_unlock_bh(&tx_ring->lock);
	PDEBUG("vnic_tx_drain: freed %u tx buffers\n", drained);
	return 0;
}

/*
 * RX descriptors are pre-posted: each slot holds a freshly allocated skb
 * before any packet arrives, so the device has somewhere to write into.
 * The ring lock is required here because the engine kthread can still be
 * touching rx_ring even while the interface is administratively down.
 */
static int vnic_rx_buf_alloc(struct vnic_priv *priv, struct vnic_ring *rx_ring, u32 idx)
{
	struct sk_buff *skb = napi_alloc_skb(&priv->napi, priv->rx_buf_size);

	if (!skb)
		return -ENOMEM;
	rx_ring->desc[idx].addr = (u64)(unsigned long)skb->data;
	rx_ring->desc[idx].len = 0;
	rx_ring->desc[idx].status = 0;
	rx_ring->bufs[idx] = skb;

	return 0;
}

static int vnic_rx_post(struct vnic_priv *priv)
{
	struct vnic_ring *rx_ring = &priv->rx_ring;
	int ret = 0;
	u32 count = rx_ring->count;
	u32 i;

	spin_lock_bh(&rx_ring->lock); /* shared with the engine kthread, which interface-down does not stop */
	for (i = 0; i < count - 1; i++) {
		ret = vnic_rx_buf_alloc(priv, rx_ring, i);
		if (ret)
			goto out;
	}
	rx_ring->next_to_use = count - 1;
	rx_ring->next_to_clean = 0;
	PDEBUG("vnic_rx_post: posted %u rx buffers\n", count);
	vnic_hal_wr32(priv->hal, VN_REG_RDT, rx_ring->next_to_use); /* full/empty check left to the engine */

out:
	spin_unlock_bh(&rx_ring->lock);
	return ret;
}

static int vnic_rx_drain(struct vnic_priv *priv)
{
	struct vnic_ring *rx_ring = &priv->rx_ring;
	u32 i;
	u32 drained = 0;

	spin_lock_bh(&rx_ring->lock); /* same reason as vnic_rx_post() */
	for (i = 0; i < rx_ring->count; i++) { /* sweep all slots directly, not via the cursors */
		if (rx_ring->bufs[i]) {
			dev_kfree_skb(rx_ring->bufs[i]);
			rx_ring->bufs[i] = NULL;
			drained++;
		}
		WRITE_ONCE(rx_ring->desc[i].status, 0); /* clear stale DD bit before reopen */
	}
	rx_ring->next_to_use = 0;
	rx_ring->next_to_clean = 0;
	vnic_hal_wr32_raw(priv->hal, VN_REG_RDH, 0); /* resync register cursor with software cursor */
	vnic_hal_wr32_raw(priv->hal, VN_REG_RDT, 0); /* raw: no doorbell needed at close */
	spin_unlock_bh(&rx_ring->lock);
	PDEBUG("vnic_rx_drain: freed %u rx buffers\n", drained);
	return 0;
}

/*
 * Deliver completed RX descriptors up to budget, refilling each slot with a
 * fresh skb before handing it back to the device. RDT is written once at the
 * end, which also wakes the engine if it was stalled on a full ring.
 */
static int vnic_rx_poll(struct vnic_priv *priv, int budget)
{
	struct vnic_ring *rx_ring = &priv->rx_ring;
	struct sk_buff *skb;
	int skb_done = 0;
	u32 ntc, ntu;

	spin_lock(&rx_ring->lock); /* NAPI is softirq context; pairs with spin_lock_bh() in vnic_rx_drain()/engine_run() */
	ntc = rx_ring->next_to_clean;
	ntu = rx_ring->next_to_use;

	while (skb_done < budget) {
		if (!(READ_ONCE(rx_ring->desc[ntc].status) & VN_DESC_DD))
			break;
		dma_rmb(); /* pairs with wmb() in engine_run() before it sets DD */

		skb = rx_ring->bufs[ntc];
		rx_ring->bufs[ntc] = NULL;
		WRITE_ONCE(rx_ring->desc[ntc].status, 0); /* clear DD now, don't rely on the refill loop */

		skb->dev = priv->dev;
		skb->protocol = eth_type_trans(skb, priv->dev);
		napi_gro_receive(&priv->napi, skb); /* deliver unconditionally; a later refill failure isn't this packet's fault */
		ntc = (ntc + 1) % rx_ring->count;

		while ((ntu + 1) % rx_ring->count != ntc) { /* catch up on refills that fell behind or failed */
			if (vnic_rx_buf_alloc(priv, rx_ring, ntu))
				break; /* OOM, already logged; retry next round */
			dma_wmb(); /* addr/bufs must be visible before clearing DD */
			WRITE_ONCE(rx_ring->desc[ntu].status, 0);
			ntu = (ntu + 1) % rx_ring->count;
		}
		skb_done++;
	}
	rx_ring->next_to_use = ntu;
	rx_ring->next_to_clean = ntc;

	if (skb_done) {
		PDEBUG("vnic_rx_poll: delivered %d/%d (budget), next_to_clean=%u, next_to_use=%u\n",
		       skb_done, budget, ntc, ntu);
		vnic_hal_wr32(priv->hal, VN_REG_RDT, ntu); /* everything before ntu has already been refilled */
	}
	spin_unlock(&rx_ring->lock);

	return skb_done;
}

/*
 * The NAPI poll. Non-static: the entry code passes it to netif_napi_add().
 */
int vnic_poll(struct napi_struct *napi, int budget)
{
	struct vnic_priv *priv = container_of(napi, struct vnic_priv, napi);
	int work_done;

	vnic_tx_poll(priv, budget);
	work_done = vnic_rx_poll(priv, budget);
	if (work_done < budget) {
		if (napi_complete_done(napi, work_done))
			vnic_hal_wr32(priv->hal, VN_REG_IMS,
				      VN_INT_TXDONE | VN_INT_RXAVAIL | VN_INT_RXEMPTY);
	}

	PDEBUG("vnic_poll: budget=%d work_done=%d\n", budget, work_done);
	return work_done;
}

/*
 * Transmit a packet (called by the kernel: ndo_start_xmit)
 * push the skb into tx_ring
 */
static int vnic_tx(struct sk_buff *skb, struct net_device *dev)
{
	int ret = 0;
	struct vnic_priv *priv = netdev_priv(dev);
	struct vnic_ring *tx_ring = &priv->tx_ring;
	struct vn_desc *des = tx_ring->desc;
	struct sk_buff **buf = tx_ring->bufs;

	spin_lock_bh(&tx_ring->lock); /* guards against vnic_tx_poll() running in softirq context */
	if ((tx_ring->next_to_use + 1) % tx_ring->count == tx_ring->next_to_clean) { /* one slot reserved for full vs. empty */
		PDEBUG("vnic_tx: ring full, next_to_use=%u next_to_clean=%u, stopping queue\n",
		       tx_ring->next_to_use, tx_ring->next_to_clean);
		netif_stop_queue(dev);
		ret = NETDEV_TX_BUSY;
		goto out;
	}

	/* check whether the skb is fragmented */
	if (skb_linearize(skb)) {
		dev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);   /* once we own this skb we must dispose of it, even on failure */
		ret = NETDEV_TX_OK;       /* the skb has already been consumed (dropped) */
		goto out;
	}

	/* desc->addr points at the skb data; cast through unsigned long
	 * (guaranteed pointer-width) before storing into the u64 desc->addr
	 */
	des[tx_ring->next_to_use].addr = (u64)(unsigned long)skb->data;
	des[tx_ring->next_to_use].len = skb->len;
	des[tx_ring->next_to_use].status = 0; /* DD bit = 0, device-owned */

	buf[tx_ring->next_to_use] = skb;

	PDEBUG("vnic_tx: queued idx=%u len=%u\n", tx_ring->next_to_use, skb->len);
	tx_ring->next_to_use = (tx_ring->next_to_use + 1) % tx_ring->count;
	vnic_hal_wr32(priv->hal, VN_REG_TDT, tx_ring->next_to_use); /* [TDH, next_to_use) is a half-open range */

out:
	spin_unlock_bh(&tx_ring->lock);
	return ret;
}

/*
 * Return statistics to the caller
 */
static void vnic_get_stats64(struct net_device *dev, struct rtnl_link_stats64 *stats)
{
	struct vnic_priv *priv = netdev_priv(dev);

	stats->rx_packets = vnic_hal_rd32(priv->hal, VN_REG_STAT_RXPKTS);
	stats->tx_packets = vnic_hal_rd32(priv->hal, VN_REG_STAT_TXPKTS);
	stats->rx_bytes   = vnic_hal_rd32(priv->hal, VN_REG_STAT_RXBYTES);
	stats->tx_bytes   = vnic_hal_rd32(priv->hal, VN_REG_STAT_TXBYTES);
	/* driver-side drop (e.g. skb_linearize() failure) + device-side drop */
	stats->rx_dropped = vnic_hal_rd32(priv->hal, VN_REG_STAT_RXDROP) + dev->stats.rx_dropped;
	stats->tx_dropped = vnic_hal_rd32(priv->hal, VN_REG_STAT_TXDROP) + dev->stats.tx_dropped;
}

/*
 * The "change_mtu" method is usually not needed.
 * If you need it, it must be like this.
 */
static int vnic_change_mtu(struct net_device *dev, int new_mtu)
{
	struct vnic_priv *priv = netdev_priv(dev);

	if (netif_running(dev))
		return -EBUSY;

	dev->mtu = new_mtu;
	priv->rx_buf_size = dev->mtu + VN_FRAME_OVERHEAD;

	return 0;
}

/* Non-static: the entry code assigns this to netdev->netdev_ops. */
const struct net_device_ops vnic_netdev_ops = {
	.ndo_open		= vnic_open,
	.ndo_stop		= vnic_release,
	.ndo_start_xmit		= vnic_tx,
	.ndo_get_stats64	= vnic_get_stats64,
	.ndo_change_mtu		= vnic_change_mtu,
};
