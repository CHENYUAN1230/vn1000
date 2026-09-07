/* SPDX-License-Identifier: GPL-2.0 */
/* vnic-hal.h - the seam between the netdev core and "the device"
 *
 * The core datapath (vnic-core.c) calls only the functions declared here; a
 * backend implements them. The baseline backend is an in-kernel engine kthread
 * (vnic-hal-engine.c); from Phase 2 on it becomes real PCI / MMIO / DMA / MSI.
 * struct vnic_hal is opaque to the core - only the backend defines it.
 */
#ifndef VNIC_HAL_H
#define VNIC_HAL_H

#include <linux/types.h>		/* u32, dma_addr_t, size_t */
#include <linux/dma-direction.h>	/* enum dma_data_direction */

struct vnic_hal;
struct vnic_priv;
struct vnic_ring;

/* register access - the only path to a device register */
u32  vnic_hal_rd32(struct vnic_hal *hal, u32 off);
void vnic_hal_wr32(struct vnic_hal *hal, u32 off, u32 val);

/* raw write: a plain store into the register file, with none of the ICS/IMS/IMC
 * or TDT/RDT-doorbell side effects of vnic_hal_wr32(). Used only where a ring
 * cursor (TDH/TDT/RDH/RDT) is resynced without waking the engine - the drain
 * paths. A CTRL.TXEN/RXEN gate replaces it in Phase 3.
 */
void vnic_hal_wr32_raw(struct vnic_hal *hal, u32 off, u32 val);

/* descriptor-ring memory */
int  vnic_hal_ring_alloc(struct vnic_hal *hal, struct vnic_ring *ring, u32 count);
void vnic_hal_ring_free(struct vnic_hal *hal, struct vnic_ring *ring);

/* per-packet buffer mapping */
dma_addr_t vnic_hal_map(struct vnic_hal *hal, void *va, size_t len, enum dma_data_direction dir);
void       vnic_hal_unmap(struct vnic_hal *hal, dma_addr_t dma, size_t len, enum dma_data_direction dir);
int        vnic_hal_mapping_error(struct vnic_hal *hal, dma_addr_t dma);

/* lifecycle
 *
 * vnic_hal_create() allocates the backend and starts the engine.
 * vnic_hal_quiesce() blocks until the device is idle and off the rings.
 * vnic_hal_stop() halts engine activity (kthread_stop) but keeps the handle
 *   valid - call it before freeing anything the engine might still touch.
 * vnic_hal_destroy() frees the handle - call it last, after vnic_hal_stop()
 *   and after the rings are freed.
 */
struct vnic_hal *vnic_hal_create(struct vnic_priv *priv);
void vnic_hal_quiesce(struct vnic_hal *hal);
void vnic_hal_stop(struct vnic_hal *hal);
void vnic_hal_destroy(struct vnic_hal *hal);

#endif /* VNIC_HAL_H */
