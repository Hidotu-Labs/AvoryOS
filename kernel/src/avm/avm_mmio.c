#include "avm_mmio.h"

#include "avm_ioapic.h"
#include "avm_ioeventfd.h"
#include "avm_irq.h"
#include "avm_lapic.h"
#include "avm_smp.h"
#include "svm.h"

#include "../lib/string.h"
#include "../mm/pmm.h"

#define APIC_BASE_GPA 0xFEE00000ull
#define IOAPIC_BASE_GPA 0xFEC00000ull
#define MMIO_WINDOW 0x1000ull

#define VMQ(v, off) (*(volatile uint64_t *)((uint8_t *)(v) + (off)))
#define VMW(v, off) (*(volatile uint16_t *)((uint8_t *)(v) + (off)))

#define PREFIX_OPSIZE 0x66
#define PREFIX_ADDRSIZE 0x67
#define OP_MOV_RM8_R8 0x88
#define OP_MOV_RM_R 0x89
#define OP_MOV_R8_RM8 0x8A
#define OP_MOV_R_RM 0x8B
#define OP_MOV_AL_MOFFS 0xA0
#define OP_MOV_AX_MOFFS 0xA1
#define OP_MOV_MOFFS_AL 0xA2
#define OP_MOV_MOFFS_AX 0xA3
#define OP_MOV_RM8_IMM8 0xC6
#define OP_MOV_RM_IMM 0xC7
#define OP_MOVS_B 0xA4
#define OP_MOVS_W 0xA5
#define OP_STOS_B 0xAA
#define OP_STOS_W 0xAB
#define OP_LODS_B 0xAC
#define OP_LODS_W 0xAD
#define PREFIX_REPNE 0xF2
#define PREFIX_REP 0xF3
#define RFLAGS_DF (1ull << 10)

/* Register indexes, in x86 encoding order: AX CX DX BX SP BP SI DI. */
#define REG_AX 0
#define REG_CX 1
#define REG_DX 2
#define REG_BX 3
#define REG_SP 4
#define REG_BP 5
#define REG_SI 6
#define REG_DI 7

struct avm_insn {
  uint8_t len;     /* instruction length in bytes */
  uint8_t size;    /* operand size: 1, 2 or 4 */
  bool write;      /* stores to memory */
  bool reg8;       /* the register operand is 8-bit */
  bool store_imm;  /* a store takes its value from imm, not a register */
  uint8_t reg;     /* register operand, 0-7 */
  uint64_t gpa;    /* address of the memory operand */
  uint64_t imm;
  /* String instructions: one element per exit. */
  bool str;
  bool rep;
  bool movs;      /* MOVS: stores the element read from src */
  bool lods;      /* LODS: loads AX */
  uint64_t dst;   /* ES:DI, the store address for STOS and MOVS */
  uint64_t src;   /* DS:SI (or the override), the load address */
  uint64_t step;  /* the index register's change per element, negated for DF */
  uint64_t mask;  /* address-size mask for SI, DI and CX */
};

int avm_mmio_guest_read(struct avm_vm *vm, uint64_t gpa, void *buf, uint64_t len) {
  for (uint32_t i = 0; i < AVM_MAX_SLOTS; i++) {
    const struct avm_slot *s = &vm->slots[i];
    if (!s->used || gpa < s->gpa || gpa + len > s->gpa + s->size)
      continue;
    memcpy(buf, (const void *)(s->hva + (gpa - s->gpa)), len);
    return 0;
  }
  return -1;
}

int avm_mmio_guest_write(struct avm_vm *vm, uint64_t gpa, const void *buf, uint64_t len) {
  for (uint32_t i = 0; i < AVM_MAX_SLOTS; i++) {
    const struct avm_slot *s = &vm->slots[i];
    if (!s->used || s->readonly || gpa < s->gpa || gpa + len > s->gpa + s->size)
      continue;
    memcpy((void *)(s->hva + (gpa - s->gpa)), buf, len);
    return 0;
  }
  return -1;
}

static uint64_t *gpr_ptr(struct avm_vcpu *vcpu, uint8_t idx) {
  struct kvm_regs *r = &vcpu->regs;
  switch (idx) {
  case REG_AX: return &r->rax;
  case REG_CX: return &r->rcx;
  case REG_DX: return &r->rdx;
  case REG_BX: return &r->rbx;
  case REG_SP: return &r->rsp;
  case REG_BP: return &r->rbp;
  case REG_SI: return &r->rsi;
  default: return &r->rdi;
  }
}

static uint64_t gpr_get(struct avm_vcpu *vcpu, uint8_t idx) {
  return *gpr_ptr(vcpu, idx);
}

/* An 8-bit operand with index 4-7 names AH, CH, DH or BH.  A 16-bit write
 * keeps the upper bits; a 32-bit write clears them, as the architecture does. */
static uint64_t reg_value(struct avm_vcpu *vcpu, uint8_t idx, bool reg8) {
  if (!reg8)
    return gpr_get(vcpu, idx);
  if (idx < 4)
    return gpr_get(vcpu, idx) & 0xFF;
  return (gpr_get(vcpu, idx - 4) >> 8) & 0xFF;
}

static void reg_set(struct avm_vcpu *vcpu, uint8_t idx, bool reg8, uint8_t size, uint64_t value) {
  if (reg8) {
    if (idx < 4) {
      uint64_t *r = gpr_ptr(vcpu, idx);
      *r = (*r & ~0xFFull) | (value & 0xFF);
    } else {
      uint64_t *r = gpr_ptr(vcpu, idx - 4);
      *r = (*r & ~0xFF00ull) | ((value & 0xFF) << 8);
    }
    return;
  }
  uint64_t *r = gpr_ptr(vcpu, idx);
  if (size == 2)
    *r = (*r & ~0xFFFFull) | (value & 0xFFFF);
  else if (size == 4)
    *r = value & 0xFFFFFFFFull;
  else
    *r = value;
}

static uint64_t size_mask(uint8_t size) {
  return size == 1 ? 0xFFull : size == 2 ? 0xFFFFull : 0xFFFFFFFFull;
}

/* 16-bit addressing: base and index registers chosen by the r/m field. */
static uint64_t ea16(struct avm_vcpu *vcpu, uint8_t mod, uint8_t rm,
                     const uint8_t *code, int *p) {
  uint64_t base = 0, index = 0;
  switch (rm) {
  case 0: base = gpr_get(vcpu, REG_BX); index = gpr_get(vcpu, REG_SI); break;
  case 1: base = gpr_get(vcpu, REG_BX); index = gpr_get(vcpu, REG_DI); break;
  case 2: base = gpr_get(vcpu, REG_BP); index = gpr_get(vcpu, REG_SI); break;
  case 3: base = gpr_get(vcpu, REG_BP); index = gpr_get(vcpu, REG_DI); break;
  case 4: base = gpr_get(vcpu, REG_SI); break;
  case 5: base = gpr_get(vcpu, REG_DI); break;
  case 6: base = (mod == 0) ? 0 : gpr_get(vcpu, REG_BP); break;
  default: base = gpr_get(vcpu, REG_BX); break;
  }

  uint64_t disp = 0;
  if (mod == 0 && rm == 6) {
    disp = (uint16_t)(code[*p] | (code[*p + 1] << 8));
    *p += 2;
  } else if (mod == 1) {
    disp = (uint64_t)(int64_t)(int8_t)code[*p];
    *p += 1;
  } else if (mod == 2) {
    disp = (uint16_t)(code[*p] | (code[*p + 1] << 8));
    *p += 2;
  }
  return (base + index + disp) & 0xFFFF;
}

/* 32-bit addressing: optional SIB byte, then base and displacement. */
static uint64_t ea32(struct avm_vcpu *vcpu, uint8_t mod, uint8_t rm,
                     const uint8_t *code, int *p) {
  uint64_t base = 0, index = 0, disp = 0;
  bool disp32_only = false;

  if (rm == 4) {
    uint8_t sib = code[(*p)++];
    uint8_t scale = sib >> 6;
    uint8_t idx = (sib >> 3) & 7;
    uint8_t b = sib & 7;
    if (idx != REG_SP)
      index = gpr_get(vcpu, idx) << scale;
    if (b == 5 && mod == 0)
      disp32_only = true;
    else
      base = gpr_get(vcpu, b);
  } else if (rm == 5 && mod == 0) {
    disp32_only = true;
  } else {
    base = gpr_get(vcpu, rm);
  }

  if (disp32_only || mod == 2) {
    disp = (uint32_t)(code[*p] | (code[*p + 1] << 8) | (code[*p + 2] << 16) |
                      ((uint32_t)code[*p + 3] << 24));
    *p += 4;
  } else if (mod == 1) {
    disp = (uint64_t)(int64_t)(int8_t)code[*p];
    *p += 1;
  }
  return (base + index + disp) & 0xFFFFFFFFull;
}

/* String instructions: MOVS, STOS and LODS.  The index registers and the
 * count are read here, and the element access is made by avm_mmio_fault. */
static void decode_string(struct avm_vcpu *vcpu, struct avm_insn *in, uint8_t op,
                          bool rep, bool opsz16, bool addr16, uint64_t src_base,
                          uint64_t es_base, uint8_t len) {
  uint8_t *v = (uint8_t *)(vcpu->vmcb_pa + pmm_get_hhdm_offset());
  bool byte_op = op == OP_MOVS_B || op == OP_STOS_B || op == OP_LODS_B;
  uint8_t size = byte_op ? 1 : (opsz16 ? 2 : 4);
  bool df = VMQ(v, VMCB_SAVE_RFLAGS) & RFLAGS_DF;

  in->str = true;
  in->rep = rep;
  in->movs = op == OP_MOVS_B || op == OP_MOVS_W;
  in->lods = op == OP_LODS_B || op == OP_LODS_W;
  in->write = !in->lods;
  in->size = size;
  in->reg = REG_AX;
  in->reg8 = byte_op;
  in->len = len;
  in->mask = addr16 ? 0xFFFFull : 0xFFFFFFFFull;
  in->step = df ? 0 - (uint64_t)size : size;
  in->dst = es_base + (vcpu->regs.rdi & in->mask);
  in->src = src_base + (vcpu->regs.rsi & in->mask);
}

/* Decodes the instruction at CS:RIP.  The VMCB gives the segment bases and
 * the code segment's D bit, which chooses 16- or 32-bit defaults. */
static bool decode(struct avm_vcpu *vcpu, struct avm_insn *in) {
  memset(in, 0, sizeof(*in));
  struct avm_vm *vm = vcpu->vm;
  uint8_t *v = (uint8_t *)(vcpu->vmcb_pa + pmm_get_hhdm_offset());
  bool cs_d = (VMW(v, VMCB_SAVE_CS + 2) >> 10) & 1;
  uint64_t cs_base = VMQ(v, VMCB_SAVE_CS + 8);
  uint64_t ds_base = VMQ(v, VMCB_SAVE_DS + 8);
  uint64_t es_base = VMQ(v, VMCB_SAVE_ES + 8);

  uint8_t code[16];
  if (avm_mmio_guest_read(vm, cs_base + vcpu->regs.rip, code, sizeof(code)) != 0)
    return false;

  bool opsz16 = !cs_d;
  bool addr16 = !cs_d;
  uint64_t seg = ds_base;
  bool rep = false;
  int p = 0;
  for (;;) {
    switch (code[p]) {
    case PREFIX_OPSIZE: opsz16 = !opsz16; p++; continue;
    case PREFIX_ADDRSIZE: addr16 = !addr16; p++; continue;
    case PREFIX_REPNE:
    case PREFIX_REP: rep = true; p++; continue;
    case 0x26: seg = VMQ(v, VMCB_SAVE_ES + 8); p++; continue;
    case 0x2E: seg = cs_base; p++; continue;
    case 0x36: seg = VMQ(v, VMCB_SAVE_SS + 8); p++; continue;
    case 0x3E: seg = ds_base; p++; continue;
    case 0x64: seg = VMQ(v, VMCB_SAVE_FS + 8); p++; continue;
    case 0x65: seg = VMQ(v, VMCB_SAVE_GS + 8); p++; continue;
    default: break;
    }
    break;
  }

  in->reg = REG_AX;
  in->imm = 0;
  uint8_t op = code[p++];
  if (op == OP_MOVS_B || op == OP_MOVS_W || op == OP_STOS_B || op == OP_STOS_W ||
      op == OP_LODS_B || op == OP_LODS_W) {
    decode_string(vcpu, in, op, rep, opsz16, addr16, seg, es_base, (uint8_t)p);
    return true;
  }
  bool moffs = op == OP_MOV_AL_MOFFS || op == OP_MOV_AX_MOFFS ||
               op == OP_MOV_MOFFS_AL || op == OP_MOV_MOFFS_AX;
  bool byte_op = op == OP_MOV_RM8_R8 || op == OP_MOV_R8_RM8 ||
                 op == OP_MOV_RM8_IMM8 || op == OP_MOV_AL_MOFFS ||
                 op == OP_MOV_MOFFS_AL;
  in->size = byte_op ? 1 : (opsz16 ? 2 : 4);
  in->reg8 = byte_op && op != OP_MOV_RM8_IMM8;
  in->store_imm = false;

  switch (op) {
  case OP_MOV_RM8_R8:
  case OP_MOV_RM_R:
  case OP_MOV_RM8_IMM8:
  case OP_MOV_RM_IMM:
  case OP_MOV_MOFFS_AL:
  case OP_MOV_MOFFS_AX:
    in->write = true;
    break;
  case OP_MOV_R8_RM8:
  case OP_MOV_R_RM:
  case OP_MOV_AL_MOFFS:
  case OP_MOV_AX_MOFFS:
    in->write = false;
    break;
  default:
    return false;
  }
  in->store_imm = op == OP_MOV_RM8_IMM8 || op == OP_MOV_RM_IMM;

  if (moffs) {
    /* A direct address: the displacement follows the opcode. */
    uint64_t disp = 0;
    int n = addr16 ? 2 : 4;
    for (int i = 0; i < n; i++)
      disp |= (uint64_t)code[p + i] << (8 * i);
    p += n;
    in->reg = REG_AX;
    in->gpa = seg + disp;
  } else {
    uint8_t modrm = code[p++];
    uint8_t mod = modrm >> 6;
    uint8_t regf = (modrm >> 3) & 7;
    uint8_t rm = modrm & 7;
    if (op == OP_MOV_RM8_IMM8 || op == OP_MOV_RM_IMM) {
      if (regf != 0)
        return false;
    } else {
      in->reg = regf;
    }
    if (mod == 3)
      return false;  /* register-to-register, not a memory access */

    uint64_t ea = addr16 ? ea16(vcpu, mod, rm, code, &p) : ea32(vcpu, mod, rm, code, &p);
    in->gpa = seg + ea;
  }

  if (op == OP_MOV_RM8_IMM8) {
    in->imm = code[p++];
  } else if (op == OP_MOV_RM_IMM) {
    int n = in->size == 2 ? 2 : 4;
    uint64_t imm = 0;
    for (int i = 0; i < n; i++)
      imm |= (uint64_t)code[p + i] << (8 * i);
    p += n;
    in->imm = imm;
  }

  in->len = (uint8_t)p;
  return true;
}

int avm_mmio_fault(struct avm_vcpu *vcpu, uint64_t gpa) {
  struct avm_insn in;
  if (!decode(vcpu, &in))
    return AVM_MMIO_ERROR;

  struct avm_vm *vm = vcpu->vm;
  uint64_t value = 0;
  if (in.movs) {
    /* MOVS stores an element read from RAM.  An MMIO source is not modelled. */
    if (gpa != in.dst || avm_mmio_guest_read(vm, in.src, &value, in.size) != 0)
      return AVM_MMIO_ERROR;
  } else if (in.write) {
    value = (in.store_imm ? in.imm : reg_value(vcpu, in.reg, in.reg8)) & size_mask(in.size);
  }

  bool apic = vm->irqchip && gpa >= APIC_BASE_GPA && gpa < APIC_BASE_GPA + MMIO_WINDOW;
  bool ioapic = vm->irqchip && gpa >= IOAPIC_BASE_GPA && gpa < IOAPIC_BASE_GPA + MMIO_WINDOW;
  /* The APIC and I/O APIC are not reached by string elements. */
  if (in.str && (apic || ioapic))
    return AVM_MMIO_ERROR;
  if (apic || ioapic) {
    uint32_t off = (uint32_t)(gpa & (MMIO_WINDOW - 1));
    avm_irq_lock(vm);
    if (in.write) {
      if (apic && off == 0x310) {
        /* ICR high: the destination.  It is sent when the low dword is written. */
        vcpu->icr_high = (uint32_t)value;
      } else if (apic && off == 0x300) {
        avm_smp_write_icr(vcpu, vcpu->icr_high >> 24, (uint32_t)value);
      } else if (apic) {
        avm_lapic_mmio_write(vcpu, off, (uint32_t)value);
      } else {
        avm_ioapic_mmio_write(vm, off, (uint32_t)value);
      }
    } else {
      uint32_t v = 0;
      if (apic)
        avm_lapic_mmio_read(vcpu, off, &v);
      else
        avm_ioapic_mmio_read(vm, off, &v);
      reg_set(vcpu, in.reg, in.reg8, in.size, v & size_mask(in.size));
    }
    avm_irq_unlock(vm);
    vcpu->regs.rip += in.len;
    avm_step_done(vcpu);
    return AVM_MMIO_HANDLED;
  }

  /* A write to a registered ioeventfd signals it, and the guest resumes. */
  if (!in.str && in.write && avm_ioeventfd_write(vm, false, gpa, in.size, value)) {
    vcpu->regs.rip += in.len;
    avm_step_done(vcpu);
    return AVM_MMIO_HANDLED;
  }

  /* Not a device the kernel models: the VMM answers the access. */
  struct kvm_run *run = vcpu->run;
  run->exit_reason = KVM_EXIT_MMIO;
  run->mmio.phys_addr = gpa;
  run->mmio.len = in.size;
  run->mmio.is_write = in.write;
  memset(run->mmio.data, 0, sizeof(run->mmio.data));
  if (in.write)
    memcpy(run->mmio.data, &value, in.size);

  vcpu->mmio_pending = true;
  avm_step_done(vcpu); /* completes when the VMM answers */
  vcpu->mmio_read = !in.write;
  vcpu->mmio_reg8 = in.reg8;
  vcpu->mmio_size = in.size;
  vcpu->mmio_reg = in.reg;
  vcpu->mmio_insn_len = in.len;
  vcpu->mmio_str = in.str;
  vcpu->mmio_str_rep = in.rep;
  vcpu->mmio_str_si = in.lods || in.movs;
  vcpu->mmio_str_di = in.str && !in.lods;
  vcpu->mmio_str_step = in.step;
  vcpu->mmio_str_mask = in.mask;
  return AVM_MMIO_TO_VMM;
}

void avm_mmio_complete(struct avm_vcpu *vcpu) {
  if (!vcpu->mmio_pending)
    return;
  if (vcpu->mmio_read) {
    uint64_t v = 0;
    memcpy(&v, vcpu->run->mmio.data, vcpu->mmio_size);
    reg_set(vcpu, vcpu->mmio_reg, vcpu->mmio_reg8, vcpu->mmio_size, v);
  }
  vcpu->mmio_pending = false;
  if (!vcpu->mmio_str) {
    vcpu->regs.rip += vcpu->mmio_insn_len;
    return;
  }
  /* One element is done.  The index registers move on, and RCX counts down
   * under REP.  While elements are left RIP stays on the instruction, so it
   * runs again and takes the next element. */
  uint64_t mask = vcpu->mmio_str_mask;
  if (vcpu->mmio_str_si)
    avm_set_addr_reg(&vcpu->regs.rsi, mask, vcpu->regs.rsi + vcpu->mmio_str_step);
  if (vcpu->mmio_str_di)
    avm_set_addr_reg(&vcpu->regs.rdi, mask, vcpu->regs.rdi + vcpu->mmio_str_step);
  if (vcpu->mmio_str_rep) {
    avm_set_addr_reg(&vcpu->regs.rcx, mask, (vcpu->regs.rcx & mask) - 1);
    if ((vcpu->regs.rcx & mask) != 0)
      return;
  }
  vcpu->regs.rip += vcpu->mmio_insn_len;
}
