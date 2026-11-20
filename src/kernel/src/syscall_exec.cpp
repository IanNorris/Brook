// syscall_exec.cpp -- execve.
//
// Split from syscall.cpp.
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "pid_namespace.h"
#include "syscall.h"
#include "busybox_applets.h"
#include "cpu.h"
#include "exec_limits.h"
#include "exec_image.h"
#include "memory/virtual_memory.h"
#include "user_copy.h"
#include "syscall_internal.h"
#include "syscall_open.h"
#include "syscall_handlers.h"

namespace brook {

// ---------------------------------------------------------------------------
// sys_execve (59)
// ---------------------------------------------------------------------------
// Replace the current process image with a new ELF binary.
// rdi = pathname, rsi = argv[], rdx = envp[]
// This function does NOT return on success — it enters the new program.

// Copy a NULL-terminated user vector without truncation. Validate pointer
// slots and each source page before dereferencing; do not scan unchecked user
// strings or require memory after their terminating NUL to be mapped.
static int64_t CopyExecVector(uint64_t vectorAddr, const char** values,
                              int maxCount, char* storage, uint32_t capacity,
                              int& count, uint32_t& used)
{
    count = 0;
    used = 0;
    if (!vectorAddr) return 0;
    for (;;) {
        uint64_t slot;
        uint64_t source;
        if (__builtin_add_overflow(vectorAddr, static_cast<uint64_t>(count) * 8, &slot) ||
            !CopyFromUser(&source, slot, sizeof(source))) return -EFAULT;
        if (!source) return 0;
        if (count == maxCount) return -E2BIG;
        uint32_t start = used;
        bool terminated = false;
        while (!terminated) {
            if (used == capacity) return -E2BIG;
            uint32_t chunk = 4096 - (source & 4095);
            if (chunk > capacity - used) chunk = capacity - used;
            // Copy the chunk (never past this page) into storage, then find
            // the terminator; bytes after it are overwritten by the next value.
            if (!CopyFromUser(storage + used, source, chunk)) return -EFAULT;
            for (uint32_t i = 0; i < chunk; ++i) {
                if (!storage[used++]) { terminated = true; break; }
            }
            source += chunk;
        }
        values[count++] = storage + start;
    }
}

int64_t sys_execve(uint64_t pathAddr, uint64_t argvAddr, uint64_t envpAddr,
                    uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    // Brook's initial-namespace nonleader exec detaches its old thread
    // identity. PID namespaces need Linux's leader identity transfer first.
    if (proc->isThread && ProcessPidView(proc)) return -95; // EOPNOTSUPP

    const char* userPath = reinterpret_cast<const char*>(pathAddr);
    if (!userPath) return -EFAULT;

    // --- Copy filename from user memory into kernel buffer ---
    char kPath[256];
    if (!CopyUserCString(pathAddr, kPath, sizeof(kPath))) return -EFAULT;

    DbgPrintf("sys_execve: pid=%u path='%s'\n", proc->pid, kPath);
    if (!g_hotLogQuiet)
    {
        SerialPrintf("[PROFILE] execve t=%lums pid=%u '%s'\n",
                     ApicTickCount(), proc->pid, kPath);
    }

    // Resolve path: try as-is, then /boot/BIN/<UPPER>, then /boot/<UPPER>
    int shebangDepth = 0;
    static constexpr int MAX_SHEBANG_DEPTH = 4;

    // argv/envp storage and counters. These are declared BEFORE the
    // resolve_path label so that a shebang re-entry (goto resolve_path) does
    // NOT re-run their initializers — the shebang handler rebuilds kArgv/argc
    // in place ([interp, interpArg?, scriptPath, orig argv[1:]]) and that
    // rebuild must survive the goto. Re-reading user argv on re-entry would
    // discard the rebuild and feed the interpreter the original script's args
    // (breaking nested shebang/makeWrapper wrappers).
    static_assert(EXEC_ARG_BYTES + EXEC_ENV_BYTES +
                  (EXEC_MAX_ARGS + EXEC_MAX_ENV) * sizeof(char*) < KERNEL_STACK_SIZE / 2,
                  "exec argument storage must leave room for nested kernel calls");
    const char* kArgv[EXEC_MAX_ARGS];
    const char* kEnvp[EXEC_MAX_ENV];
    char argBuf[EXEC_ARG_BYTES];
    char envBuf[EXEC_ENV_BYTES];
    uint32_t argBufPos = 0;
    uint32_t envBufPos = 0;
    int argc = 0;
    int envc = 0;
    int64_t copyResult = CopyExecVector(argvAddr, kArgv, EXEC_MAX_ARGS,
                                       argBuf, sizeof(argBuf), argc, argBufPos);
    if (copyResult < 0) return copyResult;
    copyResult = CopyExecVector(envpAddr, kEnvp, EXEC_MAX_ENV,
                                envBuf, sizeof(envBuf), envc, envBufPos);
    if (copyResult < 0) return copyResult;

    // Preserve the existing empty-argv convention, using the requested path.
    if (argc == 0) {
        argBufPos = strlen(kPath) + 1;
        memcpy(argBuf, kPath, argBufPos);
        kArgv[argc++] = argBuf;
    }
resolve_path:
    // Procfs exposes executable identity as a virtual path. Resolve self before
    // opening an ELF; reading procfs' path-text entry is not an executable image.
    // Keep argv unchanged, including a caller-provided argv[0]. This path-based
    // lookup does not yet retain an executable inode across unlink/rename.
    if(StrEq(kPath,"/proc/self/exe")) {
        if(!proc->exePath[0])return -ENOENT;
        const uint64_t bytes=strlen(proc->exePath)+1;
        if(bytes>sizeof(kPath))return -36;
        memcpy(kPath,proc->exePath,bytes);
    }
    AtLookup executableLookup;if(int error=executableLookup.Init(-100,kPath))return error;
    char resolvedPath[256];
    char newExePath[sizeof(Process::exePath)];
    Vnode* vn=nullptr;
    const char* lookupPath=kPath;
    int openResult=VfsOpenExecutableAt(executableLookup.fs.root,executableLookup.start(),kPath,
                                       &vn,newExePath,sizeof(newExePath));
    if(openResult && openResult!=-ENOENT)return openResult;
    bool found=vn!=nullptr;
    char aliasName[512];
    bool commandAlias=!strchr(kPath,'/');
    if(!commandAlias) {
        static const char* directories[]={"/bin/","/usr/bin/","/usr/local/bin/","/sbin/","/usr/sbin/","/boot/BIN/","/boot/bin/"};
        for(const char* prefix:directories) {
            size_t length=strlen(prefix);
            if(!strncmp(kPath,prefix,length) && kPath[length] && !strchr(kPath+length,'/'))commandAlias=true;
        }
    }
    const bool allowAliases=commandAlias && !executableLookup.SyntheticName(kPath,aliasName,sizeof(aliasName));

    if (!found && allowAliases)
    {
        // Extract basename for fallback lookups
        const char* baseName = kPath;
        for (const char* p = kPath; *p; ++p)
            if (*p == '/') baseName = p + 1;

        // FAT is case-insensitive, so try the original name first, then uppercased
        const char* prefixes[] = { "/boot/BIN/", "/boot/" };
        for (int pi = 0; pi < 2 && !found; ++pi)
        {
            char tryPath[128];
            uint32_t pLen = 0;
            for (const char* s = prefixes[pi]; *s && pLen < 120; ++s)
                tryPath[pLen++] = *s;
            for (const char* s = baseName; *s && pLen < 126; ++s)
                tryPath[pLen++] = *s;
            tryPath[pLen] = '\0';

            openResult=VfsOpenExecutableAt(executableLookup.fs.root,executableLookup.fs.root,tryPath,
                                           &vn,newExePath,sizeof(newExePath));
            if(openResult && openResult!=-ENOENT)return openResult;
            if(vn)
            {
                for (uint32_t k = 0; k <= pLen; ++k) resolvedPath[k] = tryPath[k];
                lookupPath = resolvedPath;
                found = true;
            }
        }
    }

    // Busybox fallback: if the command is not found, try running busybox
    // with the command name as argv[0]. Busybox reads argv[0] to determine
    // which applet to run.
    //
    // This handles two cases:
    //   1. Simple commands (no '/') — e.g. "ls"
    //   2. Full paths under bin directories — e.g. "/boot/BIN/ls"
    //      (bash resolves these via stat's BusyboxStatFallback, then calls
    //       execve with the full path)
    bool busyboxFallback = false;
    if (!found && allowAliases)
    {
        bool useBusybox = false;
        bool simpleCommand = true;
        for (const char* p = kPath; *p; ++p)
            if (*p == '/') { simpleCommand = false; break; }

        if (simpleCommand)
        {
            useBusybox = true;
        }
        else
        {
            // Check if path is under a bin-like directory (same as BusyboxStatFallback)
            const char* binPrefixes[] = {
                "/boot/BIN/", "/boot/bin/", "/bin/", "/usr/bin/",
                "/usr/local/bin/", "/sbin/", "/usr/sbin/"
            };
            for (int i = 0; i < 7; ++i)
            {
                const char* pfx = binPrefixes[i];
                const char* a = kPath;
                const char* b = pfx;
                while (*b && *a == *b) { a++; b++; }
                if (!*b && *a)
                {
                    bool hasSlash = false;
                    for (const char* c = a; *c; ++c)
                        if (*c == '/') { hasSlash = true; break; }
                    if (!hasSlash) { useBusybox = true; break; }
                }
            }
        }

        const char* applet = kPath;
        for (const char* p = kPath; *p; ++p)
            if (*p == '/') applet = p + 1;
        if (useBusybox && BusyboxHasApplet(applet))
        {
            static const char* busyboxPaths[] = {
                "/boot/BIN/busybox", "/boot/BIN/BUSYBOX",
                "/boot/bin/busybox", "/bin/busybox"
            };
            for (int i = 0; i < 4; i++)
            {
                openResult=VfsOpenExecutableAt(executableLookup.fs.root,executableLookup.fs.root,busyboxPaths[i],
                                               &vn,newExePath,sizeof(newExePath));
                if(openResult && openResult!=-ENOENT)return openResult;
                if(vn)
                {
                    uint32_t k = 0;
                    const char* p = busyboxPaths[i];
                    while (p[k]) { resolvedPath[k] = p[k]; k++; }
                    resolvedPath[k] = '\0';
                    lookupPath = resolvedPath;
                    found = true;
                    busyboxFallback = true;
                    DbgPrintf("sys_execve: busybox fallback for '%s'\n", kPath);
                    break;
                }
            }
        }
    }

    if (!found)
    {
        DbgPrintf("sys_execve: not found: %s\n", kPath);
        SerialPrintf("sys_execve: ENOENT path='%s' pid=%u\n", kPath,
                     ProcessCurrent() ? ProcessCurrent()->pid : 0);
        return -ENOENT;
    }

    SerialPrintf("sys_execve: envpAddr=0x%lx envc=%d argc=%d path='%s'\n",
                 envpAddr, envc, argc, lookupPath);

    // Keep the original lookup spelling for script argv, but record the final
    // ELF's resolved location (including a shebang interpreter's symlinks).
    uint32_t newExeLen = strlen(newExePath);

    ExecutableImage image{};
    int readResult = ReadExecutableImage(vn, &image);
    VfsClose(vn);
    if (readResult != 0) return readResult;
    uint8_t* elfBuf = image.data;
    uint64_t elfSize = image.size;
    DbgPrintf("sys_execve: read '%s' %lu bytes (pid %u)\n",
                 lookupPath, elfSize, proc->pid);

    DbgPrintf("sys_execve: loaded '%s' (%lu bytes) for pid %u\n",
              lookupPath, elfSize, proc->pid);

    // --- Shebang (#!) support ---
    // If the file starts with "#!", extract the interpreter path and re-exec.
    // Uses goto instead of recursive sys_execve to avoid passing kernel-space
    // pointers through user-pointer validation (which returns EFAULT).
    if (elfSize >= 2 && elfBuf[0] == '#' && elfBuf[1] == '!')
    {
        if (++shebangDepth > MAX_SHEBANG_DEPTH) {
            FreeExecutableImage(&image);
            SerialPrintf("sys_execve: shebang depth exceeded for '%s'\n", lookupPath);
            return -ELOOP;
        }

        // Parse interpreter line: "#!<interp> [arg]\n"
        const char* line = reinterpret_cast<const char*>(elfBuf);
        const char* lineEnd = line + (elfSize < 256 ? elfSize : 256);
        const char* p = line + 2;
        const char* terminator = p;
        while (terminator < lineEnd && *terminator != '\n' && *terminator != '\0')
            ++terminator;
        if (terminator == lineEnd && elfSize >= 256) {
            FreeExecutableImage(&image);
            return -ENOEXEC; // Never execute a truncated interpreter line.
        }

        while (p < lineEnd && (*p == ' ' || *p == '\t')) p++;

        // Extract interpreter path into kPath (reuse existing buffer)
        char interpPath[256];
        uint32_t interpLen = 0;
        while (p < lineEnd && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '\0')
        {
            if (interpLen == sizeof(interpPath) - 1) {
                FreeExecutableImage(&image);
                return -E2BIG;
            }
            interpPath[interpLen++] = *p;
            p++;
        }
        interpPath[interpLen] = '\0';

        // Skip whitespace to optional arg
        while (p < lineEnd && (*p == ' ' || *p == '\t')) p++;

        char interpArg[128];
        uint32_t interpArgLen = 0;
        while (p < lineEnd && *p != '\n' && *p != '\r' && *p != '\0')
        {
            if (interpArgLen == sizeof(interpArg) - 1) {
                FreeExecutableImage(&image);
                return -E2BIG;
            }
            interpArg[interpArgLen++] = *p;
            p++;
        }
        interpArg[interpArgLen] = '\0';

        FreeExecutableImage(&image);
        if (interpLen == 0) return -ENOEXEC;

        SerialPrintf("sys_execve: shebang interp='%s' arg='%s' script='%s'\n",
                     interpPath, interpArg, lookupPath);

        // Rebuild argv in-place: [interp, interpArg?, scriptPath, orig_argv[1:]]
        // Save the script path (lookupPath may point into resolvedPath which gets
        // overwritten on the next resolve_path iteration).
        char scriptPath[256];
        uint32_t spi = 0;
        while (lookupPath[spi] && spi < 255) { scriptPath[spi] = lookupPath[spi]; spi++; }
        scriptPath[spi] = '\0';

        // All argument strings are packed in argBuf. Move the old argv[1:]
        // tail once, then adjust its pointers from the end before replacing
        // argv[0]. No shared scratch buffer or silent loss of trailing args.
        uint32_t oldHeadBytes = strlen(kArgv[0]) + 1;
        uint32_t tailBytes = argBufPos - oldHeadBytes;
        int prefixCount = interpArgLen ? 3 : 2;
        uint32_t prefixBytes = interpLen + 1 + spi + 1 +
                               (interpArgLen ? interpArgLen + 1 : 0);
        if (argc - 1 + prefixCount > EXEC_MAX_ARGS ||
            prefixBytes + tailBytes > sizeof(argBuf)) return -E2BIG;
        memmove(argBuf + prefixBytes, argBuf + oldHeadBytes, tailBytes);
        for (int i = argc - 1; i >= 1; --i)
            kArgv[i + prefixCount - 1] = argBuf + prefixBytes +
                                          (kArgv[i] - argBuf - oldHeadBytes);
        uint32_t prefixPos = 0;
        int prefixIndex = 0;
        auto pushPrefix = [&](const char* s, uint32_t bytes) {
            kArgv[prefixIndex++] = argBuf + prefixPos;
            memcpy(argBuf + prefixPos, s, bytes);
            prefixPos += bytes;
        };
        pushPrefix(interpPath, interpLen + 1);
        if (interpArgLen) pushPrefix(interpArg, interpArgLen + 1);
        pushPrefix(scriptPath, spi + 1);
        argc += prefixCount - 1;
        argBufPos = prefixBytes + tailBytes;

        // Set kPath to the interpreter for path resolution
        memcpy(kPath, interpPath, interpLen + 1);

        goto resolve_path;
    }

    if (elfSize < 64) // Too small to be a valid ELF
    {
        FreeExecutableImage(&image);
        DbgPrintf("sys_execve: file too small (%lu bytes)\n", elfSize);
        return -ENOEXEC;
    }

    // Reject malformed/truncated headers while the old image and siblings
    // still exist. Allocation failures after commitment remain fatal.
    if (!ElfValidateMainImage(elfBuf, elfSize)) {
        FreeExecutableImage(&image);
        return -ENOEXEC;
    }

    ExecutableImage interpreterImage{};
    const int interpreterResult=PrepareExecInterpreter(image,&interpreterImage);
    if(interpreterResult) {FreeExecutableImage(&image);return interpreterResult;}
    // exec enters user mode without returning through this C++ frame. Drop
    // the lookup snapshot explicitly before all non-returning commit paths.
    executableLookup.Clear();

    // --- Kill sibling threads before replacing the process image ---
    // Linux: execve terminates all other threads in the thread group
    // before the old address space is destroyed.  Without this, sibling
    // threads may still be executing user code backed by the old page
    // table / file VMAs, causing use-after-free crashes (BRO-091).
    if (proc->tgid && proc->pid == proc->tgid)
    {
        SchedulerKillThreadGroup(proc->tgid, proc, 128 + 9, 9 /* SIGKILL */);
        // SchedulerKillThreadGroup latches tgidExiting=true on the whole group
        // (including this surviving leader) to stillbirth any racing clone.
        // But execve is a RE-IMAGE, not a group exit: this thread lives on as
        // the new program. The kill is synchronous (all siblings are reaped on
        // return) and this is now the only live thread, so clear the latch —
        // otherwise the re-imaged program's own future threads (e.g. an SDL
        // audio thread) are wrongly stillborn and it blocks forever waiting on
        // a thread that never runs. (This silently broke launcher/makeWrapper
        // style programs that exec then spawn threads.)
        if(!__atomic_load_n(&proc->oomEpisode,__ATOMIC_ACQUIRE))
            __atomic_store_n(&proc->tgidExiting, false, __ATOMIC_RELEASE);
    }

    // --- Replace the process image ---
    uint64_t newStackPtr = 0;
    uint64_t newEntry = ProcessExecPreparedExecutable(proc, image, interpreterImage,
                                     argc, kArgv, envc, kEnvp,
                                     &newStackPtr);

    // Both image snapshots remain retained until loading has finished.
    FreeExecutableImage(&interpreterImage);
    FreeExecutableImage(&image);

    if (!newEntry)
    {
        SerialPrintf("sys_execve: ProcessExec failed for pid %u\n", proc->pid);
        // Process is in a broken state — kill it
        SchedulerExitCurrentProcess(-1);
        __builtin_unreachable();
    }

    // --- execve committed: clear the group-exit latch ---
    // SchedulerKillThreadGroup() above latched tgidExiting=true to refuse
    // sibling-thread births while the old thread group was being torn down
    // (BRO-173).  Now that the new image has loaded successfully, the surviving
    // leader is a fresh, single-threaded, very-much-alive process — the latch
    // MUST be cleared.  Otherwise the leader's incarnation still reads
    // tgidExiting=true, and its FIRST pthread_create (e.g. SDL2/3's audio or
    // event thread) is stillborn forever: ProcessCreateThread marks the new
    // thread Terminated instead of running it, glibc's start_thread then runs on
    // a misaligned/parent stack and #GPs inside __vsnprintf_internal (movaps on a
    // 16-byte-unaligned stack).  This is BRO-203 and is why every exec'd threaded
    // app (yquake2, SuperTuxKart, any SDL app) crashed right after window setup.
    if(!__atomic_load_n(&proc->oomEpisode,__ATOMIC_ACQUIRE))
        __atomic_store_n(&proc->tgidExiting, false, __ATOMIC_RELEASE);

    for (uint32_t i = 0; i <= newExeLen; ++i)
        proc->exePath[i] = newExePath[i];

    // Update process name — use argv[0] for busybox applets, binary name otherwise
    const char* nameSource = lookupPath;
    if (busyboxFallback && argc > 0)
        nameSource = kArgv[0];
    const char* baseName2 = nameSource;
    for (const char* p = nameSource; *p; ++p)
        if (*p == '/') baseName2 = p + 1;
    uint32_t ni = 0;
    while (baseName2[ni] && ni < 30)
    {
        proc->name[ni] = baseName2[ni];
        ni++;
    }
    proc->name[ni] = '\0';

    DbgPrintf("sys_execve: entering user mode for '%s' pid=%u entry=0x%lx sp=0x%lx\n",
              proc->name, proc->pid, newEntry, newStackPtr);

    // Validate that entry point and stack are mapped in the new address space.
    {
        PhysicalAddress entryPhys = VmmVirtToPhys(proc->addressSpace->pageTable, VirtualAddress(newEntry));
        PhysicalAddress stackPhys = VmmVirtToPhys(proc->addressSpace->pageTable, VirtualAddress(newStackPtr & ~0xFFFULL));
        if (!entryPhys || !stackPhys) {
            SerialPrintf("sys_execve: FATAL unmapped pages! entry phys=0x%lx stack phys=0x%lx\n",
                         entryPhys.raw(), stackPhys.raw());
            SchedulerExitCurrentProcess(-1);
            __builtin_unreachable();
        }
    }

    // Exec changes group identity and never returns through the normal syscall
    // return boundary. A selected task must finish cleanup instead of entering
    // the new image or clearing its recovery birth fence.
    if(__atomic_load_n(&proc->oomEpisode,__ATOMIC_ACQUIRE)) {
        SchedulerRequestFatalSignal(proc,9);
        SchedulerExitCurrentProcess(128+9,9);
    }

    // --- Switch to new address space and enter user mode ---
    // Load the new page table
    __asm__ volatile("mov %0, %%cr3" : : "r"(proc->addressSpace->pageTable.pml4.raw()) : "memory");

    // Reset user segment state atomically with the live MSRs. Earlier exec
    // work can be preempted, so write GS=0 explicitly rather than trusting a
    // saved context which a switch may have refreshed from the old image.
    uint64_t segmentFlags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(segmentFlags) :: "memory");
    proc->savedCtx.fsBase = proc->fsBase;
    proc->savedCtx.gsBase = 0;
    WriteMsr(0xC0000100U, proc->fsBase);
    WriteMsr(MSR_KERNEL_GS_BASE, 0);
    __asm__ volatile("push %0; popfq" :: "r"(segmentFlags) : "memory", "cc");

    // Wake vfork parent — exec is complete, parent can resume
    ProcessVforkRelease(proc);

    // Enter user mode — this does NOT return
    SwitchToUserMode(newStackPtr, newEntry);
    __builtin_unreachable();
}

} // namespace brook
