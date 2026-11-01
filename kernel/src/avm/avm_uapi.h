#ifndef AVM_AVM_UAPI_H
#define AVM_AVM_UAPI_H

/* Kernel-side copy of the /dev/kvm ABI subset that phase 1 needs.
 *
 * Struct layouts and ioctl numbers match the public KVM ABI (the uapi header
 * in kernel/linux/include/uapi/linux/kvm.h).  Behaviour is written from the
 * design doc, not from Linux's implementation. */

#include <stdint.h>

#define KVM_API_VERSION 12

#define KVMIO 0xAE

#define KVM_IOC_NONE 0u
#define KVM_IOC_WRITE 1u
#define KVM_IOC_READ 2u

#define KVM_IOC(dir, type, nr, size)                                          \
  (((uint32_t)(dir) << 30) | ((uint32_t)(size) << 16) |                        \
   ((uint32_t)(type) << 8) | (uint32_t)(nr))

/* System ioctls, on the /dev/kvm fd. */
#define KVM_GET_API_VERSION KVM_IOC(KVM_IOC_NONE, KVMIO, 0x00, 0)
#define KVM_CREATE_VM KVM_IOC(KVM_IOC_NONE, KVMIO, 0x01, 0)
#define KVM_GET_VCPU_MMAP_SIZE KVM_IOC(KVM_IOC_NONE, KVMIO, 0x04, 0)
#define KVM_CHECK_EXTENSION KVM_IOC(KVM_IOC_NONE, KVMIO, 0x03, 0)
#define KVM_GET_MSR_INDEX_LIST KVM_IOC(KVM_IOC_READ | KVM_IOC_WRITE, KVMIO, 0x02, sizeof(struct kvm_msr_list))
#define KVM_GET_MSR_FEATURE_INDEX_LIST KVM_IOC(KVM_IOC_READ | KVM_IOC_WRITE, KVMIO, 0x0a, sizeof(struct kvm_msr_list))
#define KVM_GET_SUPPORTED_CPUID KVM_IOC(KVM_IOC_READ | KVM_IOC_WRITE, KVMIO, 0x05, sizeof(struct kvm_cpuid2))

/* Capabilities reported by KVM_CHECK_EXTENSION (numbers from kvm.h). */
#define KVM_CAP_IRQCHIP 0
#define KVM_CAP_USER_MEMORY 3
#define KVM_CAP_EXT_CPUID 7
#define KVM_CAP_NR_VCPUS 9
#define KVM_CAP_NR_MEMSLOTS 10
#define KVM_CAP_DESTROY_MEMORY_REGION_WORKS 21
#define KVM_CAP_JOIN_MEMORY_REGIONS_WORKS 30
#define KVM_CAP_INTERNAL_ERROR_DATA 40
#define KVM_CAP_IOEVENTFD 36
#define KVM_CAP_PIT2 33
#define KVM_CAP_MAX_VCPUS 66
#define KVM_CAP_SET_TSS_ADDR 4
#define KVM_CAP_MP_STATE 14
#define KVM_CAP_IRQ_ROUTING 25
#define KVM_CAP_IRQFD 32
#define KVM_CAP_SET_IDENTITY_MAP_ADDR 37
#define KVM_CAP_ADJUST_CLOCK 39
#define KVM_CAP_VCPU_EVENTS 41
#define KVM_CAP_DEBUGREGS 50
#define KVM_CAP_XSAVE 55
#define KVM_CAP_XCRS 56
#define KVM_CAP_GET_TSC_KHZ 61
#define KVM_CAP_SIGNAL_MSI 77
#define KVM_CAP_IOEVENTFD_ANY_LENGTH 122
#define KVM_CAP_SET_GUEST_DEBUG 23
#define KVM_CAP_MCE 31
#define KVM_CAP_PIT_STATE2 35
#define KVM_CAP_X86_ROBUST_SINGLESTEP 51
#define KVM_CAP_READONLY_MEM 81
#define KVM_CAP_IMMEDIATE_EXIT 136

/* VM ioctls, on a VM fd. */
#define KVM_CREATE_IRQCHIP KVM_IOC(KVM_IOC_NONE, KVMIO, 0x60, 0)
#define KVM_IRQ_LINE KVM_IOC(KVM_IOC_WRITE, KVMIO, 0x61, sizeof(struct kvm_irq_level))
#define KVM_GET_IRQCHIP KVM_IOC(KVM_IOC_READ | KVM_IOC_WRITE, KVMIO, 0x62, sizeof(struct kvm_irqchip))
#define KVM_SET_IRQCHIP KVM_IOC(KVM_IOC_READ, KVMIO, 0x63, sizeof(struct kvm_irqchip))
#define KVM_GET_PIT2 KVM_IOC(KVM_IOC_READ, KVMIO, 0x9F, sizeof(struct kvm_pit_state2))
#define KVM_SET_PIT2 KVM_IOC(KVM_IOC_WRITE, KVMIO, 0xA0, sizeof(struct kvm_pit_state2))
#define KVM_IOEVENTFD KVM_IOC(KVM_IOC_WRITE, KVMIO, 0x79, sizeof(struct kvm_ioeventfd))
#define KVM_ENABLE_CAP KVM_IOC(KVM_IOC_WRITE, KVMIO, 0xA3, sizeof(struct kvm_enable_cap))
#define KVM_SET_TSS_ADDR KVM_IOC(KVM_IOC_NONE, KVMIO, 0x47, 0)
#define KVM_SET_IDENTITY_MAP_ADDR KVM_IOC(KVM_IOC_WRITE, KVMIO, 0x48, sizeof(uint64_t))
#define KVM_SET_GSI_ROUTING KVM_IOC(KVM_IOC_WRITE, KVMIO, 0x6A, sizeof(struct kvm_irq_routing))
#define KVM_IRQFD KVM_IOC(KVM_IOC_WRITE, KVMIO, 0x76, sizeof(struct kvm_irqfd))
#define KVM_SET_CLOCK KVM_IOC(KVM_IOC_WRITE, KVMIO, 0x7B, sizeof(struct kvm_clock_data))
#define KVM_GET_CLOCK KVM_IOC(KVM_IOC_READ, KVMIO, 0x7C, sizeof(struct kvm_clock_data))
#define KVM_SIGNAL_MSI KVM_IOC(KVM_IOC_WRITE, KVMIO, 0xA5, sizeof(struct kvm_msi))
#define KVM_CREATE_PIT2 KVM_IOC(KVM_IOC_WRITE, KVMIO, 0x77, sizeof(struct kvm_pit_config))
#define KVM_CREATE_VCPU KVM_IOC(KVM_IOC_NONE, KVMIO, 0x41, 0)
#define KVM_SET_USER_MEMORY_REGION                                            \
  KVM_IOC(KVM_IOC_WRITE, KVMIO, 0x46, sizeof(struct kvm_userspace_memory_region))
#define KVM_GET_DIRTY_LOG KVM_IOC(KVM_IOC_WRITE, KVMIO, 0x42, sizeof(struct kvm_dirty_log))

/* vCPU ioctls, on a vCPU fd. */
#define KVM_RUN KVM_IOC(KVM_IOC_NONE, KVMIO, 0x80, 0)
#define KVM_SET_REGS KVM_IOC(KVM_IOC_WRITE, KVMIO, 0x82, sizeof(struct kvm_regs))
#define KVM_SET_SREGS KVM_IOC(KVM_IOC_WRITE, KVMIO, 0x84, sizeof(struct kvm_sregs))
#define KVM_SET_CPUID2 KVM_IOC(KVM_IOC_WRITE, KVMIO, 0x90, sizeof(struct kvm_cpuid2))
#define KVM_GET_MSRS KVM_IOC(KVM_IOC_READ | KVM_IOC_WRITE, KVMIO, 0x88, sizeof(struct kvm_msrs))
#define KVM_SET_MSRS KVM_IOC(KVM_IOC_WRITE, KVMIO, 0x89, sizeof(struct kvm_msrs))
#define KVM_GET_REGS KVM_IOC(KVM_IOC_READ, KVMIO, 0x81, sizeof(struct kvm_regs))
#define KVM_GET_SREGS KVM_IOC(KVM_IOC_READ, KVMIO, 0x83, sizeof(struct kvm_sregs))
#define KVM_SET_SIGNAL_MASK KVM_IOC(KVM_IOC_WRITE, KVMIO, 0x8B, sizeof(struct kvm_signal_mask))
#define KVM_GET_FPU KVM_IOC(KVM_IOC_READ, KVMIO, 0x8C, sizeof(struct kvm_fpu))
#define KVM_SET_FPU KVM_IOC(KVM_IOC_WRITE, KVMIO, 0x8D, sizeof(struct kvm_fpu))
#define KVM_GET_LAPIC KVM_IOC(KVM_IOC_READ, KVMIO, 0x8E, sizeof(struct kvm_lapic_state))
#define KVM_SET_LAPIC KVM_IOC(KVM_IOC_WRITE, KVMIO, 0x8F, sizeof(struct kvm_lapic_state))
#define KVM_GET_MP_STATE KVM_IOC(KVM_IOC_READ, KVMIO, 0x98, sizeof(struct kvm_mp_state))
#define KVM_SET_MP_STATE KVM_IOC(KVM_IOC_WRITE, KVMIO, 0x99, sizeof(struct kvm_mp_state))
#define KVM_GET_VCPU_EVENTS KVM_IOC(KVM_IOC_READ, KVMIO, 0x9F, sizeof(struct kvm_vcpu_events))
#define KVM_SET_VCPU_EVENTS KVM_IOC(KVM_IOC_WRITE, KVMIO, 0xA0, sizeof(struct kvm_vcpu_events))
#define KVM_GET_DEBUGREGS KVM_IOC(KVM_IOC_READ, KVMIO, 0xA1, sizeof(struct kvm_debugregs))
#define KVM_SET_DEBUGREGS KVM_IOC(KVM_IOC_WRITE, KVMIO, 0xA2, sizeof(struct kvm_debugregs))
#define KVM_SET_GUEST_DEBUG KVM_IOC(KVM_IOC_WRITE, KVMIO, 0x9B, sizeof(struct kvm_guest_debug))
#define KVM_X86_SETUP_MCE KVM_IOC(KVM_IOC_WRITE, KVMIO, 0x9C, sizeof(uint64_t))
#define KVM_X86_GET_MCE_CAP_SUPPORTED KVM_IOC(KVM_IOC_READ, KVMIO, 0x9D, sizeof(uint64_t))
#define KVM_X86_SET_MCE KVM_IOC(KVM_IOC_WRITE, KVMIO, 0x9E, sizeof(struct kvm_x86_mce))
#define KVM_SET_TSC_KHZ KVM_IOC(KVM_IOC_NONE, KVMIO, 0xA2, 0)
#define KVM_GET_TSC_KHZ KVM_IOC(KVM_IOC_NONE, KVMIO, 0xA3, 0)
#define KVM_GET_XSAVE KVM_IOC(KVM_IOC_READ, KVMIO, 0xA4, sizeof(struct kvm_xsave))
#define KVM_SET_XSAVE KVM_IOC(KVM_IOC_WRITE, KVMIO, 0xA5, sizeof(struct kvm_xsave))
#define KVM_GET_XCRS KVM_IOC(KVM_IOC_READ, KVMIO, 0xA6, sizeof(struct kvm_xcrs))
#define KVM_SET_XCRS KVM_IOC(KVM_IOC_WRITE, KVMIO, 0xA7, sizeof(struct kvm_xcrs))

/* Exit reasons, in kvm_run.exit_reason. */
#define KVM_EXIT_UNKNOWN 0
#define KVM_MEM_LOG_DIRTY_PAGES (1u << 0)
#define KVM_MEM_READONLY (1u << 1)
#define KVM_EXIT_IO 2
#define KVM_EXIT_HLT 5
#define KVM_EXIT_MMIO 6
#define KVM_EXIT_SHUTDOWN 8
#define KVM_EXIT_INTR 10
#define KVM_EXIT_DEBUG 4
#define KVM_EXIT_INTERNAL_ERROR 17

/* Sub-error codes for KVM_EXIT_INTERNAL_ERROR. */
#define KVM_INTERNAL_ERROR_UNEXPECTED_EXIT_REASON 4

#define KVM_EXIT_IO_IN 0
#define KVM_EXIT_IO_OUT 1

struct kvm_irq_level {
  union {
    uint32_t irq;
    int32_t status;
  };
  uint32_t level;
};

/* KVM_IOEVENTFD flags.  Virtio CCW notify is not supported, so it is not in
 * the valid mask. */
#define KVM_IOEVENTFD_FLAG_DATAMATCH (1u << 0)
#define KVM_IOEVENTFD_FLAG_PIO (1u << 1)
#define KVM_IOEVENTFD_FLAG_DEASSIGN (1u << 2)
#define KVM_IOEVENTFD_VALID_FLAG_MASK 0x7u

struct kvm_ioeventfd {
  uint64_t datamatch;
  uint64_t addr;
  uint32_t len; /* 1, 2, 4 or 8 bytes; 0 matches any length */
  int32_t fd;
  uint32_t flags;
  uint8_t pad[36];
};

struct kvm_pit_config {
  uint32_t flags;
  uint32_t pad[15];
};

struct kvm_userspace_memory_region {
  uint32_t slot;
  uint32_t flags;
  uint64_t guest_phys_addr;
  uint64_t memory_size;
  uint64_t userspace_addr;
};

/* KVM_GET_DIRTY_LOG.  dirty_bitmap is a userspace pointer, one bit per page. */
struct kvm_dirty_log {
  uint32_t slot;
  uint32_t padding1;
  uint64_t dirty_bitmap;
};

struct kvm_regs {
  uint64_t rax, rbx, rcx, rdx;
  uint64_t rsi, rdi, rsp, rbp;
  uint64_t r8, r9, r10, r11;
  uint64_t r12, r13, r14, r15;
  uint64_t rip, rflags;
};

struct kvm_segment {
  uint64_t base;
  uint32_t limit;
  uint16_t selector;
  uint8_t type;
  uint8_t present, dpl, db, s, l, g, avl;
  uint8_t unusable;
  uint8_t padding;
};

struct kvm_dtable {
  uint64_t base;
  uint16_t limit;
  uint16_t padding[3];
};

#define KVM_NR_INTERRUPTS 256

struct kvm_sregs {
  struct kvm_segment cs, ds, es, fs, gs, ss;
  struct kvm_segment tr, ldt;
  struct kvm_dtable gdt, idt;
  uint64_t cr0, cr2, cr3, cr4, cr8;
  uint64_t efer;
  uint64_t apic_base;
  uint64_t interrupt_bitmap[(KVM_NR_INTERRUPTS + 63) / 64];
};

struct kvm_cpuid_entry2 {
  uint32_t function;
  uint32_t index;
  uint32_t flags;
  uint32_t eax, ebx, ecx, edx;
  uint32_t padding[3];
};

#define KVM_CPUID_FLAG_SIGNIFCANT_INDEX (1u << 0)

struct kvm_cpuid2 {
  uint32_t nent;
  uint32_t padding;
  struct kvm_cpuid_entry2 entries[];
};

struct kvm_msr_list {
  uint32_t nmsrs;
  uint32_t indices[];
};

struct kvm_msr_entry {
  uint32_t index;
  uint32_t reserved;
  uint64_t data;
};

struct kvm_msrs {
  uint32_t nmsrs;
  uint32_t pad;
  struct kvm_msr_entry entries[];
};

/* KVM_ENABLE_CAP on a VM.  The layout is 104 bytes, as in the KVM uapi. */
struct kvm_enable_cap {
  uint32_t cap;
  uint32_t flags;
  uint64_t args[4];
  uint8_t pad[64];
};

/* KVM_TPR_ACCESS_REPORTING and KVM_SET_VAPIC_ADDR, as in the KVM uapi. */
struct kvm_tpr_access_ctl {
  uint32_t enabled;
  uint32_t flags;
  uint32_t reserved[8];
};

struct kvm_vapic_addr {
  uint64_t vapic_addr;
};

#define KVM_TPR_ACCESS_REPORTING KVM_IOC(KVM_IOC_READ | KVM_IOC_WRITE, KVMIO, 0x92, sizeof(struct kvm_tpr_access_ctl))
#define KVM_SET_VAPIC_ADDR KVM_IOC(KVM_IOC_WRITE, KVMIO, 0x93, sizeof(struct kvm_vapic_addr))

/* In-kernel PIC, IOAPIC and PIT state (KVM_GET_IRQCHIP, KVM_GET_PIT2). */
#define KVM_IRQCHIP_PIC_MASTER 0
#define KVM_IRQCHIP_PIC_SLAVE 1
#define KVM_IRQCHIP_IOAPIC 2
#define KVM_IOAPIC_NUM_PINS 24
#define KVM_PIT_FLAGS_HPET_LEGACY 0x00000001
#define KVM_PIT_FLAGS_SPEAKER_DATA_ON 0x00000002

struct kvm_pic_state {
  uint8_t last_irr;
  uint8_t irr;
  uint8_t imr;
  uint8_t isr;
  uint8_t priority_add;
  uint8_t irq_base;
  uint8_t read_reg_select;
  uint8_t poll;
  uint8_t special_mask;
  uint8_t init_state;
  uint8_t auto_eoi;
  uint8_t rotate_on_auto_eoi;
  uint8_t special_fully_nested_mode;
  uint8_t init4;
  uint8_t elcr;
  uint8_t elcr_mask;
};

/* The redirection entries are kept in the hardware format, as in the uapi
 * bitfield union. */
struct kvm_ioapic_state {
  uint64_t base_address;
  uint32_t ioregsel;
  uint32_t id;
  uint32_t irr;
  uint32_t pad;
  uint64_t redirtbl[KVM_IOAPIC_NUM_PINS];
};

struct kvm_irqchip {
  uint32_t chip_id;
  uint32_t pad;
  union {
    char dummy[512];
    struct kvm_pic_state pic;
    struct kvm_ioapic_state ioapic;
  } chip;
};

struct kvm_pit_channel_state {
  uint32_t count;
  uint16_t latched_count;
  uint8_t count_latched;
  uint8_t status_latched;
  uint8_t status;
  uint8_t read_state;
  uint8_t write_state;
  uint8_t write_latch;
  uint8_t rw_mode;
  uint8_t mode;
  uint8_t bcd;
  uint8_t gate;
  int64_t count_load_time;
};

struct kvm_pit_state2 {
  struct kvm_pit_channel_state channels[3];
  uint32_t flags;
  uint32_t reserved[9];
};

/* Guest debugging (KVM_SET_GUEST_DEBUG).  Single-step is supported; hardware
 * and software breakpoints are not. */
#define KVM_GUESTDBG_ENABLE 0x00000001
#define KVM_GUESTDBG_SINGLESTEP 0x00000002
#define KVM_GUESTDBG_USE_SW_BP 0x00010000
#define KVM_GUESTDBG_USE_HW_BP 0x00020000
#define KVM_GUESTDBG_INJECT_DB 0x00040000
#define KVM_GUESTDBG_INJECT_BP 0x00080000
#define KVM_GUESTDBG_BLOCKIRQ 0x00100000

struct kvm_guest_debug {
  uint32_t control;
  uint32_t pad;
  struct {
    uint64_t debugreg[8];
  } arch;
};

struct kvm_debug_exit_arch {
  uint32_t exception;
  uint32_t pad;
  uint64_t pc;
  uint64_t dr6;
  uint64_t dr7;
};

/* KVM_X86_SET_MCE.  Machine-check injection is not supported. */
struct kvm_x86_mce {
  uint64_t status;
  uint64_t addr;
  uint64_t misc;
  uint64_t mcg_status;
  uint8_t bank;
  uint8_t pad1[7];
  uint64_t pad2[3];
};

/* Interrupt routing.  The chip ids and entry types are the uapi values. */
#define KVM_IRQCHIP_PIC_MASTER 0
#define KVM_IRQCHIP_PIC_SLAVE 1
#define KVM_IRQCHIP_IOAPIC 2
#define KVM_IRQ_ROUTING_IRQCHIP 1
#define KVM_IRQ_ROUTING_MSI 2

struct kvm_irq_routing_irqchip {
  uint32_t irqchip;
  uint32_t pin;
};

struct kvm_irq_routing_msi {
  uint32_t address_lo;
  uint32_t address_hi;
  uint32_t data;
  union {
    uint32_t pad;
    uint32_t devid;
  };
};

struct kvm_irq_routing_entry {
  uint32_t gsi;
  uint32_t type;
  uint32_t flags;
  uint32_t pad;
  union {
    struct kvm_irq_routing_irqchip irqchip;
    struct kvm_irq_routing_msi msi;
    uint32_t pad[8];
  } u;
};

struct kvm_irq_routing {
  uint32_t nr;
  uint32_t flags;
  struct kvm_irq_routing_entry entries[];
};

#define KVM_IRQFD_FLAG_DEASSIGN (1u << 0)
#define KVM_IRQFD_FLAG_RESAMPLE (1u << 1)

struct kvm_irqfd {
  uint32_t fd;
  uint32_t gsi;
  uint32_t flags;
  uint32_t resamplefd;
  uint8_t pad[16];
};

struct kvm_msi {
  uint32_t address_lo;
  uint32_t address_hi;
  uint32_t data;
  uint32_t flags;
  uint32_t devid;
  uint8_t pad[12];
};

struct kvm_clock_data {
  uint64_t clock;
  uint32_t flags;
  uint32_t pad0;
  uint64_t realtime;
  uint64_t host_tsc;
  uint32_t pad[4];
};

/* Signals blocked while KVM_RUN runs, as a sigset of len bytes. */
struct kvm_signal_mask {
  uint32_t len;
  uint8_t sigset[];
};

struct kvm_fpu {
  uint8_t fpr[8][16];
  uint16_t fcw;
  uint16_t fsw;
  uint8_t ftwx; /* in fxsave format */
  uint8_t pad1;
  uint16_t last_opcode;
  uint64_t last_ip;
  uint64_t last_dp;
  uint8_t xmm[16][16];
  uint32_t mxcsr;
  uint32_t pad2;
};

#define KVM_APIC_REG_SIZE 0x400
struct kvm_lapic_state {
  char regs[KVM_APIC_REG_SIZE];
};

#define KVM_MP_STATE_RUNNABLE 0
#define KVM_MP_STATE_UNINITIALIZED 1
#define KVM_MP_STATE_INIT_RECEIVED 2

struct kvm_mp_state {
  uint32_t mp_state;
};

struct kvm_vcpu_events {
  struct {
    uint8_t injected;
    uint8_t nr;
    uint8_t has_error_code;
    uint8_t pending;
    uint32_t error_code;
  } exception;
  struct {
    uint8_t injected;
    uint8_t nr;
    uint8_t soft;
    uint8_t shadow;
  } interrupt;
  struct {
    uint8_t injected;
    uint8_t pending;
    uint8_t masked;
    uint8_t pad;
  } nmi;
  uint32_t sipi_vector;
  uint32_t flags;
  struct {
    uint8_t smm;
    uint8_t pending;
    uint8_t smm_inside_nmi;
    uint8_t latched_init;
  } smi;
  struct {
    uint8_t pending;
  } triple_fault;
  uint8_t reserved[26];
  uint8_t exception_has_payload;
  uint64_t exception_payload;
};

struct kvm_debugregs {
  uint64_t db[4];
  uint64_t dr6;
  uint64_t dr7;
  uint64_t flags;
  uint64_t reserved[9];
};

/* The 4 KB XSAVE layout; the state components follow CPUID leaf 0xD. */
struct kvm_xsave {
  uint32_t region[1024];
  uint32_t extra[];
};

struct kvm_xcr {
  uint32_t xcr;
  uint32_t reserved;
  uint64_t value;
};

#define KVM_MAX_XCRS 16

struct kvm_xcrs {
  uint32_t nr_xcrs;
  uint32_t flags;
  struct kvm_xcr xcrs[KVM_MAX_XCRS];
  uint64_t padding[16];
};

/* The shared run page, mapped at offset 0 of a vCPU fd.  Only the fields
 * phase 1 writes are spelled out; the union is padded to its ABI size. */
struct kvm_run {
  uint8_t request_interrupt_window;
  uint8_t immediate_exit;
  uint8_t padding1[6];

  uint32_t exit_reason;
  uint8_t ready_for_interrupt_injection;
  uint8_t if_flag;
  uint16_t flags;

  uint64_t cr8;
  uint64_t apic_base;

  union {
    struct {
      uint64_t hardware_exit_reason;
    } hw;
    struct {
      uint32_t suberror;
      uint32_t ndata;
      uint64_t data[16];
    } internal;
    struct {
      uint8_t direction;
      uint8_t size;
      uint16_t port;
      uint32_t count;
      uint64_t data_offset;
    } io;
    struct {
      uint64_t phys_addr;
      uint8_t data[8];
      uint32_t len;
      uint8_t is_write;
    } mmio;
    struct {
      struct kvm_debug_exit_arch arch;
    } debug;
    uint8_t pad[256];
  };
};

#endif /* AVM_AVM_UAPI_H */
