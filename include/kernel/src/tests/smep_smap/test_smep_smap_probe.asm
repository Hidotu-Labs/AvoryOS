; test_smep_smap_probe.asm — fault-protected probes for SMEP/SMAP self-tests.
;
; Each probe performs one raw supervisor access to a user-mapped page and is
; backed by an exception-table entry, so the resulting #PF lands on a fixup
; label instead of panicking the kernel.

global smap_probe_read
global smap_probe_read_ac
global smep_probe_exec

section .text

; -----------------------------------------------------------------------------
; long smap_probe_read(const void *p)
; Reads 8 bytes from p with AC clear.  With SMAP active this faults (-14);
; without SMAP it returns the value.
; -----------------------------------------------------------------------------
smap_probe_read:
    mov rax, [rdi]
    ret
smap_probe_read_fixup:
    mov rax, -14
    ret

; -----------------------------------------------------------------------------
; long smap_probe_read_ac(const void *p)
; Reads 8 bytes from p with AC set, then restores the caller's RFLAGS.
; Only call this when the CPU supports SMAP (stac is #UD otherwise).
; -----------------------------------------------------------------------------
smap_probe_read_ac:
    pushfq
    stac
smap_probe_read_ac_insn:
    mov rax, [rdi]
    popfq
    ret
smap_probe_read_ac_fixup:
    popfq
    mov rax, -14
    ret

; -----------------------------------------------------------------------------
; long smep_probe_exec(void *p)
; Calls code at p.  Returns 1 if the call executed and returned; with SMEP the
; target fetch faults and the fixup returns 0.
; -----------------------------------------------------------------------------
smep_probe_exec:
    call rdi
    mov rax, 1
    ret
smep_probe_exec_fixup:
    add rsp, 8          ; discard the return address pushed by the faulting call
    xor eax, eax
    ret

; -----------------------------------------------------------------------------
; Exception Fixup Table
;
; smep_probe_exec needs two entries: on an instruction-fetch fault some CPUs
; report RIP as the CALL instruction, others as the fetch *target*.  The
; target address is SMEP_SMAP_TEST_VA from test_smep_smap.c — keep in sync.
; -----------------------------------------------------------------------------
%define SMEP_PROBE_TARGET 0x0000000040000000

section __ex_table progbits alloc noexec nowrite
    dq smap_probe_read,         smap_probe_read_fixup
    dq smap_probe_read_ac_insn, smap_probe_read_ac_fixup
    dq smep_probe_exec,         smep_probe_exec_fixup
    dq SMEP_PROBE_TARGET,       smep_probe_exec_fixup
