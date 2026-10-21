// Actual admission predicate; hosted tests replace only RFLAGS acquisition.
static bool CompletionCanSleep(Process* process) {
    uint64_t flags;
#ifdef BROOK_GPU_COMPLETION_HOST_TEST
    flags=CompletionHostFlags();
#else
    __asm__ volatile("pushfq; pop %0" : "=r"(flags) :: "memory");
#endif
    if (!(flags & 0x200) || !process || process->terminalPending ||
        process->state == ProcessState::Terminated || process->state == ProcessState::Stopped) return false;
    return !((__atomic_load_n(&process->sigPending,__ATOMIC_ACQUIRE) |
        PosixTimerPending(process)) & ~__atomic_load_n(&process->sigMask,__ATOMIC_ACQUIRE));
}
