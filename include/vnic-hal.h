/* SPDX-License-Identifier: GPL-2.0 */
/* vnic-hal.h - the seam between the netdev core and "the device"
 *
 * vnic-core.c calls only the functions declared here; the build-specific half
 * implements them: vnic-engine.c (in-kernel kthread) or vnic-pci.c (real PCI /
 * MMIO / DMA / MSI). struct vnic_hal is opaque to the core: each backend
 * defines its own, file-local, and builds it inside its own module entry code.
 */
#ifndef VNIC_HAL_H
#define VNIC_HAL_H

#include <linux/types.h>		/* u32, dma_addr_t, size_t */
#include <linux/dma-direction.h>	/* enum dma_data_direction */

struct vnic_hal;
struct vnic_priv;
struct vnic_ring;

/* register access: the only path to a device register */
u32  vnic_hal_rd32(struct vnic_hal *hal, u32 off);
void vnic_hal_wr32(struct vnic_hal *hal, u32 off, u32 val);

/* raw write: a plain store into the register file, with none of the ICS/IMS/IMC
 * or TDT/RDT-doorbell side effects of vnic_hal_wr32(). Used only where a ring
 * cursor (TDH/TDT/RDH/RDT) is resynced without waking the engine (the drain
 * paths). A CTRL.TXEN/RXEN gate replaces it in Phase 3.
 */
void vnic_hal_wr32_raw(struct vnic_hal *hal, u32 off, u32 val);

/* descriptor-ring memory */
int  vnic_hal_ring_alloc(struct vnic_hal *hal, struct vnic_ring *ring, u32 count);
void vnic_hal_ring_free(struct vnic_hal *hal, struct vnic_ring *ring);

/* per-packet buffer mapping (Phase 3) */
dma_addr_t vnic_hal_map(struct vnic_hal *hal, void *va, size_t len, enum dma_data_direction dir);
void       vnic_hal_unmap(struct vnic_hal *hal, dma_addr_t dma, size_t len, enum dma_data_direction dir);
int        vnic_hal_mapping_error(struct vnic_hal *hal, dma_addr_t dma);

/* completion interrupt, set up / torn down per ndo_open / ndo_stop:
 *   vnic_hal_irq_request()  engine: tasklet_setup()   pci: request_irq()
 *   vnic_hal_irq_free()     engine: tasklet_kill()     pci: free_irq()
 */
int  vnic_hal_irq_request(struct vnic_hal *hal);
void vnic_hal_irq_free(struct vnic_hal *hal);

/* quiesce: block until the device is idle and will not touch the rings.
 * Called from vnic_release() before the rings are drained.
 *   engine: waits out an in-flight engine_run()
 *   pci:    synchronize_irq() once TX/RX are disabled
 *
 * Building and tearing down struct vnic_hal is NOT in this interface: the core
 * never does it. Each backend builds its own hal in its module entry code
 * (vnic-engine.c: module_init; vnic-pci.c: pci probe).
 */
void vnic_hal_quiesce(struct vnic_hal *hal);

#endif /* VNIC_HAL_H */
