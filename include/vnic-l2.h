#ifndef VNIC_L2_H
#define VNIC_L2_H
#include "vnic_regs.h"

#undef PDEBUG             /* undef it, just in case */
#ifdef VNIC_DEBUG
#  ifdef __KERNEL__
     /* This one if debugging is on, and kernel space */
#    define PDEBUG(fmt, args...) printk( KERN_DEBUG "vnic: " fmt, ## args)
#  else
     /* This one for user space */
#    define PDEBUG(fmt, args...) fprintf(stderr, fmt, ## args)
#  endif
#else
#  define PDEBUG(fmt, args...) /* not debugging: nothing */
#endif

/* custom ring size */
#define VNIC_TX_RING_SIZE 256
#define VNIC_RX_RING_SIZE 256
extern struct net_device *netdev; // single device

struct vnic_ring {
	struct vn_desc *desc; // array of descriptors; each element is a real struct vn_desc value
	u32 count;
	u32 next_to_use; // tx: value to write into TDT. rx: value to write into RDT
	u32 next_to_clean; // tx: reclaimed by vnic_tx_poll(), trails TDH. rx: delivered by vnic_rx_poll(), trails RDH
	spinlock_t lock;
	struct sk_buff **bufs; // array of skb pointers; each element is a struct sk_buff *
};


/*
 * This structure is private to each device. It is used to pass
 * packets in and out, so there is place for a packet
 */

struct vnic_priv {
	/*
	 * a pointer here would need an extra kzalloc:
	 * struct vnic_ring *tx_ring;
	 * struct vnic_ring *rx_ring;
	 */

	/* vn_desc define in "vnic_regs.h", List of incoming packets */
	struct vnic_ring tx_ring; 
	struct vnic_ring rx_ring;  
	spinlock_t lock;
	struct net_device *dev;
	struct napi_struct napi;
	u32 regs[VN_BAR0_SIZE/4];
	bool doorbell;
	wait_queue_head_t wq;
	struct task_struct *engine; // kthread
	u32 rx_buf_size;
	struct tasklet_struct intr_tasklet;
	struct mutex run_lock; // serializes an engine run vs vnic_release()'s quiesce
};

#endif