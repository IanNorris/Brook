// crash_dump.c — Brook user-mode crash-dump writer.
//
// This runs on a fresh thread injected into the crashed process by the
// kernel fault handler (commit d of the series).  The heap, stdio, TLS,
// and any locks the crashed thread was holding are all suspect — the
// writer must therefore use raw syscalls only.
//
// See files/crash-dump-plan.md for rationale and design notes.

#include "crash_dump.h"

#include <stdint.h>
#include <stddef.h>

// ---- Raw syscall helpers ------------------------------------------------

#define SYS_READ             0
#define SYS_WRITE            1
#define SYS_OPEN             2
#define SYS_CLOSE            3
#define SYS_FSYNC           74
#define SYS_MKDIR           83
#define SYS_CLOCK_GETTIME  228
#define SYS_GETTID         186

#define BROOK_SYS_SET_CRASH_ENTRY  502
#define BROOK_SYS_CRASH_COMPLETE   503

#define O_WRONLY   0x0001
#define O_CREAT    0x0040
#define O_TRUNC    0x0200

static long __syscall3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "0"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}
static long __syscall1(long n, long a) { return __syscall3(n, a, 0, 0); }
static long __syscall2(long n, long a, long b) { return __syscall3(n, a, b, 0); }

// ---- Tiny formatter — no libc ------------------------------------------

static size_t cd_strlen(const char* s) {
    size_t n = 0; while (s[n]) ++n; return n;
}

static char* cd_put(char* p, char* end, const char* s) {
    while (*s && p < end) *p++ = *s++;
    return p;
}

static char* cd_u64(char* p, char* end, uint64_t v, int base, int pad) {
    char buf[32]; int n = 0;
    if (v == 0) buf[n++] = '0';
    while (v) {
        unsigned d = (unsigned)(v % (uint64_t)base);
        buf[n++] = (char)(d < 10 ? ('0' + d) : ('a' + d - 10));
        v /= (uint64_t)base;
    }
    while (n < pad) buf[n++] = '0';
    while (n > 0 && p < end) *p++ = buf[--n];
    return p;
}

static char* cd_hex(char* p, char* end, uint64_t v, int pad) {
    return cd_u64(p, end, v, 16, pad);
}
static char* cd_dec(char* p, char* end, uint64_t v) {
    return cd_u64(p, end, v, 10, 0);
}

// ---- Writer ------------------------------------------------------------

static void cd_writeall(int fd, const char* buf, size_t len) {
    while (len) {
        long w = __syscall3(SYS_WRITE, (long)fd, (long)buf, (long)len);
        if (w <= 0) return;
        buf += (size_t)w;
        len -= (size_t)w;
    }
}

static void cd_write_line(int fd, const char* s) {
    cd_writeall(fd, s, cd_strlen(s));
    cd_writeall(fd, "\n", 1);
}

struct timespec_t { long tv_sec; long tv_nsec; };

static uint64_t cd_now_sec(void) {
    struct timespec_t ts = { 0, 0 };
    __syscall2(SYS_CLOCK_GETTIME, 0 /* CLOCK_REALTIME */, (long)&ts);
    return (uint64_t)ts.tv_sec;
}

static void cd_write_reg_row(int fd, const char* n1, uint64_t v1,
                             const char* n2, uint64_t v2,
                             const char* n3, uint64_t v3) {
    char buf[128];
    char* p = buf;
    char* e = buf + sizeof(buf);
    p = cd_put(p, e, "  "); p = cd_put(p, e, n1); p = cd_put(p, e, "=0x");
    p = cd_hex(p, e, v1, 16);
    p = cd_put(p, e, "  "); p = cd_put(p, e, n2); p = cd_put(p, e, "=0x");
    p = cd_hex(p, e, v2, 16);
    p = cd_put(p, e, "  "); p = cd_put(p, e, n3); p = cd_put(p, e, "=0x");
    p = cd_hex(p, e, v3, 16);
    p = cd_put(p, e, "\n");
    cd_writeall(fd, buf, (size_t)(p - buf));
}

// Safe probe: read one byte, return -1 if it looks non-canonical.
// We can't signal-catch from here; just bail on obviously-bogus addrs.
static int cd_addr_canonical(uint64_t a) {
    uint64_t hi = a >> 47;
    return (hi == 0) || (hi == 0x1FFFFu);
}

static void cd_dump_bytes(int fd, const char* label, uint64_t addr,
                          int before, int after) {
    char buf[256]; char* p = buf; char* e = buf + sizeof(buf);
    p = cd_put(p, e, label); p = cd_put(p, e, " @ 0x");
    p = cd_hex(p, e, addr, 16); p = cd_put(p, e, ":\n");
    cd_writeall(fd, buf, (size_t)(p - buf));

    if (!cd_addr_canonical(addr)) {
        cd_write_line(fd, "  <non-canonical address>");
        return;
    }

    const uint8_t* start = (const uint8_t*)(addr - (uint64_t)before);
    int total = before + after;
    p = buf; e = buf + sizeof(buf);
    p = cd_put(p, e, "  ");
    for (int i = 0; i < total; ++i) {
        if (i > 0 && (i % 16) == 0) {
            p = cd_put(p, e, "\n  ");
        }
        if (i == before) p = cd_put(p, e, ">> ");
        uint8_t b = start[i];
        p = cd_hex(p, e, b, 2);
        p = cd_put(p, e, " ");
        if (p > e - 8) {
            cd_writeall(fd, buf, (size_t)(p - buf));
            p = buf;
        }
    }
    p = cd_put(p, e, "\n");
    cd_writeall(fd, buf, (size_t)(p - buf));
}

static void cd_dump_stack_trace(int fd, uint64_t rbp, uint64_t rip) {
    cd_write_line(fd, "Stack trace (user, rbp walk):");
    char buf[96]; char* p; char* e;

    // Frame 0 = faulting RIP
    p = buf; e = buf + sizeof(buf);
    p = cd_put(p, e, "  #0  0x"); p = cd_hex(p, e, rip, 16); p = cd_put(p, e, "\n");
    cd_writeall(fd, buf, (size_t)(p - buf));

    uint64_t cur = rbp;
    for (int i = 1; i < 32; ++i) {
        if (!cd_addr_canonical(cur) || cur < 0x1000 || (cur & 7)) break;
        uint64_t next_rbp = *(const uint64_t*)cur;
        uint64_t ret_rip  = *(const uint64_t*)(cur + 8);
        if (!cd_addr_canonical(ret_rip) || ret_rip < 0x1000) break;
        p = buf; e = buf + sizeof(buf);
        p = cd_put(p, e, "  #"); p = cd_dec(p, e, (uint64_t)i);
        p = cd_put(p, e, "  0x"); p = cd_hex(p, e, ret_rip, 16);
        p = cd_put(p, e, "\n");
        cd_writeall(fd, buf, (size_t)(p - buf));
        if (next_rbp <= cur) break; // not growing down → corrupt
        cur = next_rbp;
    }
}

static const char* cd_sig_name(uint32_t s) {
    switch (s) {
        case 4:  return "SIGILL";
        case 7:  return "SIGBUS";
        case 8:  return "SIGFPE";
        case 11: return "SIGSEGV";
        default: return "SIG?";
    }
}
static const char* cd_vec_name(uint32_t v) {
    switch (v) {
        case 0:  return "#DE Divide-by-zero";
        case 6:  return "#UD Invalid Opcode";
        case 13: return "#GP General Protection";
        case 14: return "#PF Page Fault";
        case 16: return "#MF x87 FP";
        case 17: return "#AC Alignment Check";
        case 19: return "#XM SIMD FP";
        default: return "?";
    }
}

static int cd_copy_comm(char* out, int outCap, const char* src) {
    int n = 0;
    while (n < outCap - 1 && src[n] && n < 31) { out[n] = src[n]; ++n; }
    out[n] = 0;
    if (n == 0) { out[0] = '?'; out[1] = 0; n = 1; }
    return n;
}

void __brook_crash_entry(const BrookCrashCtx* ctx) {
    // Make the target directory (ignore EEXIST).
    __syscall2(SYS_MKDIR, (long)"/data", 0755);
    __syscall2(SYS_MKDIR, (long)"/data/crashes", 0755);

    // Build filename: /data/crashes/<ts>_<tid>_<comm>.txt
    char comm[32];
    if (ctx && ctx->magic == BROOK_CRASH_CTX_MAGIC) {
        cd_copy_comm(comm, sizeof(comm), ctx->commName);
    } else {
        comm[0] = '?'; comm[1] = 0;
    }

    uint64_t tid = ctx ? ctx->tid : 0;
    uint64_t ts  = cd_now_sec();

    char path[160];
    char* p = path; char* e = path + sizeof(path);
    p = cd_put(p, e, "/data/crashes/");
    p = cd_dec(p, e, ts); p = cd_put(p, e, "_");
    p = cd_dec(p, e, tid); p = cd_put(p, e, "_");
    p = cd_put(p, e, comm); p = cd_put(p, e, ".txt");
    if (p < e) *p = 0; else path[sizeof(path)-1] = 0;

    int fd = (int)__syscall3(SYS_OPEN, (long)path,
                             O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        // No file — just complete.  The kernel will kill the process.
        __syscall1(BROOK_SYS_CRASH_COMPLETE,
                   (long)(128 + (ctx ? (int)ctx->signum : 11)));
        return;
    }

    if (!ctx || ctx->magic != BROOK_CRASH_CTX_MAGIC) {
        cd_write_line(fd, "Brook User Crash Dump v1 — INVALID CONTEXT");
        __syscall1(SYS_FSYNC, (long)fd);
        __syscall1(SYS_CLOSE, (long)fd);
        __syscall1(BROOK_SYS_CRASH_COMPLETE, 128 + 11);
        return;
    }

    char line[256];
    char* lp; char* le;

    cd_write_line(fd, "Brook User Crash Dump v1");
    cd_write_line(fd, "========================");

    lp = line; le = line + sizeof(line);
    lp = cd_put(lp, le, "Time:       "); lp = cd_dec(lp, le, ts);
    lp = cd_put(lp, le, " (unix epoch seconds)\n");
    cd_writeall(fd, line, (size_t)(lp - line));

    lp = line; le = line + sizeof(line);
    lp = cd_put(lp, le, "Process:    "); lp = cd_put(lp, le, comm);
    lp = cd_put(lp, le, " (tid=");
    lp = cd_dec(lp, le, tid); lp = cd_put(lp, le, ")\n");
    cd_writeall(fd, line, (size_t)(lp - line));

    lp = line; le = line + sizeof(line);
    lp = cd_put(lp, le, "Signal:     ");
    lp = cd_dec(lp, le, ctx->signum); lp = cd_put(lp, le, " (");
    lp = cd_put(lp, le, cd_sig_name(ctx->signum)); lp = cd_put(lp, le, ")\n");
    cd_writeall(fd, line, (size_t)(lp - line));

    lp = line; le = line + sizeof(line);
    lp = cd_put(lp, le, "Vector:     ");
    lp = cd_dec(lp, le, ctx->vector); lp = cd_put(lp, le, " (");
    lp = cd_put(lp, le, cd_vec_name(ctx->vector)); lp = cd_put(lp, le, ")\n");
    cd_writeall(fd, line, (size_t)(lp - line));

    lp = line; le = line + sizeof(line);
    lp = cd_put(lp, le, "Error code: 0x"); lp = cd_hex(lp, le, ctx->errorCode, 0);
    lp = cd_put(lp, le, "\n");
    cd_writeall(fd, line, (size_t)(lp - line));

    lp = line; le = line + sizeof(line);
    lp = cd_put(lp, le, "Fault addr: 0x"); lp = cd_hex(lp, le, ctx->faultAddr, 16);
    lp = cd_put(lp, le, "\n\n");
    cd_writeall(fd, line, (size_t)(lp - line));

    cd_write_line(fd, "Registers:");
    cd_write_reg_row(fd, "RIP", ctx->rip, "RSP", ctx->rsp, "RBP", ctx->rbp);
    cd_write_reg_row(fd, "RAX", ctx->rax, "RBX", ctx->rbx, "RCX", ctx->rcx);
    cd_write_reg_row(fd, "RDX", ctx->rdx, "RSI", ctx->rsi, "RDI", ctx->rdi);
    cd_write_reg_row(fd, "R8 ", ctx->r8,  "R9 ", ctx->r9,  "R10", ctx->r10);
    cd_write_reg_row(fd, "R11", ctx->r11, "R12", ctx->r12, "R13", ctx->r13);
    cd_write_reg_row(fd, "R14", ctx->r14, "R15", ctx->r15, "FLG", ctx->rflags);
    cd_write_reg_row(fd, "CS ", ctx->cs,  "SS ", ctx->ss,  "FSB", ctx->fsBase);
    cd_writeall(fd, "\n", 1);

    cd_dump_stack_trace(fd, ctx->rbp, ctx->rip);
    cd_writeall(fd, "\n", 1);

    cd_dump_bytes(fd, "Code", ctx->rip, 16, 48);
    cd_writeall(fd, "\n", 1);
    cd_dump_bytes(fd, "Stack", ctx->rsp, 0, 256);

    // NOTE: no fsync — Brook kernel doesn't implement syscall 74 yet.
    // Close should still flush page cache to disk via the VFS sync path.
    __syscall1(SYS_CLOSE, (long)fd);

    __syscall1(BROOK_SYS_CRASH_COMPLETE,
               (long)(128 + (int)ctx->signum));
}

// ---- Constructor: register crash entry before main runs ----------------
//
// Attribute constructor runs from crt_init before main().  If the syscall
// fails (e.g., already registered, or kernel lacks the feature) we silently
// ignore — the process still runs, just without crash-dump support.

__attribute__((constructor))
static void __brook_crash_init(void) {
    __syscall1(BROOK_SYS_SET_CRASH_ENTRY, (long)(void*)__brook_crash_entry);
}
