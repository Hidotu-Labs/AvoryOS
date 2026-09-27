#ifndef NVME_NVME_H
#define NVME_NVME_H

/*
 * NVMe controller driver — register definitions and public API.
 *
 * Phases 2-7 scope: admin queue pair (depth 64), Identify controller and every
 * namespace, per-CPU I/O queue pairs with interrupt completions, block devices
 * per namespace, 4Kn media via 512-byte virtual sectors with RMW, chained PRP
 * lists, FUA writes, graceful CC.SHN, reset recovery and the hardening audit
 * (integer bounds, DMA lifetime, IRQ teardown, CID exhaustion, barriers and
 * bounce zeroing).  The constants for later phases live here so the register
 * layout has a single source of truth; docs/nvme.md maps each audit item to
 * its implementation and test.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Controller registers, NVMe 1.4 §3.1.  Offsets from BAR0. */
#define NVME_REG_CAP 0x0000
#define NVME_REG_VS 0x0008
#define NVME_REG_INTMS 0x000C
#define NVME_REG_INTMC 0x0010
#define NVME_REG_CC 0x0014
#define NVME_REG_CSTS 0x001C
#define NVME_REG_NSSR 0x0020
#define NVME_REG_AQA 0x0024
#define NVME_REG_ASQ 0x0028
#define NVME_REG_ACQ 0x0030

typedef struct __attribute__((packed)) {
  uint64_t cap;      /* 0x00 Controller Capabilities */
  uint32_t vs;       /* 0x08 Version */
  uint32_t intms;    /* 0x0C Interrupt Mask Set */
  uint32_t intmc;    /* 0x10 Interrupt Mask Clear */
  uint32_t cc;       /* 0x14 Controller Configuration */
  uint32_t reserved0;/* 0x18 */
  uint32_t csts;     /* 0x1C Controller Status */
  uint32_t nssr;     /* 0x20 NVM Subsystem Reset */
  uint32_t aqa;      /* 0x24 Admin Queue Attributes */
  uint64_t asq;      /* 0x28 Admin Submission Queue Base */
  uint64_t acq;      /* 0x30 Admin Completion Queue Base */
  uint32_t cmbloc;   /* 0x38 Controller Memory Buffer Location */
  uint32_t cmbsz;    /* 0x3C Controller Memory Buffer Size */
} nvme_regs_t;

_Static_assert(offsetof(nvme_regs_t, vs) == NVME_REG_VS, "NVMe VS offset");
_Static_assert(offsetof(nvme_regs_t, cc) == NVME_REG_CC, "NVMe CC offset");
_Static_assert(offsetof(nvme_regs_t, csts) == NVME_REG_CSTS, "NVMe CSTS offset");
_Static_assert(offsetof(nvme_regs_t, aqa) == NVME_REG_AQA, "NVMe AQA offset");
_Static_assert(offsetof(nvme_regs_t, asq) == NVME_REG_ASQ, "NVMe ASQ offset");
_Static_assert(offsetof(nvme_regs_t, acq) == NVME_REG_ACQ, "NVMe ACQ offset");
_Static_assert(sizeof(nvme_regs_t) == 0x40, "NVMe register block size");

/* CAP fields (NVMe 1.4 §3.1.1). */
#define NVME_CAP_MQES(cap) ((uint16_t)((cap) & 0xFFFFu))
#define NVME_CAP_CQR(cap) (((cap) >> 16) & 0x1u)
#define NVME_CAP_TO(cap) (((cap) >> 24) & 0xFFu)
#define NVME_CAP_DSTRD(cap) (((cap) >> 32) & 0xFu)
#define NVME_CAP_NSSRS(cap) (((cap) >> 36) & 0x1u)
#define NVME_CAP_CSS(cap) (((cap) >> 37) & 0xFFu)
#define NVME_CAP_BPS(cap) (((cap) >> 45) & 0x1u)
#define NVME_CAP_MPSMIN(cap) (((cap) >> 48) & 0xFu)
#define NVME_CAP_MPSMAX(cap) (((cap) >> 52) & 0xFu)

/* CC fields. */
#define NVME_CC_EN (1u << 0)
#define NVME_CC_CSS_NVM (0u << 4)
#define NVME_CC_MPS_4K (0u << 7)
#define NVME_CC_SHN_MASK (3u << 14)
#define NVME_CC_SHN_NONE (0u << 14)
#define NVME_CC_SHN_NORMAL (1u << 14)
#define NVME_CC_IOSQES_64 (6u << 16)
#define NVME_CC_IOCQES_16 (4u << 20)

/* CSTS fields. */
#define NVME_CSTS_RDY (1u << 0)
#define NVME_CSTS_CFS (1u << 1)
#define NVME_CSTS_SHST_MASK (3u << 2)
#define NVME_CSTS_SHST_COMPLETE (2u << 2)

/* Doorbells start at 0x1000; stride is 2^(2 + DSTRD) bytes. */
#define NVME_DOORBELL_BASE 0x1000u
#define NVME_DOORBELL_STRIDE(dstrd) (4u << (dstrd))

static inline uint32_t nvme_sq_doorbell_offset(uint16_t qid, uint8_t dstrd) {
  return NVME_DOORBELL_BASE +
         (uint32_t)(2u * (uint32_t)qid) * NVME_DOORBELL_STRIDE(dstrd);
}

static inline uint32_t nvme_cq_doorbell_offset(uint16_t qid, uint8_t dstrd) {
  return NVME_DOORBELL_BASE +
         (uint32_t)(2u * (uint32_t)qid + 1u) * NVME_DOORBELL_STRIDE(dstrd);
}

/* Queue geometry. */
#define NVME_QUEUE_ENTRY_SIZE_SQ 64u
#define NVME_QUEUE_ENTRY_SIZE_CQ 16u
#define NVME_ADMIN_QUEUE_DEPTH 64u
#define NVME_IO_QUEUE_DEPTH 64u
/* I/O queue pairs (qid 1..N), one per CPU up to this many. */
#define NVME_MAX_QUEUES 16u
#define NVME_MAX_NAMESPACES 32
#define NVME_IDENTIFY_SIZE 4096u
#define NVME_PAGE_SIZE 4096u
/*
 * Largest single command the driver builds.  PRP1 covers the first page; the
 * remainder is described by up to NVME_PRP_LIST_PAGES chained list pages,
 * which hold (511 + 511 + 512) entries in the worst case, so 4 MiB always fits
 * even for a buffer whose first page holds only 512 bytes.
 */
#define NVME_MAX_TRANSFER (4u * 1024u * 1024u)
/* PRP list entries per page (8 bytes each). */
#define NVME_PRP_LIST_ENTRIES 512u
/* Chained list pages per command (the third gains headroom for misalignment). */
#define NVME_PRP_LIST_PAGES 3u
/* Worst-case data pages a full chain can describe. */
#define NVME_MAX_PRP_ENTRIES (NVME_PRP_LIST_ENTRIES + 511u + 511u)

/* LBA formats accepted by the driver: 512-byte through 4096-byte logical
 * blocks, no metadata.  Larger LBAs are byte-translated to 512-byte virtual
 * sectors and sub-LBA writes go through a read-modify-write bounce. */
#define NVME_MIN_LBA_SHIFT 9u
#define NVME_MAX_LBA_SHIFT 12u

/* Admin command opcodes (NVMe 1.4 §5.1). */
#define NVME_ADMIN_DELETE_SQ 0x00
#define NVME_ADMIN_CREATE_SQ 0x01
#define NVME_ADMIN_GET_LOG_PAGE 0x02
#define NVME_ADMIN_DELETE_CQ 0x04
#define NVME_ADMIN_CREATE_CQ 0x05
#define NVME_ADMIN_IDENTIFY 0x06
#define NVME_ADMIN_ABORT 0x08
#define NVME_ADMIN_SET_FEATURES 0x09
#define NVME_ADMIN_GET_FEATURES 0x0A
#define NVME_ADMIN_ASYNC_EVENT 0x0C

/* NVM command opcodes (NVMe 1.4 §6). */
#define NVME_CMD_FLUSH 0x00
#define NVME_CMD_WRITE 0x01
#define NVME_CMD_READ 0x02

/* Identify CNS values. */
#define NVME_ID_CNS_NAMESPACE 0x00
#define NVME_ID_CNS_CONTROLLER 0x01
#define NVME_ID_CNS_ACTIVE_NS 0x02
#define NVME_ID_CNS_NS_DESC_LIST 0x03

/* Create CQ/SQ CDW11 bits. */
#define NVME_CQ_PC (1u << 0)
#define NVME_CQ_IEN (1u << 1)
#define NVME_SQ_PC (1u << 0)
#define NVME_SQ_QPRIO(v) (((uint32_t)(v)&0x3u) << 1)
#define NVME_CQ_IV(v) (((uint32_t)(v)&0xFFFFu) << 16)
#define NVME_SQ_CQID(v) (((uint32_t)(v)&0xFFFFu) << 16)

/* Read/Write CDW12: NLB bits 15:0, control bits 31:16.  FUA is bit 14 of the
 * control half (NVMe 1.4 Figure 106), i.e. bit 30 of the raw dword. */
#define NVME_RW_FUA (1u << 30)

/* Completion queue entry status word. */
#define NVME_CQE_PHASE (1u << 0)
#define NVME_CQE_STATUS_MASK 0xFFFEu
#define NVME_CQE_SC(status) (((status) >> 1) & 0xFFu)
#define NVME_CQE_SCT(status) (((status) >> 9) & 0x7u)
#define NVME_CQE_MORE(status) (((status) >> 14) & 0x1u)
#define NVME_CQE_DNR(status) (((status) >> 15) & 0x1u)

/* Status codes worth naming. */
#define NVME_SC_SUCCESS 0x00
#define NVME_SC_INVALID_OPCODE 0x01
#define NVME_SC_INVALID_FIELD 0x02
#define NVME_SC_INVALID_NSID 0x0B
#define NVME_SC_LBA_RANGE 0x80

/* Identify Controller Data Structure offsets (NVMe 1.4 §5.15.2). */
#define NVME_ID_CTRL_VID 0
#define NVME_ID_CTRL_SSVID 2
#define NVME_ID_CTRL_SN 4
#define NVME_ID_CTRL_MN 24
#define NVME_ID_CTRL_FR 64
#define NVME_ID_CTRL_RAB 72
#define NVME_ID_CTRL_IEEE 73
#define NVME_ID_CTRL_CMIC 76
#define NVME_ID_CTRL_MDTS 77
#define NVME_ID_CTRL_CNTLID 78
#define NVME_ID_CTRL_VER 80
#define NVME_ID_CTRL_SQES 512
#define NVME_ID_CTRL_CQES 513
#define NVME_ID_CTRL_MAXCMD 514
#define NVME_ID_CTRL_NN 516
#define NVME_ID_CTRL_ONCS 520
#define NVME_ID_CTRL_VWC 525

/* Identify Namespace Data Structure offsets (NVMe 1.4 §5.15.3). */
#define NVME_ID_NS_NSZE 0
#define NVME_ID_NS_NCAP 8
#define NVME_ID_NS_NUSE 16
#define NVME_ID_NS_NSFEAT 24
#define NVME_ID_NS_NLBAF 25
#define NVME_ID_NS_FLBAS 26
#define NVME_ID_NS_MC 27
#define NVME_ID_NS_DPC 28
#define NVME_ID_NS_DPS 29
#define NVME_ID_NS_LBAF 128
#define NVME_ID_NS_LBAF_MAX 64

/* LBA Format field. */
#define NVME_LBAF_RP(f) ((uint8_t)((f) & 0x3u))
#define NVME_LBAF_LBADS(f) ((uint8_t)(((f) >> 16) & 0xFFu))
#define NVME_LBAF_MS(f) ((uint16_t)(((f) >> 24) & 0xFFFFu))

/* Admin/IO submission queue entry (NVMe 1.4 Figure 82). */
typedef struct __attribute__((packed)) {
  uint8_t opcode;
  uint8_t flags;
  uint16_t cid;
  uint32_t nsid;
  uint32_t reserved0;
  uint32_t reserved1;
  uint64_t mptr;
  uint64_t prp1;
  uint64_t prp2;
  uint32_t cdw10;
  uint32_t cdw11;
  uint32_t cdw12;
  uint32_t cdw13;
  uint32_t cdw14;
  uint32_t cdw15;
} nvme_command_t;

_Static_assert(sizeof(nvme_command_t) == NVME_QUEUE_ENTRY_SIZE_SQ,
               "NVMe SQE must be 64 bytes");

/* Completion queue entry (NVMe 1.4 Figure 87). */
typedef struct __attribute__((packed)) {
  uint32_t result;
  uint32_t reserved;
  uint16_t sq_head;
  uint16_t sq_id;
  uint16_t cid;
  uint16_t status; /* bit 0: phase; bits 15:1: status code */
} nvme_completion_t;

_Static_assert(sizeof(nvme_completion_t) == NVME_QUEUE_ENTRY_SIZE_CQ,
               "NVMe CQE must be 16 bytes");

/*
 * Probe every NVMe PCI function (class 0x01, subclass 0x08), bring up admin
 * and I/O queues, identify namespaces and register one block device per
 * namespace (nvme0n1, ...).  Returns the number of controllers initialized.
 */
int nvme_init(void);

unsigned nvme_controller_count(void);
unsigned nvme_namespace_count(void);

/*
 * Graceful controller shutdown: stop I/O, request CC.SHN=normal and wait for
 * CSTS.SHST to report completion before clearing CC.EN.  Called from the
 * reboot/power-off syscall path; a no-op when no controller is started.
 */
void nvme_shutdown(void);

#ifdef NVME_SELFTEST
/*
 * Bring every controller down and back up `cycles` times, checking the
 * arithmetic after each disable->enable->identify cycle.  Additionally issues
 * `identify_ops` Identify commands on the last live controller.  0 on success.
 */
int nvme_test_admin_cycles(unsigned cycles, unsigned identify_ops);

/*
 * Phase 5 checks.  Each returns 0 when it ran and passed, a positive failure
 * count, or -1 when the configuration has nothing to test (skip).  The data
 * tests are destructive to the first sectors of scratch media and only do
 * anything when built with NVME_SELFTEST_DATA=1.
 */
int nvme_test_prp_chain(void);        /* chained PRP list structure */
int nvme_test_shutdown_cycle(void);   /* graceful CC.SHN + re-enable */
int nvme_test_4kn(void);              /* 4Kn RMW edge coverage */
int nvme_test_multi_ns(void);         /* namespace isolation */
int nvme_test_recovery(void);         /* timeout -> reset -> retry */
int nvme_test_audit(void);            /* Phase 7 bounds/lifetime audit */
#endif

#endif /* NVME_NVME_H */
