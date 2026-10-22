/* SPDX-License-Identifier: GPL-2.0 */
/* vnic.h - driver-private types and the core <-> entry contract
 *
 * Shared by vnic-core.c and the build-specific halves (vnic-engine.c /
 * vnic-pci.c). The device register spec is include/vnic_regs.h; the backend
 * seam is vnic-hal.h.
 */
#ifndef VNIC_H
#define VNIC_H

#include <linux/netdevice.h>	/* struct net_device, napi_struct, net_device_ops */
#include <linux/skbuff.h>	/* struct sk_buff */
#include <linux/spinlock.h>	/* spinlock_t */
#include <linux/types.h>	/* dma_addr_t */
#include <linux/workqueue.h>	/* struct delayed_work */

#include "vnic_regs.h"		/* struct vn_desc, VN_* register defs */

struct vnic_hal;		/* opaque: defined per backend in vnic-engine.c / vnic-pci.c */

#undef PDEBUG			/* undef it, just in case */
#ifdef VNIC_DEBUG
#  ifdef __KERNEL__
	/* debugging on, kernel space */
#    define PDEBUG(fmt, args...) printk(KERN_DEBUG "vnic: " fmt, ## args)
#  else
	/* debugging on, user space */
#    define PDEBUG(fmt, args...) fprintf(stderr, fmt, ## args)
#  endif
#else
#  define PDEBUG(fmt, args...)	/* not debugging: nothing */
#endif

#define VNIC_TX_RING_SIZE 256
#define VNIC_RX_RING_SIZE 256

/**
 * struct vnic_ring - one TX or RX descriptor ring
 * @desc: array of @count descriptors (the device reads/writes these)
 * @desc_dma: DMA (bus) address of @desc, as returned by dma_alloc_coherent();
 *            this is what gets written into TDBAL/RDBAL etc, since the device
 *            cannot use a CPU virtual address
 * @count: number of descriptors
 * @next_to_use: TX: value to write into TDT; RX: value to write into RDT
 * @next_to_clean: TX: reclaimed by vnic_tx_poll(), trails TDH;
 *                 RX: delivered by vnic_rx_poll(), trails RDH
 * @lock: serialises the driver's producer/consumer against each other and
 *        against vnic_{tx,rx}_drain()
 * @bufs: array of @count skb pointers, parallel to @desc
 */
struct vnic_ring {
	struct vn_desc *desc;
	dma_addr_t desc_dma; // point to the address as same as ring->desc
	u32 count;
	u32 next_to_use;
	u32 next_to_clean;
	spinlock_t lock;
	struct sk_buff **bufs;
};

/* the device's 32-bit STAT_* registers, in the order of vnic_stat_regs[] */
enum vnic_stat {
	VNIC_STAT_TXPKTS,
	VNIC_STAT_TXBYTES,
	VNIC_STAT_RXPKTS,
	VNIC_STAT_RXBYTES,
	VNIC_STAT_TXDROP,
	VNIC_STAT_RXDROP,
	VNIC_NSTATS,
};

/**
 * struct vnic_stats - device counters accumulated into 64-bit totals
 * @lock: serialises vnic_stats_update() between its callers
 * @total: running totals since the netdev was registered
 * @last: register values at the previous read, for the u32 delta
 * @work: periodic read, so a 32-bit register can't wrap twice unseen
 */
struct vnic_stats {
	spinlock_t lock;
	u64 total[VNIC_NSTATS];
	u32 last[VNIC_NSTATS];
	struct delayed_work work;
};

/**
 * struct vnic_priv - per-netdev private state (netdev_priv area)
 * @hal: backend handle; the only way the core reaches "the device"
 * @tx_ring: transmit descriptor ring
 * @rx_ring: receive descriptor ring
 * @dev: back-pointer to the owning net_device
 * @napi: NAPI context for the poll loop
 * @rx_buf_size: RX skb size, tracks the MTU
 * @stats: device counters, kept across down/up and device resets
 */
struct vnic_priv {
	struct vnic_hal *hal;
	struct vnic_ring tx_ring;
	struct vnic_ring rx_ring;
	struct net_device *dev;
	struct napi_struct napi;
	u32 rx_buf_size;
	struct vnic_stats stats;
};

/* implemented in vnic-core.c, used by the entry code in vnic-engine.c / vnic-pci.c */
extern const struct net_device_ops vnic_netdev_ops;
int vnic_poll(struct napi_struct *napi, int budget);

#endif /* VNIC_H */
