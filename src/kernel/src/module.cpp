#include "module.h"
#include "elf.h"
#include "ksymtab.h"
#include "memory/virtual_memory.h"
#include "memory/heap.h"
#include "vfs.h"
#include "serial.h"
#include "kprintf.h"
#include "mem_tag.h"
#include "spinlock.h"
#ifdef BROOK_HOST_TEST
#include <cstring>
#else
#include "string.h"
#endif

namespace brook {

// ---- Module registry ----

static constexpr uint32_t MODULE_MAX = 64;
static ModuleHandle g_modules[MODULE_MAX];
static IrqSpinLock g_moduleLock;
#ifndef BROOK_HOST_TEST
extern "C" uint8_t __text_start[], __kernel_end[];
#endif

static bool ModuleAddressIsKernel(uintptr_t address)
{
#ifdef BROOK_HOST_TEST
    (void)address;
    return false; // Host fixtures supply explicit module mappings.
#else
    return address >= reinterpret_cast<uintptr_t>(__text_start) &&
           address < reinterpret_cast<uintptr_t>(__kernel_end);
#endif
}

bool ModuleRetainAddresses(const uintptr_t* addresses, uint32_t count)
{
    if (!addresses || !count) return false;
    const uint64_t flags = IrqSpinLockAcquire(&g_moduleLock);
    uint64_t owners = 0;
    for (uint32_t a = 0; a < count; ++a) {
        bool found = false;
        for (uint32_t i = 0; i < MODULE_MAX; ++i) {
            const ModuleHandle& m = g_modules[i];
            if (!m.pageCount || addresses[a] < m.baseVirt ||
                (addresses[a] - m.baseVirt) / 4096 >= m.pageCount) continue;
            if (m.unloading || (!m.active && !m.loading)) {
                IrqSpinLockRelease(&g_moduleLock, flags);
                return false;
            }
            owners |= uint64_t(1) << i;
            found = true;
            break;
        }
        if (!found && !ModuleAddressIsKernel(addresses[a])) {
            IrqSpinLockRelease(&g_moduleLock, flags);
            return false;
        }
    }
    for (uint32_t i = 0; i < MODULE_MAX; ++i)
        if (owners & (uint64_t(1) << i)) g_modules[i].resident = true;
    IrqSpinLockRelease(&g_moduleLock, flags);
    return true;
}

// Reserve before blocking I/O. Abandoned reservations have not published any
// callbacks; once a mapping is published, init completion owns its teardown.
struct ModuleReservation {
    ModuleHandle* slot = nullptr;
    ModuleReservation() {
        const uint64_t flags = IrqSpinLockAcquire(&g_moduleLock);
        for (auto& m : g_modules) {
            if (m.active || m.loading || m.unloading) continue;
            m = {};
            m.loading = true;
            slot = &m;
            break;
        }
        IrqSpinLockRelease(&g_moduleLock, flags);
    }
    ~ModuleReservation() {
        if (!slot) return;
        const uint64_t flags = IrqSpinLockAcquire(&g_moduleLock);
        *slot = {};
        IrqSpinLockRelease(&g_moduleLock, flags);
    }
};


// ---- Simple string helpers (avoid depending on string.h here) ----

static uint32_t ModStrLen(const char* s)
{
    uint32_t n = 0;
    while (s[n]) ++n;
    return n;
}

static char ModToLower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

static bool ModStrEqI(const char* a, const char* b)
{
    while (*a && *b) { if (ModToLower(*a++) != ModToLower(*b++)) return false; }
    return *a == *b;
}

static bool ModStrEq(const char* a, const char* b)
{
    while (*a && *b) { if (*a++ != *b++) return false; }
    return *a == *b;
}

static bool ModStrEndsWith(const char* str, const char* suffix)
{
    uint32_t slen = ModStrLen(str);
    uint32_t xlen = ModStrLen(suffix);
    if (xlen > slen) return false;
    return ModStrEqI(str + slen - xlen, suffix);
}

// Zero a byte range using a volatile pointer so the compiler can't replace
// it with a memset call (which isn't available in our freestanding environment).
static void ModZero(void* dst, uint64_t len)
{
    memset(dst, 0, len);
}

// Copy bytes.
static void ModCopy(void* dst, const void* src, uint64_t len)
{
    memcpy(dst, src, len);
}

// ---- ELF loader internals ----

struct LoadedSection {
    uint64_t base;   // virtual address of loaded section (0 = not loaded)
};

// The ELF header bounds count to 16 bits. Allocate only the validated table's
// metadata, avoiding a compiler-dependent fixed section count and kernel stack use.
struct SectionMappings {
    LoadedSection* data;
    explicit SectionMappings(uint32_t count)
        : data(static_cast<LoadedSection*>(kmalloc(uint64_t(count) * sizeof(LoadedSection)))) {}
    ~SectionMappings() { kfree(data); }
    SectionMappings(const SectionMappings&) = delete;
    SectionMappings& operator=(const SectionMappings&) = delete;
};

static bool ValidateElf(const uint8_t* data, uint64_t size)
{
    if (size < sizeof(Elf64_Ehdr)) return false;
    auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(data);
    if (ehdr->e_ident[0] != ELFMAG0 ||
        ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 ||
        ehdr->e_ident[3] != ELFMAG3)
    {
        SerialPuts("module: bad ELF magic\n");
        return false;
    }
    if (ehdr->e_ident[4] != ELFCLASS64)
    {
        SerialPuts("module: not ELF64\n");
        return false;
    }
    if (ehdr->e_ident[5] != ELFDATA2LSB)
    {
        SerialPuts("module: not little-endian\n");
        return false;
    }
    if (ehdr->e_type != ET_REL)
    {
        SerialPrintf("module: not a relocatable object (type=%u)\n", ehdr->e_type);
        return false;
    }
    if (ehdr->e_machine != EM_X86_64)
    {
        SerialPuts("module: not x86-64\n");
        return false;
    }
    // Extended section numbering is unsupported. Validate with subtraction
    // before allocating or forming pointers, including a wrapping e_shoff.
    if (!ehdr->e_shnum || ehdr->e_shentsize != sizeof(Elf64_Shdr) ||
        ehdr->e_shoff > size ||
        ehdr->e_shnum > (size - ehdr->e_shoff) / sizeof(Elf64_Shdr)) {
        SerialPuts("module: invalid section header table\n");
        return false;
    }
    return true;
}

// Resolve a symbol: first check the local symbol table, then the kernel.
// localBase[i] = virtual address of section i (0 if not loaded).
static uint64_t ResolveSymbol(const Elf64_Sym* sym, const uint8_t* data,
                               const Elf64_Shdr* shdrs, uint32_t shnum,
                               const char* strtab, const LoadedSection* secs)
{
    if (sym->st_shndx == SHN_ABS)
        return sym->st_value;

    if (sym->st_shndx != SHN_UNDEF)
    {
        // Defined locally — value is offset within its section.
        uint16_t secIdx = sym->st_shndx;
        if (secIdx >= shnum) return 0;
        return secs[secIdx].base + sym->st_value;
    }

    // Undefined — look up in kernel symbol table.
    const char* name = strtab + sym->st_name;
    const void* addr = KsymLookup(name);
    if (!addr)
    {
        SerialPrintf("module: unresolved symbol '%s'\n", name);
        return 0;
    }
    return reinterpret_cast<uint64_t>(addr);
}

// Apply relocations from a SHT_RELA section.
static bool ApplyRela(const Elf64_Shdr* relaSec, const uint8_t* data,
                      uint64_t dataSize,
                      const Elf64_Shdr* shdrs, uint32_t shnum,
                      const Elf64_Sym* symtab, uint32_t symCount,
                      const char* strtab, const LoadedSection* secs)
{
    // sh_info = index of the section being relocated.
    uint32_t targetSecIdx = relaSec->sh_info;
    if (targetSecIdx >= shnum) return true; // nothing to do
    uint64_t targetBase = secs[targetSecIdx].base;
    if (!targetBase) return true; // section not loaded (ok to skip)

    // Validate relocation data fits within buffer.
    if (relaSec->sh_offset >= dataSize ||
        relaSec->sh_size > dataSize - relaSec->sh_offset)
    {
        SerialPuts("module: rela section data out of bounds\n");
        return false;
    }

    auto* relas = reinterpret_cast<const Elf64_Rela*>(data + relaSec->sh_offset);
    uint32_t count = static_cast<uint32_t>(relaSec->sh_size / sizeof(Elf64_Rela));

    for (uint32_t i = 0; i < count; ++i)
    {
        const Elf64_Rela& rela = relas[i];
        uint32_t symIdx  = ELF64_R_SYM(rela.r_info);
        uint32_t relType = ELF64_R_TYPE(rela.r_info);

        if (symIdx >= symCount)
        {
            SerialPrintf("module: rela %u: symIdx %u out of range\n", i, symIdx);
            return false;
        }

        uint64_t S = ResolveSymbol(&symtab[symIdx], data, shdrs, shnum, strtab, secs);
        if (!S && symtab[symIdx].st_shndx == SHN_UNDEF) return false;

        int64_t  A = rela.r_addend;
        uint64_t P = targetBase + rela.r_offset; // address of the location to patch

        void* patchLoc = reinterpret_cast<void*>(P);

        switch (relType)
        {
        case R_X86_64_NONE:
            break;

        case R_X86_64_64:
        {
            // S + A (64-bit absolute)
            uint64_t val = S + static_cast<uint64_t>(A);
            ModCopy(patchLoc, &val, 8);
            break;
        }

        case R_X86_64_PC32:
        case R_X86_64_PLT32:
        {
            // S + A - P (32-bit PC-relative)
            int64_t val = static_cast<int64_t>(S) + A - static_cast<int64_t>(P);
            if (val > 0x7FFFFFFF || val < -0x80000000LL)
            {
                SerialPrintf("module: R_X86_64_PC32 overflow at 0x%lx "
                             "(val=0x%lx)\n", P, val);
                return false;
            }
            uint32_t val32 = static_cast<uint32_t>(static_cast<int32_t>(val));
            ModCopy(patchLoc, &val32, 4);
            break;
        }

        case R_X86_64_32:
        {
            // S + A (32-bit zero-extended)
            uint64_t val = S + static_cast<uint64_t>(A);
            if (val > 0xFFFFFFFFULL)
            {
                SerialPrintf("module: R_X86_64_32 overflow at 0x%lx\n", P);
                return false;
            }
            uint32_t val32 = static_cast<uint32_t>(val);
            ModCopy(patchLoc, &val32, 4);
            break;
        }

        case R_X86_64_32S:
        {
            // S + A (32-bit sign-extended)
            int64_t val = static_cast<int64_t>(S) + A;
            if (val > 0x7FFFFFFFLL || val < -0x80000000LL)
            {
                SerialPrintf("module: R_X86_64_32S overflow at 0x%lx\n", P);
                return false;
            }
            uint32_t val32 = static_cast<uint32_t>(static_cast<int32_t>(val));
            ModCopy(patchLoc, &val32, 4);
            break;
        }

        case R_X86_64_PC64:
        {
            // S + A - P (64-bit PC-relative)
            int64_t val = static_cast<int64_t>(S) + A - static_cast<int64_t>(P);
            ModCopy(patchLoc, &val, 8);
            break;
        }

        default:
            SerialPrintf("module: unsupported relocation type %u\n", relType);
            return false;
        }
    }
    return true;
}

// ---- ParseAndLoad: core ELF loader ----

struct ParseResult {
    uint64_t          baseVirt;
    uint64_t          pageCount;
    const ModuleInfo* info;
};

static bool ParseAndLoad(const uint8_t* data, uint64_t size, ParseResult& out)
{
    if (!ValidateElf(data, size)) return false;

    auto* ehdr  = reinterpret_cast<const Elf64_Ehdr*>(data);
    auto* shdrs = reinterpret_cast<const Elf64_Shdr*>(data + ehdr->e_shoff);
    uint32_t shnum = ehdr->e_shnum;

    // Section name string table.
    const char* shstrtab = nullptr;
    if (ehdr->e_shstrndx < shnum &&
        shdrs[ehdr->e_shstrndx].sh_offset < size)
        shstrtab = reinterpret_cast<const char*>(
            data + shdrs[ehdr->e_shstrndx].sh_offset);

    // ---- Pass 1: calculate total memory needed ----
    // Allocate a contiguous virtual range covering all SHF_ALLOC sections.

    uint64_t totalSize = 0;
    for (uint32_t i = 0; i < shnum; ++i)
    {
        const Elf64_Shdr& s = shdrs[i];
        if (!(s.sh_flags & SHF_ALLOC)) continue;
        // Validate section data fits within the ELF buffer.
        if (s.sh_type != SHT_NOBITS &&
            (s.sh_offset >= size || s.sh_size > size - s.sh_offset))
        {
            SerialPrintf("module: section %u data out of bounds\n", i);
            return false;
        }
        uint64_t align = s.sh_addralign > 1 ? s.sh_addralign : 1;
        totalSize = (totalSize + align - 1) & ~(align - 1);
        totalSize += s.sh_size;
        if (totalSize > 256 * 1024 * 1024ULL) // sanity cap: 256 MiB
        {
            SerialPuts("module: total section size exceeds 256 MiB limit\n");
            return false;
        }
    }
    if (totalSize == 0)
    {
        SerialPuts("module: no allocatable sections\n");
        return false;
    }

    SectionMappings mappings(shnum);
    if (!mappings.data) { SerialPuts("module: section metadata allocation failed\n"); return false; }
    LoadedSection* secs = mappings.data;
    for (uint32_t i = 0; i < shnum; ++i) secs[i].base = 0;

    uint64_t pageCount = (totalSize + 4095) / 4096;
    // VMM_WRITABLE | VMM_EXEC (exec flag = 0 currently means executable since
    // the kernel's VMM doesn't set NX by default)
    uint64_t baseVirt = VmmAllocModulePages(pageCount, VMM_WRITABLE, MemTag::Device, KernelPid).raw();
    if (!baseVirt)
    {
        SerialPuts("module: VMM allocation failed\n");
        return false;
    }
    ModZero(reinterpret_cast<void*>(baseVirt), pageCount * 4096);

    // ---- Pass 2: place sections ----
    uint64_t cursor = baseVirt;

    for (uint32_t i = 0; i < shnum; ++i)
    {
        const Elf64_Shdr& s = shdrs[i];
        if (!(s.sh_flags & SHF_ALLOC)) continue;

        uint64_t align = s.sh_addralign > 1 ? s.sh_addralign : 1;
        cursor = (cursor + align - 1) & ~(align - 1);
        secs[i].base = cursor;

        if (s.sh_type == SHT_NOBITS)
        {
            // .bss — already zeroed by ModZero above.
        }
        else
        {
            ModCopy(reinterpret_cast<void*>(cursor), data + s.sh_offset, s.sh_size);
        }
        cursor += s.sh_size;
    }

    // ---- Find symbol table and string table ----
    const Elf64_Sym* symtab  = nullptr;
    uint32_t         symCount = 0;
    const char*      strtab  = nullptr;

    for (uint32_t i = 0; i < shnum; ++i)
    {
        if (shdrs[i].sh_type == SHT_SYMTAB)
        {
            // Validate symtab data fits within buffer.
            if (shdrs[i].sh_offset >= size ||
                shdrs[i].sh_size > size - shdrs[i].sh_offset)
            {
                SerialPuts("module: symtab section out of bounds\n");
                VmmFreePages(VirtualAddress(baseVirt), pageCount);
                return false;
            }
            symtab   = reinterpret_cast<const Elf64_Sym*>(data + shdrs[i].sh_offset);
            symCount = static_cast<uint32_t>(shdrs[i].sh_size / sizeof(Elf64_Sym));
            // sh_link = index of associated string table.
            uint32_t strIdx = shdrs[i].sh_link;
            if (strIdx < shnum &&
                shdrs[strIdx].sh_offset < size)
                strtab = reinterpret_cast<const char*>(data + shdrs[strIdx].sh_offset);
            break;
        }
    }

    if (!symtab || !strtab)
    {
        SerialPuts("module: no symbol table\n");
        VmmFreePages(VirtualAddress(baseVirt), pageCount);
        return false;
    }

    // ---- Pass 3: apply relocations ----
    for (uint32_t i = 0; i < shnum; ++i)
    {
        if (shdrs[i].sh_type != SHT_RELA) continue;
        if (!ApplyRela(&shdrs[i], data, size, shdrs, shnum, symtab, symCount, strtab, secs))
        {
            VmmFreePages(VirtualAddress(baseVirt), pageCount);
            return false;
        }
    }

    // ---- Find .modinfo section ----
    const ModuleInfo* modInfo = nullptr;
    for (uint32_t i = 0; i < shnum; ++i)
    {
        if (!shstrtab) break;
        const char* name = shstrtab + shdrs[i].sh_name;
        // Match ".modinfo"
        if (name[0] == '.' && name[1] == 'm' && name[2] == 'o' &&
            name[3] == 'd' && name[4] == 'i' && name[5] == 'n' &&
            name[6] == 'f' && name[7] == 'o' && name[8] == '\0')
        {
            if (secs[i].base)
                modInfo = reinterpret_cast<const ModuleInfo*>(secs[i].base);
            break;
        }
    }

    if (!modInfo)
    {
        SerialPuts("module: no .modinfo section — missing DECLARE_MODULE?\n");
        VmmFreePages(VirtualAddress(baseVirt), pageCount);
        return false;
    }

    if (modInfo->magic != MODULE_MAGIC)
    {
        SerialPrintf("module: bad magic 0x%x\n", modInfo->magic);
        VmmFreePages(VirtualAddress(baseVirt), pageCount);
        return false;
    }

    if (modInfo->abiVersion != MODULE_ABI_VERSION)
    {
        SerialPrintf("module: ABI version mismatch (module=%u kernel=%u)\n",
                     modInfo->abiVersion, MODULE_ABI_VERSION);
        VmmFreePages(VirtualAddress(baseVirt), pageCount);
        return false;
    }

    out.baseVirt  = baseVirt;
    out.pageCount = pageCount;
    out.info      = modInfo;
    return true;
}

// Finish under the same lock used by retention and unload. Failed init may
// already have exported callbacks, so its pages then remain resident even
// though the original load still reports failure.
static bool ModuleCompleteInit(ModuleHandle* slot, int rc)
{
    uint64_t flags = IrqSpinLockAcquire(&g_moduleLock);
    slot->loading = false;
    slot->initFailed = rc != 0;
    const bool retain = rc == 0 || slot->resident;
    slot->active = retain;
    slot->unloading = !retain;
    const ModuleInfo* info = slot->info;
    const uint64_t baseVirt = slot->baseVirt, pages = slot->pageCount;
    IrqSpinLockRelease(&g_moduleLock, flags);
    if (rc == 0) return true;
    SerialPrintf("module: '%s' init() returned %d%s\n", info->name, rc,
                 retain ? " — retaining registered callbacks" : " — unloading");
    if (!retain) {
        VmmFreePages(VirtualAddress(baseVirt), pages);
        flags = IrqSpinLockAcquire(&g_moduleLock);
        *slot = {};
        IrqSpinLockRelease(&g_moduleLock, flags);
    }
    return false;
}

// ---- Public API ----

ModuleHandle* ModuleLoad(const char* path)
{
    ModuleReservation reservation;
    ModuleHandle* slot = reservation.slot;
    if (!slot) {
        SerialPuts("module: registry full\n");
        return nullptr;
    }

    // Read file via VFS.
    Vnode* vn = VfsOpen(path);
    if (!vn)
    {
        SerialPrintf("module: cannot open '%s'\n", path);
        return nullptr;
    }

    // Read the whole file — probe size first by reading into a small buffer,
    // then read in a single pass once we know the size.
    // VfsRead returns actual bytes; we size up by reading until short read.
    static constexpr uint32_t READ_CHUNK = 65536; // 64KB chunks
    uint64_t capacity = READ_CHUNK;
    uint8_t* buf = static_cast<uint8_t*>(kmalloc(capacity));
    if (!buf) { VfsClose(vn); return nullptr; }

    uint64_t off   = 0;
    uint64_t total = 0;
    for (;;)
    {
        if (total + READ_CHUNK > capacity)
        {
            // Grow buffer.
            uint64_t newCap = capacity * 2;
            uint8_t* newBuf = static_cast<uint8_t*>(kmalloc(newCap));
            if (!newBuf)
            {
                kfree(buf);
                VfsClose(vn);
                SerialPuts("module: out of memory reading file\n");
                return nullptr;
            }
            ModCopy(newBuf, buf, total);
            kfree(buf);
            buf      = newBuf;
            capacity = newCap;
        }
        int n = VfsRead(vn, buf + total, READ_CHUNK, &off);
        if (n <= 0) break;
        total += static_cast<uint64_t>(n);
    }
    VfsClose(vn);

    if (total == 0)
    {
        SerialPrintf("module: '%s' is empty\n", path);
        kfree(buf);
        return nullptr;
    }

    SerialPrintf("module: loading '%s' (%lu bytes)\n", path, total);

    ParseResult pr;
    if (!ParseAndLoad(buf, total, pr))
    {
        kfree(buf);
        return nullptr;
    }
    kfree(buf);

    // Publish mapping identity BEFORE init can register permanent callbacks.
    // The short registry hold never covers VFS, module code or page teardown.
    uint64_t flags = IrqSpinLockAcquire(&g_moduleLock);
    for (auto& m : g_modules) {
        if (&m == slot || !m.info || (!m.active && !m.loading && !m.unloading) ||
            !ModStrEq(m.info->name, pr.info->name)) continue;
        ModuleHandle* existing = m.active && !m.initFailed ? &m : nullptr;
        IrqSpinLockRelease(&g_moduleLock, flags);
        SerialPrintf("module: '%s' already loaded or initializing — skipping '%s'\n",
                     pr.info->name, path);
        VmmFreePages(VirtualAddress(pr.baseVirt), pr.pageCount);
        return existing;
    }
    slot->info = pr.info;
    slot->baseVirt = pr.baseVirt;
    slot->pageCount = pr.pageCount;
    reservation.slot = nullptr; // mapping now owns its lifecycle
    IrqSpinLockRelease(&g_moduleLock, flags);

    const int rc = pr.info->init ? pr.info->init() : 0;
    if (rc == 0)
        SerialPrintf("module: '%s' v%s loaded OK at 0x%lx (%lu pages)\n",
                     pr.info->name, pr.info->version, pr.baseVirt, pr.pageCount);
    return ModuleCompleteInit(slot, rc) ? slot : nullptr;
}

bool ModuleUnload(ModuleHandle* handle)
{
    const uint64_t flags = IrqSpinLockAcquire(&g_moduleLock);
    // Only registry-owned handles are accepted. Keep the identity reserved
    // through exit/free; retaining callbacks and claiming unload are exclusive.
    bool owned = false;
    for (auto& m : g_modules) if (&m == handle) owned = true;
    if (!owned || !handle->active || handle->loading || handle->unloading || handle->resident) {
        IrqSpinLockRelease(&g_moduleLock, flags);
        SerialPuts("module: unload refused (inactive, busy or permanently retained)\n");
        return false;
    }
    handle->active = false;
    handle->unloading = true;
    const ModuleInfo* info = handle->info;
    const uint64_t baseVirt = handle->baseVirt, pages = handle->pageCount;
    IrqSpinLockRelease(&g_moduleLock, flags);
    if (info && info->exit) info->exit();
    VmmFreePages(VirtualAddress(baseVirt), pages);
    const uint64_t finishFlags = IrqSpinLockAcquire(&g_moduleLock);
    *handle = {};
    IrqSpinLockRelease(&g_moduleLock, finishFlags);
    return true;
}

ModuleHandle* ModuleFind(const char* name)
{
    if (!name) return nullptr;
    const uint64_t flags = IrqSpinLockAcquire(&g_moduleLock);
    for (uint32_t i = 0; i < MODULE_MAX; ++i)
    {
        if (g_modules[i].active && g_modules[i].info &&
            ModStrEq(g_modules[i].info->name, name))
        {
            IrqSpinLockRelease(&g_moduleLock, flags);
            return &g_modules[i];
        }
    }
    IrqSpinLockRelease(&g_moduleLock, flags);
    return nullptr;
}

void ModuleDump()
{
    KPrintf("Loaded modules:\n");
    uint32_t count = 0;
    for (uint32_t i = 0; i < MODULE_MAX; ++i) {
        const ModuleSnapshot snap = ModuleSnapshotAt(i);
        if (!snap.active) continue;
        KPrintf("  %-16s v%-6s %s%s%s\n", snap.name, snap.version, snap.description,
                snap.resident ? " [resident]" : "", snap.initFailed ? " [init failed]" : "");
        ++count;
    }
    if (!count) KPrintf("  (none)\n");
}

uint32_t ModuleDiscoverAndLoad(const char* dirPath)
{
    Vnode* dir = VfsOpen(dirPath);
    if (!dir)
    {
        SerialPrintf("module: discover: cannot open '%s'\n", dirPath);
        return 0;
    }

    uint32_t loaded = 0;
    uint32_t failed = 0;
    DirEntry entry;
    uint32_t cookie = 0;

    // Build path prefix for each entry: dirPath + "/" + name
    uint32_t dirLen = ModStrLen(dirPath);
    char pathBuf[128];
    if (dirLen >= sizeof(pathBuf) - 16)
    {
        VfsClose(dir);
        return 0;
    }
    ModCopy(pathBuf, dirPath, dirLen);
    pathBuf[dirLen] = '/';

    while (VfsReaddir(dir, &entry, &cookie) > 0)
    {
        // Only load files ending in ".mod"
        if (!ModStrEndsWith(entry.name, ".mod")) continue;

        uint32_t nameLen = ModStrLen(entry.name);
        if (dirLen + 1 + nameLen + 1 > sizeof(pathBuf)) continue;
        ModCopy(pathBuf + dirLen + 1, entry.name, nameLen + 1);

        if (ModuleLoad(pathBuf))
            ++loaded;
        else
            ++failed;
    }
    VfsClose(dir);

    SerialPrintf("module: discover '%s': %u loaded, %u failed\n",
                 dirPath, loaded, failed);
    return loaded;
}

uint32_t ModuleMaxSlots() { return MODULE_MAX; }

ModuleSnapshot ModuleSnapshotAt(uint32_t idx)
{
    ModuleSnapshot snap = {};
    if (idx >= MODULE_MAX) return snap;
    const uint64_t flags = IrqSpinLockAcquire(&g_moduleLock);
    const auto& m = g_modules[idx];
    if (m.active && m.info) {
        snap.active = true;
        const auto copy = [](char* out, uint32_t size, const char* in) {
            uint32_t n = 0;
            if (in) while (n + 1 < size && in[n]) { out[n] = in[n]; ++n; }
            out[n] = 0;
        };
        copy(snap.name, sizeof(snap.name), m.info->name);
        copy(snap.version, sizeof(snap.version), m.info->version);
        copy(snap.description, sizeof(snap.description), m.info->description);
        snap.resident = m.resident;
        snap.initFailed = m.initFailed;
        snap.sizeBytes = m.pageCount * 4096;
    }
    IrqSpinLockRelease(&g_moduleLock, flags);
    return snap;
}

} // namespace brook
