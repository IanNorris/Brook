#include "module.h"
#include "elf.h"
#include "ksymtab.h"
#include "vmm.h"
#include "heap.h"
#include "vfs.h"
#include "serial.h"
#include "mem_tag.h"

namespace brook {

// ---- Module registry ----

static constexpr uint32_t MODULE_MAX = 64;
static ModuleHandle g_modules[MODULE_MAX];

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
    volatile uint8_t* p = static_cast<volatile uint8_t*>(dst);
    for (uint64_t i = 0; i < len; ++i) p[i] = 0;
}

// Copy bytes.
static void ModCopy(void* dst, const void* src, uint64_t len)
{
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    for (uint64_t i = 0; i < len; ++i) d[i] = s[i];
}

// ---- ELF loader internals ----

struct LoadedSection {
    uint64_t base;   // virtual address of loaded section (0 = not loaded)
};

static constexpr uint32_t MAX_SECTIONS = 64;

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
    if (ehdr->e_shnum > MAX_SECTIONS)
    {
        SerialPrintf("module: too many sections (%u)\n", ehdr->e_shnum);
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
                      const Elf64_Shdr* shdrs, uint32_t shnum,
                      const Elf64_Sym* symtab, uint32_t symCount,
                      const char* strtab, const LoadedSection* secs)
{
    // sh_info = index of the section being relocated.
    uint32_t targetSecIdx = relaSec->sh_info;
    if (targetSecIdx >= shnum) return true; // nothing to do
    uint64_t targetBase = secs[targetSecIdx].base;
    if (!targetBase) return true; // section not loaded (ok to skip)

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
    if (ehdr->e_shstrndx < shnum)
        shstrtab = reinterpret_cast<const char*>(
            data + shdrs[ehdr->e_shstrndx].sh_offset);

    // ---- Pass 1: calculate total memory needed ----
    // Allocate a contiguous virtual range covering all SHF_ALLOC sections.

    uint64_t totalSize = 0;
    for (uint32_t i = 0; i < shnum; ++i)
    {
        const Elf64_Shdr& s = shdrs[i];
        if (!(s.sh_flags & SHF_ALLOC)) continue;
        // Align to section alignment.
        uint64_t align = s.sh_addralign > 1 ? s.sh_addralign : 1;
        totalSize = (totalSize + align - 1) & ~(align - 1);
        totalSize += s.sh_size;
    }
    if (totalSize == 0)
    {
        SerialPuts("module: no allocatable sections\n");
        return false;
    }

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
    LoadedSection secs[MAX_SECTIONS];
    for (uint32_t i = 0; i < MAX_SECTIONS; ++i) secs[i].base = 0;
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
            symtab   = reinterpret_cast<const Elf64_Sym*>(data + shdrs[i].sh_offset);
            symCount = static_cast<uint32_t>(shdrs[i].sh_size / sizeof(Elf64_Sym));
            // sh_link = index of associated string table.
            uint32_t strIdx = shdrs[i].sh_link;
            if (strIdx < shnum)
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
        if (!ApplyRela(&shdrs[i], data, shdrs, shnum, symtab, symCount, strtab, secs))
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

// ---- Public API ----

ModuleHandle* ModuleLoad(const char* path)
{
    // Find a free registry slot.
    ModuleHandle* slot = nullptr;
    for (uint32_t i = 0; i < MODULE_MAX; ++i)
    {
        if (!g_modules[i].active) { slot = &g_modules[i]; break; }
    }
    if (!slot)
    {
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

    // Call module init.
    if (pr.info->init)
    {
        int rc = pr.info->init();
        if (rc != 0)
        {
            SerialPrintf("module: '%s' init() returned %d — unloading\n",
                         pr.info->name, rc);
            VmmFreePages(VirtualAddress(pr.baseVirt), pr.pageCount);
            return nullptr;
        }
    }

    slot->info      = pr.info;
    slot->baseVirt  = pr.baseVirt;
    slot->pageCount = pr.pageCount;
    slot->active    = true;

    SerialPrintf("module: '%s' v%s loaded OK at 0x%lx (%lu pages)\n",
                 pr.info->name, pr.info->version,
                 pr.baseVirt, pr.pageCount);
    return slot;
}

void ModuleUnload(ModuleHandle* handle)
{
    if (!handle || !handle->active) return;
    if (handle->info && handle->info->exit)
        handle->info->exit();
    VmmFreePages(VirtualAddress(handle->baseVirt), handle->pageCount);
    handle->active    = false;
    handle->info      = nullptr;
    handle->baseVirt  = 0;
    handle->pageCount = 0;
}

ModuleHandle* ModuleFind(const char* name)
{
    if (!name) return nullptr;
    for (uint32_t i = 0; i < MODULE_MAX; ++i)
    {
        if (g_modules[i].active && g_modules[i].info &&
            ModStrEq(g_modules[i].info->name, name))
            return &g_modules[i];
    }
    return nullptr;
}

void ModuleDump()
{
    SerialPuts("Loaded modules:\n");
    uint32_t count = 0;
    for (uint32_t i = 0; i < MODULE_MAX; ++i)
    {
        if (!g_modules[i].active) continue;
        SerialPrintf("  %-20s  v%-8s  0x%016lx  (%lu pages)  %s\n",
                     g_modules[i].info->name,
                     g_modules[i].info->version,
                     g_modules[i].baseVirt,
                     g_modules[i].pageCount,
                     g_modules[i].info->description);
        ++count;
    }
    if (!count) SerialPuts("  (none)\n");
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

} // namespace brook
