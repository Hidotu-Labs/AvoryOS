#ifndef DRIVERS_NET_E1000_REGS_H
#define DRIVERS_NET_E1000_REGS_H

/* Register map shared by the supported 82540EM and 82574L PCI models. */
#define E1000_VENDOR 0x8086
#define E1000_82540EM 0x100e
#define E1000E_82574L 0x10d3
#define E1000_MMIO_SIZE 0x20000u

#define E1000_CTRL 0x0000u
#define E1000_CTRL_EXT 0x0018u
#define E1000_STATUS 0x0008u
#define E1000_EERD 0x0014u
#define E1000_ICR 0x00c0u
#define E1000_ITR 0x00c4u
#define E1000_ICS 0x00c8u
#define E1000_IMS 0x00d0u
#define E1000_IMC 0x00d8u
#define E1000_IAM 0x00e0u
#define E1000_RCTL 0x0100u
#define E1000_TCTL 0x0400u
#define E1000_TIPG 0x0410u
#define E1000_RDBAL 0x2800u
#define E1000_RDBAH 0x2804u
#define E1000_RDLEN 0x2808u
#define E1000_RDH 0x2810u
#define E1000_RDT 0x2818u
#define E1000_RDTR 0x2820u
#define E1000_RADV 0x282cu
#define E1000_TDBAL 0x3800u
#define E1000_TDBAH 0x3804u
#define E1000_TDLEN 0x3808u
#define E1000_TDH 0x3810u
#define E1000_TDT 0x3818u
#define E1000_TIDV 0x3820u
#define E1000_TADV 0x382cu
#define E1000_TARC0 0x3840u
#define E1000_TARC1 0x3940u
#define E1000_RXCSUM 0x5000u
#define E1000_RFCTL 0x5008u
#define E1000_MTA 0x5200u
#define E1000_RAL0 0x5400u
#define E1000_RAH0 0x5404u
#define E1000_MRQC 0x5818u

#define E1000_CTRL_SLU (1u << 6)
#define E1000_CTRL_RST (1u << 26)
#define E1000_STATUS_LU (1u << 1)
#define E1000_RCTL_EN (1u << 1)
#define E1000_RCTL_MPE (1u << 4)
#define E1000_RCTL_BAM (1u << 15)
#define E1000_RCTL_SECRC (1u << 26)
#define E1000_RCTL_VFE (1u << 18)
#define E1000_RCTL_DTYP_MASK (3u << 10)
#define E1000_TCTL_EN (1u << 1)
#define E1000_TCTL_PSP (1u << 3)
#define E1000E_TARC_ENABLE (1u << 10)
#define E1000E_TARC_82574_ERRATA (1u << 26)
#define E1000E_RFCTL_EXTEN (1u << 15)
#define E1000E_CTRL_EXT_VME (1u << 30)
#define E1000_INT_TXDW (1u << 0)
#define E1000_INT_LSC (1u << 2)
#define E1000_INT_RXDMT0 (1u << 4)
#define E1000_INT_RXO (1u << 6)
#define E1000_INT_RXT0 (1u << 7)
#define E1000_INT_MASK (E1000_INT_TXDW | E1000_INT_LSC | E1000_INT_RXDMT0 | \
                        E1000_INT_RXO | E1000_INT_RXT0)
#define E1000_RX_DD 1u
#define E1000_RX_EOP (1u << 1)
#define E1000_TX_EOP 1u
#define E1000_TX_IFCS (1u << 1)
#define E1000_TX_RS (1u << 3)
#define E1000_TX_DD 1u
#define E1000_TX_ERRORS 0x0eu
#define E1000_RAH_AV (1u << 31)
#define E1000_EERD_START 1u
#define E1000_EERD_DONE (1u << 4)
#define E1000_EERD_ADDR_SHIFT 8
#define E1000_EERD_DATA_SHIFT 16

/* 82574L (QEMU e1000e) EERD uses the revised field positions. */
#define E1000E_EERD_START 1u
#define E1000E_EERD_DONE (1u << 1)
#define E1000E_EERD_ADDR_SHIFT 2
#define E1000E_EERD_DATA_SHIFT 16

#endif
