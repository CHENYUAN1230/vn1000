/* SPDX-License-Identifier: GPL-2.0 */
#ifndef VNIC_H
#define VNIC_H
#include "vnic_regs.h"
#include "vnic-hal.h"

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

extern struct net_device *netdev;	/* the single device */

/**
 * struct vnic_ring - one TX or RX descriptor ring
 * @desc: array of @count descriptors (the device reads/writes these)
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
	u32 count;
	u32 next_to_use;
	u32 next_to_clean;
	spinlock_t lock;
	struct sk_buff **bufs;
};

/**
 * struct vnic_priv - per-device private state (netdev_priv area)
 * @hal: backend handle; the only way the core reaches "the device"
 * @tx_ring: transmit descriptor ring
 * @rx_ring: receive descriptor ring
 * @dev: back-pointer to the owning net_device
 * @napi: NAPI context for the poll loop
 * @rx_buf_size: RX skb size, tracks the MTU
 * @intr_tasklet: bottom half for the simulated interrupt
 */
struct vnic_priv {
	struct vnic_hal *hal;
	struct vnic_ring tx_ring;
	struct vnic_ring rx_ring;
	struct net_device *dev;
	struct napi_struct napi;
	u32 rx_buf_size;
	struct tasklet_struct intr_tasklet;
};

#endif /* VNIC_H */
