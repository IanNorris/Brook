// panic_probe.S — fault-tolerant memory probe for the panic/exception path.
//
// panic_probe_u64 performs the ONE load that is allowed to fault while dumping
// crash state.  Its faulting instruction is registered in the .panic_extable
// section as (fault_rip, fixup_rip).  When it faults, the #PF/#GP handler
// (HandleExceptionFull) looks the RIP up in that table at the very top of the
// handler — before any memory-touching diagnostic work — rewrites the saved RIP
// to the fixup, and IRETQs.  The probe then returns 0 instead of nesting a
// fault.  This is the Linux-style immutable exception-table pattern: stateless,
// re-entrant across any number of probes and both panic paths, and GS-free (the
// lookup runs after the stub's paranoid-swapgs, so kernel GS is already active).
//
// Only the LOAD may fault; the store target (dst) is always trusted static/BSS
// memory supplied by the caller, so it never faults.  RSP is untouched, so the
// fixup's `ret` returns cleanly to the caller.

.text

// int panic_probe_u64(const void* src /* rdi */, uint64_t* dst /* rsi */)
//   Loads 8 bytes from src into *dst.  Returns 1 on success, 0 if src faulted.
.global panic_probe_u64
.type panic_probe_u64, @function
panic_probe_u64:
.Lpanic_probe_u64_load:
    movq (%rdi), %rax        // the ONLY instruction permitted to fault
    movq %rax, (%rsi)        // dst is trusted panic memory — cannot fault
    movl $1, %eax
    ret

// Fixup landing: reached only via the extable when the load above faults.
.global panic_probe_u64_fixup
.type panic_probe_u64_fixup, @function
panic_probe_u64_fixup:
    xorl %eax, %eax          // return false; RSP/return-addr untouched by the fault
    ret

// Immutable exception table: one 16-byte entry {fault_rip, fixup_rip} per
// guarded instruction.  Bounded by __start_panic_extable/__stop_panic_extable
// (see linker.ld).  "a" = allocated, read-only.
.section .panic_extable, "a"
.align 8
    .quad .Lpanic_probe_u64_load
    .quad panic_probe_u64_fixup
.previous
