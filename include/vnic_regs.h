/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vnic_regs.h - vNIC VN1000 register / descriptor specification
 *
 * Authoritative hardware spec shared by BOTH the Linux driver and the
 * QEMU device model. Derived from docs/vn1000-datasheet.md - keep in sync.
 *
 * VN1000: single-queue, single-function virtual 1GbE Ethernet controller.
 * All registers are 32-bit, little-endian, in the BAR0 MMIO space.
 */
#ifndef VNIC_REGS_H
#define VNIC_REGS_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t  u8;
#endif

/* ---- PCI identity (datasheet section 2) ---- */
#define VN_PCI_VENDOR_ID    0x1234u
#define VN_PCI_DEVICE_ID    0x11E9u

/* ---- ID / VERSION values (datasheet section 4.1 / 4.2) ---- */
#define VN_ID_MAGIC         0x564E3130u   /* "VN10" */
#define VN_VERSION_1_0      0x00010000u   /* major.minor */

/* ---- BAR0 register offsets (datasheet section 3) ---- */
#define VN_REG_ID           0x0000  /* RO   identity magic */
#define VN_REG_VERSION      0x0004  /* RO   version major.minor */
#define VN_REG_CAP          0x0008  /* RO   capability bitmap */
#define VN_REG_CTRL         0x0010  /* RW   device control */
#define VN_REG_STATUS       0x0014  /* RO   device status */
#define VN_REG_MAC_LO       0x0018  /* RW   MAC bytes 0..3 */
#define VN_REG_MAC_HI       0x001C  /* RW   MAC bytes 4..5 + valid */
#define VN_REG_MAXFRM       0x0020  /* RW   max frame size (bytes) */
#define VN_REG_TDBAL        0x0100  /* RW   TX ring base addr low */
#define VN_REG_TDBAH        0x0104  /* RW   TX ring base addr high */
#define VN_REG_TDLEN        0x0108  /* RW   TX ring length bytes = N * 16 */
#define VN_REG_TDH          0x010C  /* RO   TX head (device consumer index) */
#define VN_REG_TDT          0x0110  /* RW   TX tail (driver producer, doorbell) */
#define VN_REG_RDBAL        0x0200  /* RW   RX ring base addr low */
#define VN_REG_RDBAH        0x0204  /* RW   RX ring base addr high */
#define VN_REG_RDLEN        0x0208  /* RW   RX ring length bytes = N * 16 */
#define VN_REG_RDH          0x020C  /* RO   RX head (device producer index) */
#define VN_REG_RDT          0x0210  /* RW   RX tail (driver refill, doorbell) */
#define VN_REG_ICR          0x0300  /* RC   interrupt cause (read-to-clear) */
#define VN_REG_ICS          0x0304  /* WO   interrupt cause set */
#define VN_REG_IMS          0x0308  /* RW1S interrupt mask set (enable) */
#define VN_REG_IMC          0x030C  /* RW1C interrupt mask clear (disable) */
#define VN_REG_ITR          0x0310  /* RW   interrupt moderation (microseconds) */
#define VN_REG_STAT_TXPKTS  0x0400  /* RO   packets transmitted */
#define VN_REG_STAT_TXBYTES 0x0404  /* RO   bytes transmitted */
#define VN_REG_STAT_RXPKTS  0x0408  /* RO   packets received */
#define VN_REG_STAT_RXBYTES 0x040C  /* RO   bytes received */
#define VN_REG_STAT_TXDROP  0x0410  /* RO   TX drops */
#define VN_REG_STAT_RXDROP  0x0414  /* RO   RX drops */

#define VN_BAR0_SIZE        0x1000u /* 4 KiB(16^3 bytes) MMIO window 16^3 */

/* ---- CAP bits (datasheet section 4.3) ---- */
#define VN_CAP_MSI          (1u << 0)
#define VN_CAP_MODER        (1u << 1)
#define VN_CAP_POLL         (1u << 2)  /* reserved (advanced A2) */
#define VN_CAP_CSUM         (1u << 3)  /* reserved */

/* ---- CTRL bits (datasheet section 4.4) ---- */
#define VN_CTRL_EN          (1u << 0)
#define VN_CTRL_RST         (1u << 1)  /* software reset, self-clearing */
#define VN_CTRL_TXEN        (1u << 2)
#define VN_CTRL_RXEN        (1u << 3)
#define VN_CTRL_PROMISC     (1u << 4)

/* ---- STATUS bits (datasheet section 4.5) ---- */
#define VN_STATUS_RUNNING   (1u << 0)
#define VN_STATUS_LINK_UP   (1u << 1)

/* ---- MAC_HI bits (datasheet section 4.6) ---- */
#define VN_MAC_HI_ADDR_MASK 0x0000FFFFu  /* MAC bytes 4..5 */
#define VN_MAC_HI_AV        (1u << 31)   /* address valid */

/* ---- interrupt cause / mask bits, shared by ICR/ICS/IMS/IMC (section 4.8) ---- */
#define VN_INT_TXDONE       (1u << 0)
#define VN_INT_RXAVAIL      (1u << 1)
#define VN_INT_RXEMPTY      (1u << 2)
#define VN_INT_ERR          (1u << 3)

/* ---- frame / MTU (datasheet section 4.7) ---- */
#define VN_FRAME_OVERHEAD   18      /* 14B Ethernet header + 4B FCS */
#define VN_MAXFRM_DEFAULT   1518    /* MTU 1500 + overhead */
#define VN_MTU_MIN          68
#define VN_MTU_MAX          9000

/* ---- descriptor ring constraints (datasheet section 4.7) ---- */
#define VN_DESC_SIZE        16      /* sizeof(struct vn_desc) */
#define VN_RING_MIN         8       /* min descriptor count (power of two) */
#define VN_RING_MAX         4096    /* max descriptor count */
#define VN_RING_ALIGN       256     /* ring base byte alignment */

/**
 * struct vn_desc - VN1000 TX/RX descriptor (datasheet section 5)
 * @addr:   buffer address - DMA address (Level 2) or kernel token (Level 1)
 * @len:    TX: packet length; RX (on completion): bytes received
 * @flags:  TX: command bits (%VN_TXF_EOP, %VN_TXF_RS); RX: result (%VN_RXF_*)
 * @status: DD ownership bit (%VN_DESC_DD); driver clears -> 0, device sets -> 1
 *
 * TX and RX share this 16-byte layout; field meaning differs by direction.
 * The device reads/writes it by DMA, so the layout is ABI (see the
 * compile-time size check below).
 */
struct vn_desc {
	u64 addr;
	u32 len;
	u16 flags;
	u16 status;
} __attribute__((packed));

/* compile-time guarantee that the on-the-wire layout is exactly 16 bytes */
typedef char vn_desc_size_check[(sizeof(struct vn_desc) == VN_DESC_SIZE) ? 1 : -1];

/* ---- TX descriptor flags, driver-written (datasheet section 5.1) ---- */
#define VN_TXF_EOP          (1u << 0)  /* end of packet */
#define VN_TXF_RS           (1u << 1)  /* report status */
#define VN_TXF_IC           (1u << 2)  /* insert checksum (reserved) */

/* ---- RX descriptor flags, device-written (datasheet section 5.2) ---- */
#define VN_RXF_EOP          (1u << 0)
#define VN_RXF_ERR          (1u << 1)

/* ---- descriptor status, written by both sides (datasheet section 5.3) ---- */
#define VN_DESC_DD          (1u << 0)  /* descriptor done: 0 = device-owned, 1 = driver-owned */

#endif /* VNIC_REGS_H */
