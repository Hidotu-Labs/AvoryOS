/* Linux i386 signal frames. Included by sys_signal.c after its native
 * context helpers; these frames must never use the x86-64 stack ABI. */
struct compat_signal_stack {
  uint32_t sp, flags, size;
};
struct compat_sigcontext {
  uint32_t gs, fs, es, ds;
  uint32_t di, si, bp, sp, bx, dx, cx, ax;
  uint32_t trapno, err, ip, cs, flags, sp_at_signal, ss;
  uint32_t fpstate, oldmask, cr2;
};
struct compat_signal_context {
  uint32_t flags, link;
  struct compat_signal_stack stack;
  struct compat_sigcontext sc;
  uint32_t mask[2];
};
struct compat_legacy_frame {
  uint32_t restorer, sig;
  struct compat_sigcontext sc;
  uint8_t unused_fpstate[624];
  uint32_t extramask;
  uint8_t retcode[8];
};
struct compat_rt_frame {
  uint32_t restorer, sig, pinfo, puc;
  uint32_t info[32];
  struct compat_signal_context uc;
  uint8_t retcode[8];
};
_Static_assert(sizeof(struct compat_sigcontext) == 88, "i386 sigcontext");
_Static_assert(sizeof(struct compat_signal_context) == 116, "i386 ucontext");
_Static_assert(offsetof(struct compat_legacy_frame, extramask) == 720,
               "i386 legacy extra signal mask");
_Static_assert(offsetof(struct compat_rt_frame, uc) == 144, "i386 rt ucontext");

static void signal_compat_deliver(struct registers *regs, int sig,
                                  struct thread *t, struct k_sigaction *sa,
                                  const struct kernel_siginfo *queued) {
  bool rt = (sa->sa_flags & SA_SIGINFO) != 0;
  bool alt = on_sig_stack(t, regs->rsp);
  uint64_t top = regs->rsp;
  if ((sa->sa_flags & SA_ONSTACK) && t->ss_size &&
      !(t->ss_flags & SS_DISABLE) && !alt) {
    top = t->ss_sp + t->ss_size;
    alt = true;
  }
  size_t size = rt ? sizeof(struct compat_rt_frame) : sizeof(struct compat_legacy_frame);
  if (top >= ELF32_USER_LIMIT || top < size + 624 + 128 ||
      (uintptr_t)sa->sa_handler >= ELF32_USER_LIMIT)
    signal_frame_terminate(t);
  uint64_t fpaddr = (top - 624) & ~63ULL;
  uint64_t address = ((fpaddr - size + 4) & ~15ULL) - 4;
  if ((alt && !on_sig_stack(t, address)) ||
      !vmm_is_user_addr_range_writable(address, top - address))
    signal_frame_terminate(t);

  /* Reuse native siginfo generation and its lazy-FPU ownership handling,
   * then translate the layouts before touching any user memory. */
  struct sigframe seed = {0};
  seed.regs = *regs;
  fill_signal_context(&seed, sig, t);
  if (queued) seed.info = *queued;
  uint64_t mask = t->has_saved_signal_mask ? t->saved_signal_mask : t->signal_mask;
  uint16_t fs, gs;
  __asm__ volatile("mov %%fs, %0; mov %%gs, %1" : "=rm"(fs), "=rm"(gs));
  struct compat_sigcontext sc = {
    .gs=gs, .fs=fs, .es=0x23, .ds=0x23,
    .di=regs->rdi, .si=regs->rsi, .bp=regs->rbp, .sp=regs->rsp,
    .bx=regs->rbx, .dx=regs->rdx, .cx=regs->rcx, .ax=regs->rax,
    .trapno=regs->int_no, .err=regs->err_code, .ip=regs->rip,
    .cs=0x1b, .flags=regs->rflags, .sp_at_signal=regs->rsp, .ss=0x23,
    .fpstate=fpaddr, .oldmask=mask, .cr2=t->fault_addr
  };
  /* _fpstate_32 has a 112-byte legacy x87 prefix followed by the 512-byte
   * FXSR image. Keep the legacy environment and x87 slots meaningful too. */
  uint8_t fp[624] = {0};
  const uint8_t *fx = (const uint8_t *)seed.ucontext.fpregs_mem;
  memcpy(fp + 112, fx, 512);
  uint32_t env[7] = {0};
  uint16_t cw, sw;
  memcpy(&cw, fx, 2); memcpy(&sw, fx + 2, 2);
  env[0] = cw; env[1] = sw;
  unsigned tos = (sw >> 11) & 7;
  for (unsigned i = 0; i < 8; i++) {
    unsigned tag = 3;
    if (fx[4] & (1u << i)) {
      const uint8_t *st = fx + 32 + ((i - tos) & 7) * 16;
      uint64_t mantissa; uint16_t exp;
      memcpy(&mantissa, st, 8); memcpy(&exp, st + 8, 2);
      exp &= 0x7fff;
      tag = !exp && !mantissa ? 1 :
            (!exp || exp == 0x7fff || !(mantissa >> 63)) ? 2 : 0;
    }
    env[2] |= tag << (i * 2);
    memcpy(fp + 28 + i * 10, fx + 32 + i * 16, 10);
  }
  memcpy(&env[3], fx + 8, 4); memcpy(&env[5], fx + 16, 4);
  memcpy(fp, env, sizeof(env)); memcpy(fp + 108, &sw, 2);

  uint32_t restorer = (sa->sa_flags & SA_RESTORER) ?
      (uintptr_t)sa->sa_restorer : (rt ? ELF32_RT_SIGRETURN : ELF32_SIGRETURN);
  if (!restorer || restorer >= ELF32_USER_LIMIT)
    signal_frame_terminate(t);
  if (copy_to_user((void *)fpaddr, fp, sizeof(fp)))
    signal_frame_terminate(t);
  if (rt) {
    struct compat_rt_frame frame = {0};
    frame.restorer = restorer; frame.sig = sig;
    frame.pinfo = address + offsetof(struct compat_rt_frame, info);
    frame.puc = address + offsetof(struct compat_rt_frame, uc);
    frame.uc.sc = sc;
    frame.uc.stack = (struct compat_signal_stack){t->ss_sp, t->ss_flags, t->ss_size};
    frame.uc.mask[0] = mask; frame.uc.mask[1] = mask >> 32;
    memcpy(frame.info, &seed.info, 12);
    memcpy((uint8_t *)frame.info + 12, (uint8_t *)&seed.info + 16, 112);
    if (sig == SIGSYS) {
      frame.info[3] = seed.info._sigsys.si_call_addr;
      frame.info[4] = seed.info._sigsys.si_syscall;
      frame.info[5] = seed.info._sigsys.si_arch;
    }
    if (copy_to_user((void *)address, &frame, sizeof(frame)))
      signal_frame_terminate(t);
    regs->rdx = frame.pinfo; regs->rcx = frame.puc;
  } else {
    struct compat_legacy_frame frame = {0};
    frame.restorer = restorer; frame.sig = sig; frame.sc = sc;
    frame.extramask = mask >> 32;
    if (copy_to_user((void *)address, &frame, sizeof(frame)))
      signal_frame_terminate(t);
    regs->rdx = 0; regs->rcx = 0;
  }
  t->has_saved_signal_mask = false;
  regs->rip = (uintptr_t)sa->sa_handler; regs->rsp = address;
  regs->rax = sig; regs->rflags &= ~(0x100ULL | 0x400ULL); /* TF, DF */
  t->signal_mask |= sa->sa_mask;
  if (!(sa->sa_flags & SA_NODEFER)) t->signal_mask |= 1ULL << (sig - 1);
  t->signal_mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
  if (sa->sa_flags & SA_RESETHAND)
    t->signal_handlers[sig - 1].sa_handler = (void *)SIG_DFL;
  t->last_report_sig = 0;
}

static bool compat_signal_selector(struct thread *t, uint32_t selector) {
  if (!selector || selector == 0x23) return true;
  return (selector & 7) == 3 && selector >> 3 >= 6 && selector >> 3 <= 8 &&
         t->compat_tls[(selector >> 3) - 6] != 0;
}

void signal_compat_sigreturn(struct registers *regs, bool rt) {
  struct thread *t = sched_get_current();
  struct compat_sigcontext sc;
  uint64_t mask;
  if (!t || regs->cs != 0x1b || regs->rsp < (rt ? 4u : 8u))
    signal_frame_terminate(t);
  uint64_t address = (uint32_t)regs->rsp - (rt ? 4u : 8u);
  if (rt) {
    struct compat_rt_frame frame;
    if (copy_from_user(&frame, (void *)address, sizeof(frame)))
      signal_frame_terminate(t);
    sc = frame.uc.sc;
    mask = frame.uc.mask[0] | ((uint64_t)frame.uc.mask[1] << 32);
    if (frame.uc.stack.flags & ~(SS_DISABLE | SS_ONSTACK | SS_AUTODISARM) ||
        (uint64_t)frame.uc.stack.sp + frame.uc.stack.size > ELF32_USER_LIMIT)
      signal_frame_terminate(t);
    t->ss_sp = frame.uc.stack.sp; t->ss_size = frame.uc.stack.size;
    t->ss_flags = frame.uc.stack.flags & ~SS_ONSTACK;
  } else {
    struct compat_legacy_frame frame;
    if (copy_from_user(&frame, (void *)address, sizeof(frame)))
      signal_frame_terminate(t);
    sc = frame.sc;
    mask = sc.oldmask | ((uint64_t)frame.extramask << 32);
  }
  if (sc.cs != 0x1b || sc.ss != 0x23 || sc.ip >= ELF32_USER_LIMIT ||
      sc.sp >= ELF32_USER_LIMIT || !compat_signal_selector(t, sc.fs) ||
      !compat_signal_selector(t, sc.gs) || (sc.ds && sc.ds != 0x23) ||
      (sc.es && sc.es != 0x23))
    signal_frame_terminate(t);
  uint8_t fx[512] __attribute__((aligned(64)));
  if (sc.fpstate) {
    uint8_t fp[624];
    if (copy_from_user(fp, (void *)(uintptr_t)sc.fpstate, sizeof(fp)))
      signal_frame_terminate(t);
    memcpy(fx, fp + 112, sizeof(fx));
    uint32_t mxcsr, mxcsr_mask;
    memcpy(&mxcsr, fx + 24, 4);
    memcpy(&mxcsr_mask, (uint8_t *)t->fpu_state + 28, 4);
    if (!mxcsr_mask) mxcsr_mask = 0xffbf;
    if (mxcsr & ~mxcsr_mask) signal_frame_terminate(t);
    /* Restore legacy x87 fields, which i386 ucontext users may edit. */
    memcpy(fx, fp, 2); memcpy(fx + 2, fp + 4, 2);
    fx[4] = 0;
    uint32_t tag; memcpy(&tag, fp + 8, 4);
    for (unsigned i = 0; i < 8; i++) {
      if (((tag >> (i * 2)) & 3) != 3) fx[4] |= 1u << i;
      memcpy(fx + 32 + i * 16, fp + 28 + i * 10, 10);
    }
  }
  hal_irq_state_t irq = hal_irq_save();
  if (sc.fpstate) {
    fpu_ensure_loaded(t);
    memcpy(t->fpu_state, fx, sizeof(fx));
    if (cpu_has_xsave_flag) {
      uint64_t mask = __atomic_load_n(&cpu_xsave_mask, __ATOMIC_ACQUIRE);
      /* XRSTOR must restore the handler's saved x87/SSE even if the
       * thread's previous XSAVE header marked them as initial state. */
      uint64_t state; memcpy(&state, (uint8_t *)t->fpu_state + 512, 8);
      state |= 3; memcpy((uint8_t *)t->fpu_state + 512, &state, 8);
      __asm__ volatile("xrstor64 %0" :: "m"(t->fpu_state),
                       "a"((uint32_t)mask), "d"((uint32_t)(mask >> 32)) : "memory");
    } else {
      __asm__ volatile("fxrstor64 %0" :: "m"(fx) : "memory");
    }
  }
  uint64_t kernel_gs = rdmsr(0xC0000101);
  uint16_t fs = sc.fs, gs = sc.gs, ds = sc.ds, es = sc.es;
  __asm__ volatile("mov %0, %%fs; mov %1, %%gs; mov %2, %%ds; mov %3, %%es"
                   :: "rm"(fs), "rm"(gs), "rm"(ds), "rm"(es) : "memory");
  wrmsr(0xC0000101, kernel_gs);
  uint64_t fd = fs >> 3 >= 6 ? t->compat_tls[(fs >> 3) - 6] : 0;
  uint64_t gd = gs >> 3 >= 6 ? t->compat_tls[(gs >> 3) - 6] : 0;
  t->compat_fs = fs; t->compat_gs = gs;
  t->fs_base = ((fd >> 16) & 0xffffff) | ((fd >> 32) & 0xff000000);
  t->gs_base = ((gd >> 16) & 0xffffff) | ((gd >> 32) & 0xff000000);
  wrmsr(0xC0000100, t->fs_base); wrmsr(0xC0000102, t->gs_base);
  *regs = (struct registers){.rdi=sc.di, .rsi=sc.si, .rbp=sc.bp,
    .rbx=sc.bx, .rdx=sc.dx, .rcx=sc.cx, .rax=sc.ax,
    .rip=sc.ip, .rsp=sc.sp, .cs=0x1b, .ss=0x23,
    .rflags=(sc.flags & 0x240dd5U) | 0x202, .int_no=0x80};
  t->signal_mask = mask & ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
  t->fault_regs_valid = false; t->fault_addr = 0;
  hal_irq_restore(irq);
}
