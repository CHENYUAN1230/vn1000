/*TODO: batch refill logic - accumulate N completions before refilling the rx ring; have vnic_tx ring the doorbell only after a fixed number of packets */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/moduleparam.h>

#include <linux/sched.h>
#include <linux/kernel.h> /* printk() */
#include <linux/slab.h> /* kmalloc() */
#include <linux/errno.h>  /* error codes */
#include <linux/types.h>  /* size_t */
#include <linux/interrupt.h> /* mark_bh */
#include <linux/mutex.h>

#include <linux/in.h>
#include <linux/netdevice.h>   /* struct device, and other headers */
#include <linux/etherdevice.h> /* eth_type_trans */
#include <linux/ip.h>          /* struct iphdr */
#include <linux/tcp.h>         /* struct tcphdr */
#include <linux/skbuff.h>
#include <linux/version.h> 	/* LINUX_VERSION_CODE  */

#include "vnic-l2.h"

#include <linux/in6.h>
#include <asm/checksum.h>

MODULE_AUTHOR("Yu An Chen");
MODULE_LICENSE("Dual BSD/GPL");

/*
 * Forward declarations, so functions can be grouped by role (ring,
 * open/close, tx, rx, engine) instead of by definition order.
 */
static void vnic_reg_write(struct vnic_priv *priv, u32 off, u32 val);
static inline u32 vnic_reg_read(struct vnic_priv *priv, u32 off);
static void vnic_ring_free(struct vnic_ring *ring);
static int vnic_ring_alloc(struct vnic_ring *ring, u32 count);
static int vnic_open(struct net_device *dev);
static int vnic_release(struct net_device *dev);
static void vnic_tx_poll(struct vnic_priv *priv, int budget);
static int vnic_tx_drain(struct vnic_priv *priv);
static int vnic_rx_buf_alloc(struct vnic_priv *priv, struct vnic_ring *rx_ring, u32 idx);
static int vnic_rx_post(struct vnic_priv *priv);
static int vnic_rx_drain(struct vnic_priv *priv);
static int vnic_rx_poll(struct vnic_priv *priv, int budget);
static int vnic_poll(struct napi_struct *napi, int budget);
static void vnic_engine_run(struct vnic_priv *priv);
static int vnic_engine_thread(void *data);
static void vnic_tx_flip_addr(struct sk_buff *skb);
static int vnic_tx(struct sk_buff *skb, struct net_device *dev);
static void vnic_get_stats64(struct net_device *dev, struct rtnl_link_stats64 *stats);
static int vnic_change_mtu(struct net_device *dev, int new_mtu);
static void vnic_cleanup(void);
static int vnic_init_module(void);
static void vnic_intr_tasklet(struct tasklet_struct *t);


static void vnic_intr_tasklet(struct tasklet_struct *t)
{
	struct vnic_priv *priv = from_tasklet(priv, t, intr_tasklet);
	u32 cause = vnic_reg_read(priv, VN_REG_ICR);
	u32 mask = vnic_reg_read(priv, VN_REG_IMS);
	if (cause & mask & ~VN_INT_ERR) { // ERR alone shouldn't wake NAPI - it can't fix an OOM
		if(napi_schedule_prep(&priv->napi)){
			vnic_reg_write(priv, VN_REG_IMC, VN_INT_TXDONE | VN_INT_RXAVAIL | VN_INT_RXEMPTY); // close the masks
			PDEBUG("vnic_intr_tasklet: masked TX/RX/RXEMPTY, mask now=0x%x\n", vnic_reg_read(priv, VN_REG_IMS));
			__napi_schedule(&priv->napi) ;
		}	
		PDEBUG("vnic_intr_tasklet: cause=0x%x, mask=0x%x\n", cause, mask);
	}
}

/*
 * write and read regster helper function
 */
static void vnic_reg_write(struct vnic_priv *priv, u32 off, u32 val)
{
	unsigned long flags;

	if(off == VN_REG_ICS){
		spin_lock_irqsave(&priv->lock, flags);
		priv->regs[VN_REG_ICR/4] |= val;
		spin_unlock_irqrestore(&priv->lock, flags);
	}
	else if(off == VN_REG_IMS){
		spin_lock_irqsave(&priv->lock, flags);
		priv->regs[VN_REG_IMS/4] |= val;
		spin_unlock_irqrestore(&priv->lock, flags);
	}
	else if(off == VN_REG_IMC){
		spin_lock_irqsave(&priv->lock, flags);
		priv->regs[VN_REG_IMS/4] &= ~val;
		spin_unlock_irqrestore(&priv->lock, flags);
	}
	else{
		priv->regs[off / 4] = val;
		if(off == VN_REG_TDT || off == VN_REG_RDT){
			priv->doorbell = true;
			wake_up_interruptible(&priv->wq);
		}
	}
}

static inline u32 vnic_reg_read(struct vnic_priv *priv, u32 off)
{
	unsigned long flags;
	u32 val;

	if(off == VN_REG_ICR){
		spin_lock_irqsave(&priv->lock, flags);
		val = priv->regs[off / 4];
		priv->regs[off / 4] = 0;
		spin_unlock_irqrestore(&priv->lock, flags);
		return val;
	}

	val = priv->regs[off / 4];
	return val;
}


/*
 * ring allocate and release
 */

static void vnic_ring_free(struct vnic_ring *ring)
{

	/* free ring->desc */
	if(ring->desc){
		kfree(ring->desc);
		ring->desc = NULL;
	}

	/* free ring->bufs */
	if(ring->bufs){
		kfree(ring->bufs);
		ring->bufs = NULL;
	}
	return;
}
static int vnic_ring_alloc(struct vnic_ring *ring, u32 count)
{
	int err = 0;
	/* allocate ring->desc */
	ring->desc = kcalloc(count, sizeof(struct vn_desc), GFP_KERNEL); // use kcalloc for initial to 0
	if(!ring->desc){
		printk(KERN_NOTICE "Ran out of memory allocating ring>desc\n");
		err = -ENOMEM;
		goto rollback;
	}

	/* allocate ring->bufs */
	ring->bufs = kcalloc(count, sizeof(struct sk_buff*), GFP_KERNEL);
	if(!ring->bufs){
		printk(KERN_NOTICE "Ran out of memory allocating ring->bufs\n");
		err = -ENOMEM;
		goto rollback;
	}

	/* initial others */
	ring->count = count;
	ring->next_to_use = 0;
	ring->next_to_clean = 0;
	spin_lock_init(&ring->lock);
	return 0;

rollback:
	vnic_ring_free(ring);
	return err;

}



    
/*
 * Open and close
 */

static int vnic_open(struct net_device *dev)
{	
	int ret = 0;
	struct vnic_priv *priv = netdev_priv(dev);
	priv->rx_buf_size = dev->mtu + VN_FRAME_OVERHEAD; // mtu might change
	ret = vnic_rx_post(priv);
	if(ret){
		vnic_rx_drain(priv);
		goto out;
	}

	PDEBUG("vnic_open: posted %u rx buffers, opening queue\n", priv->rx_ring.next_to_use);
	napi_enable(&priv->napi);
	tasklet_enable(&priv->intr_tasklet);
	vnic_reg_write(priv, VN_REG_IMS, VN_INT_TXDONE | VN_INT_RXAVAIL | VN_INT_RXEMPTY | VN_INT_ERR); // re-arm, vnic_release() masked everything off
	netif_start_queue(dev);

out:
	return ret;
}

static int vnic_release(struct net_device *dev)
{
	struct vnic_priv *priv = netdev_priv(dev);

	netif_tx_disable(dev); /* stop the queue and wait out any in-flight vnic_tx() */
	PDEBUG("vnic_release: closing, draining rx ring\n");
	napi_disable(&priv->napi); // wait out any in-flight vnic_rx_poll() before draining the ring
	tasklet_disable(&priv->intr_tasklet);
	/* At this point nothing can ring TDT/RDT again (TX disabled, NAPI disabled), so
	 * this quiesce only has to wait out a run already in progress - no separate
	 * enable flag needed to block a future one. */
	mutex_lock(&priv->run_lock);
	mutex_unlock(&priv->run_lock);
	vnic_reg_write(priv, VN_REG_IMC, VN_INT_TXDONE | VN_INT_RXAVAIL | VN_INT_RXEMPTY | VN_INT_ERR); // no poll left to re-arm IMS
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

	spin_lock(&tx_ring->lock); // NAPI runs in softirq context; no _bh needed against vnic_tx()'s spin_lock_bh()
	while(tx_ring->next_to_clean != tx_ring->next_to_use){
		if(!(READ_ONCE(tx_ring->desc[tx_ring->next_to_clean].status) & VN_DESC_DD))
			break;
		dma_rmb(); // pairs with wmb() in vnic_engine_run() before it sets DD
		napi_consume_skb(tx_ring->bufs[tx_ring->next_to_clean], budget); // normal completion, not an error drop
		tx_ring->bufs[tx_ring->next_to_clean] = NULL;
		dma_wmb(); // bufs=NULL must be visible before the slot is freed
		WRITE_ONCE(tx_ring->desc[tx_ring->next_to_clean].status, 0);
		tx_ring->next_to_clean = (tx_ring->next_to_clean + 1) % tx_ring->count;
	}
	if (netif_queue_stopped(priv->dev) && (tx_ring->next_to_use + 1) % tx_ring->count != tx_ring->next_to_clean) {
		PDEBUG("vnic_tx_poll: waking queue, next_to_use=%u next_to_clean=%u\n", tx_ring->next_to_use, tx_ring->next_to_clean);
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

	spin_lock_bh(&tx_ring->lock); // same reason as vnic_rx_drain()
	for (i = 0; i < tx_ring->count; i++) { // sweep all slots directly, not via next_to_use/next_to_clean
		if (tx_ring->bufs[i]) {
			dev_kfree_skb(tx_ring->bufs[i]);
			tx_ring->bufs[i] = NULL;
			drained++;
		}
		WRITE_ONCE(tx_ring->desc[i].status, 0); // clear stale DD bit before reopen
	}
	tx_ring->next_to_use = 0;
	tx_ring->next_to_clean = 0;
	priv->regs[VN_REG_TDH / 4] = 0; // resync register cursor with software cursor before reopen
	priv->regs[VN_REG_TDT / 4] = 0; // direct write: no doorbell needed at close
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
	int ret = 0;
	if (!skb){
		if (printk_ratelimit())
				printk(KERN_NOTICE "vnic skb allocate fail\n");
			ret = -ENOMEM;
			goto out;
	}
	rx_ring->desc[idx].addr = (u64)(unsigned long)skb->data;
	rx_ring->desc[idx].len = 0;
	rx_ring->desc[idx].status = 0;
	rx_ring->bufs[idx] = skb;

out:
	return ret;
}

static int vnic_rx_post(struct vnic_priv *priv)
{
	struct vnic_ring *rx_ring = &priv->rx_ring;
	int ret = 0;
	u32 count = rx_ring->count;
	u32 i;

	spin_lock_bh(&rx_ring->lock); // shared with the engine kthread, which is not stopped by interface-down
	for (i = 0; i < count-1; i++) {
		if ((ret = vnic_rx_buf_alloc(priv, rx_ring, i)))
			goto out;
	}
	rx_ring->next_to_use = count-1;
	rx_ring->next_to_clean = 0;
	PDEBUG("vnic_rx_post: posted %u rx buffers\n", count);
	vnic_reg_write(priv, VN_REG_RDT, rx_ring->next_to_use); // full/empty check is left to the engine's comparison

out:
	spin_unlock_bh(&rx_ring->lock);
	return ret;
}

static int vnic_rx_drain(struct vnic_priv *priv)
{
	struct vnic_ring *rx_ring = &priv->rx_ring;
	u32 i;
	u32 drained = 0;

	spin_lock_bh(&rx_ring->lock); // same reason as vnic_rx_post()
	for (i = 0; i < rx_ring->count; i++) { // sweep all slots directly, not via next_to_use/next_to_clean
		if (rx_ring->bufs[i]) {
			dev_kfree_skb(rx_ring->bufs[i]);
			rx_ring->bufs[i] = NULL;
			drained++;
		}
		WRITE_ONCE(rx_ring->desc[i].status, 0); // clear stale DD bit before reopen
	}
	rx_ring->next_to_use = 0;
	rx_ring->next_to_clean = 0;
	priv->regs[VN_REG_RDH / 4] = 0; // resync register cursor with software cursor before reopen
	priv->regs[VN_REG_RDT / 4] = 0; // direct write: no doorbell needed at close
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

	spin_lock(&rx_ring->lock); // NAPI already runs in softirq context; pairs with the spin_lock_bh() in vnic_rx_drain()/vnic_engine_run()
	ntc = rx_ring->next_to_clean;
	ntu = rx_ring->next_to_use;

	while(skb_done < budget){
		if(!(READ_ONCE(rx_ring->desc[ntc].status) & VN_DESC_DD))
			break;
		dma_rmb(); // pairs with wmb() in vnic_engine_run() before it sets DD

		skb = rx_ring->bufs[ntc];
		rx_ring->bufs[ntc] = NULL;
		WRITE_ONCE(rx_ring->desc[ntc].status, 0); // clear DD now, don't rely on the refill loop below catching up

		skb->dev = priv->dev;
		skb->protocol = eth_type_trans(skb, priv->dev);
		napi_gro_receive(&priv->napi, skb); // deliver unconditionally; a refill failure afterward isn't this packet's fault
		ntc = (ntc + 1) % rx_ring->count;

		while((ntu + 1) % rx_ring->count != ntc){ // catch up on refills that fell behind or previously failed
			if (vnic_rx_buf_alloc(priv, rx_ring, ntu))
				break; // OOM, already logged; retry next round
			dma_wmb(); // addr/bufs must be visible before clearing DD
			WRITE_ONCE(rx_ring->desc[ntu].status, 0);
			ntu = (ntu + 1) % rx_ring->count;
		}
		skb_done++;
	}
	rx_ring->next_to_use = ntu;
	rx_ring->next_to_clean = ntc;

	if(skb_done){
		PDEBUG("vnic_rx_poll: delivered %d/%d (budget), next_to_clean=%u, next_to_use=%u\n", skb_done, budget, ntc, ntu);
		vnic_reg_write(priv, VN_REG_RDT, ntu); // everything before ntu has already been refilled
	}
	spin_unlock(&rx_ring->lock);

	return skb_done;

}

/*
 * The poll implementation.
 */
static int vnic_poll(struct napi_struct *napi, int budget)
{
	struct vnic_priv *priv = container_of(napi, struct vnic_priv, napi);
	int work_done;

	vnic_tx_poll(priv, budget);
	work_done = vnic_rx_poll(priv, budget);
	if(work_done < budget){
		if(napi_complete_done(napi, work_done))
			vnic_reg_write(priv, VN_REG_IMS, VN_INT_TXDONE | VN_INT_RXAVAIL | VN_INT_RXEMPTY);

	}
		
	PDEBUG("vnic_poll: budget=%d work_done=%d\n", budget, work_done);
	return work_done;
}
	    



/*
 * pretend to be driver
 * Engine kthread (triggered by kthread_run() in vnic_init_module())
 */


/*
 * Copies each TX frame into the RX ring (loopback) before marking TX done.
 * If the target RX slot is still full, leaves that TX descriptor unprocessed
 * and stops - this is deliberate backpressure via netif_stop_queue(), not a
 * drop.
 */


static void vnic_engine_run(struct vnic_priv *priv)
{
	// tx ring
	struct vnic_ring *tx_ring = &priv->tx_ring;
	u32 th = vnic_reg_read(priv, VN_REG_TDH);
	u32 tt = vnic_reg_read(priv, VN_REG_TDT);
	u32 txp = vnic_reg_read(priv, VN_REG_STAT_TXPKTS);
	u32 txb = vnic_reg_read(priv, VN_REG_STAT_TXBYTES);

	// rx ring
	struct vnic_ring *rx_ring = &priv->rx_ring;
	u32 rh, rt; // read under the lock below
	u32 rxp = vnic_reg_read(priv, VN_REG_STAT_RXPKTS);
	u32 rxb = vnic_reg_read(priv, VN_REG_STAT_RXBYTES);
	u32 rxdrop = vnic_reg_read(priv, VN_REG_STAT_RXDROP);
	u32 th_start = th;
	u32 rh_start;

	/* No tx_ring->lock here: that lock only serialises vnic_tx() against
	 * vnic_tx_poll() (driver-side producer/consumer), not the engine, which
	 * plays "the device" and is only ever excluded from tx_ring by the
	 * run_lock quiesce in vnic_release() - see the driving conversation.
	 * Individual descriptor handoff is lock-free: DD bit + dma_wmb()/dma_rmb(). */
	spin_lock_bh(&rx_ring->lock); // mutually excludes this whole section from vnic_rx_drain()
	rh = vnic_reg_read(priv, VN_REG_RDH);
	rt = vnic_reg_read(priv, VN_REG_RDT);
	rh_start = rh;
	while(th != tt && rh != rt){ // one spot reserved to distinguish full vs. empty, same as TX
		if(READ_ONCE(rx_ring->desc[rh].status) & VN_DESC_DD){
			PDEBUG("vnic_engine_run: rx ring full at rh=%u, stalling th=%u\n", rh, th);
			vnic_reg_write(priv, VN_REG_ICS, VN_INT_RXEMPTY);
			break;
		}
		dma_rmb(); // pairs with wmb() in vnic_rx_poll() before it clears DD

		// rx ring - frames too short to even hold an Ethernet header can't be
		// looped back at all; skip rh entirely rather than touching it and
		// deciding afterward (that used to leave skb_put() residue behind on
		// a slot that never advanced - see the driving conversation)
		if (tx_ring->bufs[th]->len < sizeof(struct ethhdr)) {
			PDEBUG("vnic_engine_run: loopback dropped frame at th=%u (len=%u too short)\n", th, tx_ring->bufs[th]->len);
			rxdrop++;
		}
		else{
			memcpy(rx_ring->bufs[rh]->data, tx_ring->bufs[th]->data, tx_ring->bufs[th]->len);
			skb_put(rx_ring->bufs[rh], tx_ring->bufs[th]->len); // mark the copied bytes valid
			vnic_tx_flip_addr(rx_ring->bufs[rh]);
			rx_ring->desc[rh].len = tx_ring->desc[th].len;
			rxp ++;
			rxb += rx_ring->desc[rh].len;
			dma_wmb(); // data + len must be visible before DD says "ready"
			WRITE_ONCE(rx_ring->desc[rh].status, rx_ring->desc[rh].status | VN_DESC_DD);
			rh = (rh + 1) % rx_ring->count;
		}
		// tx ring
		dma_wmb(); // pairs with rmb() in vnic_tx_poll()
		WRITE_ONCE(tx_ring->desc[th].status, tx_ring->desc[th].status | VN_DESC_DD);

		txp ++;
		txb += tx_ring->desc[th].len;
		th = (th + 1) % tx_ring->count;
	}

	vnic_reg_write(priv, VN_REG_RDH, rh);
	vnic_reg_write(priv, VN_REG_STAT_RXPKTS, rxp);
	vnic_reg_write(priv, VN_REG_STAT_RXBYTES, rxb);
	vnic_reg_write(priv, VN_REG_STAT_RXDROP, rxdrop);
	spin_unlock_bh(&rx_ring->lock);
	if (th != th_start){
		PDEBUG("vnic_engine_run: processed th %u->%u, txp=%u rxp=%u rxdrop=%u\n", th_start, th, txp, rxp, rxdrop);
		vnic_reg_write(priv, VN_REG_ICS, VN_INT_TXDONE);
	}
	if (rh != rh_start)
		vnic_reg_write(priv, VN_REG_ICS, VN_INT_RXAVAIL);

	// tx ring (no lock needed - see the note above; run_lock is the only thing
	// that has to exclude vnic_tx_drain())
	vnic_reg_write(priv, VN_REG_TDH, th);
	vnic_reg_write(priv, VN_REG_STAT_TXPKTS, txp);
	vnic_reg_write(priv, VN_REG_STAT_TXBYTES, txb);
	tasklet_schedule(&priv->intr_tasklet); // switch napi_schedule by using tasklet_schedule
}

static int vnic_engine_thread(void *data)
{
	struct vnic_priv *priv = data;
	while(!kthread_should_stop()){ // set true by vnic_cleanup()'s kthread_stop()
		wait_event_interruptible(priv->wq, priv->doorbell || kthread_should_stop());
		priv->doorbell = false;
		if(kthread_should_stop())
			break;
		mutex_lock(&priv->run_lock); // lets vnic_release()'s quiesce wait out an in-progress run
		vnic_engine_run(priv);  // do real thing
		mutex_unlock(&priv->run_lock);
	}
	return 0;
}

/*
 * Loopback header rewrite: swap the Ethernet src/dst (so eth_type_trans()
 * later classifies the frame as PACKET_HOST) and, for IPv4, additionally
 * swap src/dst IP addresses. Non-IPv4 frames (ARP, IPv6 ND, ...) still get
 * the MAC swap and are kept as-is - only the IP-specific step is
 * conditional, so there is no drop path here at all. Checksums stay valid
 * since swapping only reorders fields already inside the checksummed
 * region. Caller guarantees skb->len >= sizeof(struct ethhdr) before this
 * is called - see the size check in vnic_engine_run().
 */
static void vnic_tx_flip_addr(struct sk_buff *skb)
{
	struct ethhdr *eth = (struct ethhdr *)skb->data;
	u8 eth_tmp[ETH_ALEN]; // temp mac address

	// swap h_source, h_dest
	memcpy(eth_tmp, eth->h_source, ETH_ALEN);
	memcpy(eth->h_source, eth->h_dest , ETH_ALEN);
	memcpy(eth->h_dest, eth_tmp , ETH_ALEN);

	if (eth->h_proto == htons(ETH_P_IP) && skb->len >= sizeof(struct ethhdr) + sizeof(struct iphdr)) {
		struct iphdr *iph = (struct iphdr *)(skb->data + sizeof(struct ethhdr));
		u32 ip_tmp = iph->saddr;
		iph->saddr = iph->daddr;
		iph->daddr = ip_tmp;
		iph->check = 0;         /* and rebuild the checksum (ip needs it) */
		iph->check = ip_fast_csum((unsigned char *)iph,iph->ihl);
	}
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
	

	
	spin_lock_bh(&tx_ring->lock); // guards against vnic_tx_poll() running in softirq context
	if((tx_ring->next_to_use + 1) % tx_ring->count == tx_ring->next_to_clean){ // one spot reserved for full vs. empty
		PDEBUG("vnic_tx: ring full, next_to_use=%u next_to_clean=%u, stopping queue\n", tx_ring->next_to_use, tx_ring->next_to_clean);
		netif_stop_queue(dev);
		ret = NETDEV_TX_BUSY;
		goto out;
	}

	/*
	 * check whether the skb is fragmented
	 */
	if (skb_linearize(skb)) {
		dev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);   // once we own this skb we must dispose of it, even on failure
		ret = NETDEV_TX_OK;   // the skb has already been consumed (dropped)
		goto out;
	}

	/* tx_ring->desc point to skb data */
	/* cast through unsigned long (guaranteed pointer-width) before narrowing/widening to the u64 stored in desc->addr */
	des[tx_ring->next_to_use].addr = (u64)(unsigned long)skb->data;
	des[tx_ring->next_to_use].len = skb->len;
	des[tx_ring->next_to_use].status = 0; // DD bit = 0, device-owned

	/* tx_ring->bufs point to skb */
	buf[tx_ring->next_to_use] = skb;

	PDEBUG("vnic_tx: queued idx=%u len=%u\n", tx_ring->next_to_use, skb->len);
	tx_ring->next_to_use = (tx_ring->next_to_use + 1) % tx_ring->count;
	vnic_reg_write(priv, VN_REG_TDT, tx_ring->next_to_use); // [TDH, next_to_use) is a half-open range

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
	stats->rx_packets = vnic_reg_read(priv, VN_REG_STAT_RXPKTS);
	stats->tx_packets = vnic_reg_read(priv, VN_REG_STAT_TXPKTS);
	stats->rx_bytes   = vnic_reg_read(priv, VN_REG_STAT_RXBYTES);
	stats->tx_bytes   = vnic_reg_read(priv, VN_REG_STAT_TXBYTES);
	// driver-side drop (e.g. skb_linearize() failure) + device-side drop
	stats->rx_dropped   = vnic_reg_read(priv, VN_REG_STAT_RXDROP) + dev->stats.rx_dropped;
	stats->tx_dropped   = vnic_reg_read(priv, VN_REG_STAT_TXDROP) + dev->stats.tx_dropped;
}






/*
 * The "change_mtu" method is usually not needed.
 * If you need it, it must be like this.
 */
static int vnic_change_mtu(struct net_device *dev, int new_mtu)
{
	struct vnic_priv *priv = netdev_priv(dev);
	if(netif_running(dev))
		return -EBUSY;

	dev->mtu = new_mtu;
	priv->rx_buf_size = dev->mtu + VN_FRAME_OVERHEAD;

	return 0; /* success */
}


static const struct net_device_ops vnic_netdev_ops = {
	.ndo_open            = vnic_open,
	.ndo_stop            = vnic_release,
	.ndo_start_xmit      = vnic_tx,
	.ndo_get_stats64     = vnic_get_stats64,
	.ndo_change_mtu      = vnic_change_mtu,
};



/*
 * The devices
 */

struct net_device *netdev; // only one device



/*
 * Finally, the module stuff
 */

static void vnic_cleanup(void)
{
	if (netdev) {
		struct vnic_priv *priv = netdev_priv(netdev);
		PDEBUG("vnic_cleanup: unloading\n");
		unregister_netdev(netdev);
		if(priv->engine)
			kthread_stop(priv->engine);
		
		tasklet_kill(&priv->intr_tasklet);
		vnic_ring_free(&priv->rx_ring);
		vnic_ring_free(&priv->tx_ring);
		free_netdev(netdev); //will call netif_napi_del()
		netdev = NULL;
	}
	return;
}




static int vnic_init_module(void)
{
	int ret = 0;
	struct vnic_priv *priv;

	netdev = alloc_etherdev(sizeof(struct vnic_priv));
	if(!netdev){
		ret = -ENOMEM;
		return ret;
	}

	strscpy(netdev->name, "vnicl2_loop%d", sizeof(netdev->name));
	netdev->netdev_ops = &vnic_netdev_ops;
	eth_hw_addr_random(netdev);     // let the kernel assign a random MAC
	netdev->min_mtu = VN_MTU_MIN;
	netdev->max_mtu = VN_MTU_MAX;

	priv = netdev_priv(netdev);
	priv->dev = netdev;
	netif_napi_add(netdev, &priv->napi, vnic_poll);

	/* initial tasklet */
	tasklet_setup(&priv->intr_tasklet, vnic_intr_tasklet); // setup already enable the tasklet
	tasklet_disable(&priv->intr_tasklet); // disable first, enabled during open()

	spin_lock_init(&priv->lock);
	mutex_init(&priv->run_lock);
	/* fill regs[] */
	vnic_reg_write(priv, VN_REG_ID, VN_ID_MAGIC);
	vnic_reg_write(priv, VN_REG_VERSION, VN_VERSION_1_0);
	vnic_reg_write(priv, VN_REG_CAP, VN_CAP_MSI | VN_CAP_MODER);
	vnic_reg_write(priv, VN_REG_IMS, VN_INT_TXDONE | VN_INT_RXAVAIL | VN_INT_RXEMPTY | VN_INT_ERR); // open all the masks

	/* initial tx kthread */
	init_waitqueue_head(&priv->wq);   // wq must be initialized before the engine thread can use it
	priv->engine = kthread_run(vnic_engine_thread, priv, "vnic-l2-engine");
	if (IS_ERR(priv->engine)) {
		ret = PTR_ERR(priv->engine);
		priv->engine = NULL;
		goto free_netdev;   // falls into the existing error-label chain
	}

	/* allocate tx ring and rx ring */
	ret = vnic_ring_alloc(&priv->tx_ring, VNIC_TX_RING_SIZE);
	if(ret){
		ret = -ENOMEM;
		goto free_engine; // netdev has been allocated
	}

	ret = vnic_ring_alloc(&priv->rx_ring, VNIC_RX_RING_SIZE);
	if(ret){
		ret = -ENOMEM;
		goto free_tx_ring;
	}

	/* write ring register */

	// tx
	u64 tx_base = (u64)(unsigned long)priv->tx_ring.desc;
	vnic_reg_write(priv, VN_REG_TDBAL, lower_32_bits(tx_base));
	vnic_reg_write(priv, VN_REG_TDBAH, upper_32_bits(tx_base));
	vnic_reg_write(priv, VN_REG_TDLEN, priv->tx_ring.count * sizeof(struct vn_desc));
	vnic_reg_write(priv, VN_REG_TDH, priv->tx_ring.next_to_clean);
	priv->regs[VN_REG_TDT / 4] = priv->tx_ring.next_to_use; // write directly (not via vnic_reg_write) so this doesn't ring the doorbell

	// rx
	u64 rx_base = (u64)(unsigned long)priv->rx_ring.desc;
	vnic_reg_write(priv, VN_REG_RDBAL, lower_32_bits(rx_base));
	vnic_reg_write(priv, VN_REG_RDBAH, upper_32_bits(rx_base));
	vnic_reg_write(priv, VN_REG_RDLEN, priv->rx_ring.count * sizeof(struct vn_desc));
	vnic_reg_write(priv, VN_REG_RDH, priv->rx_ring.next_to_clean);
	priv->regs[VN_REG_RDT / 4] = priv->rx_ring.next_to_use; // write directly (not via vnic_reg_write) so this doesn't ring the doorbell
	


	/* regisiter device, should be the last movement */
	if ((ret = register_netdev(netdev))) {
		printk("vnic: error %i registering device \"%s\"\n",
				ret, netdev->name);
		/* not registered yet, skip unregister_netdev() */
		ret = -ENODEV;
		goto free_rx_ring;
	}
	netdev_info(netdev, "VN1000 Level 2 (baseline: in-kernel engine) loaded (ID=0x%08x)\n", vnic_reg_read(priv, VN_REG_ID));
	return ret;
	

free_rx_ring:
	vnic_ring_free(&priv->rx_ring);
free_tx_ring:
	vnic_ring_free(&priv->tx_ring);
free_engine:
	kthread_stop(priv->engine);
free_netdev:
	free_netdev(netdev);
	netdev = NULL;
	return ret;
}


module_init(vnic_init_module);
module_exit(vnic_cleanup);
