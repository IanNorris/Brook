// fw_cfg.cpp — QEMU fw_cfg interface reader
//
// Reads configuration data passed by QEMU via the fw_cfg pseudo-device.
// Uses legacy I/O port interface (0x510 selector, 0x511 data).
//
// Typical use: pass `-fw_cfg name=opt/bootargs,string=doom4` on QEMU
// command line, then read it at boot with FwCfgReadFile("opt/bootargs").

#include "fw_cfg.h"
#include "serial.h"
#include "kprintf.h"
#include "string.h"

namespace brook {

// I/O ports for the legacy fw_cfg interface
static constexpr uint16_t FWCFG_PORT_SEL  = 0x510;  // Write: 16-bit selector
static constexpr uint16_t FWCFG_PORT_DATA = 0x511;  // Read: 8-bit data

// Well-known selector keys
static constexpr uint16_t FWCFG_SIGNATURE = 0x0000;
static constexpr uint16_t FWCFG_ID        = 0x0001;
static constexpr uint16_t FWCFG_FILE_DIR  = 0x0019;

static bool g_fwCfgAvailable = false;

// Directory entry from QEMU fw_cfg
struct FwCfgFile {
    uint32_t size;      // big-endian
    uint16_t select;    // big-endian
    uint16_t reserved;
    char     name[56];
};

// Byte-swap helpers
static inline uint16_t Bswap16(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t Bswap32(uint32_t v) { return __builtin_bswap32(v); }

static inline void FwCfgSelect(uint16_t key)
{
    __asm__ volatile("outw %0, %1" :: "a"(key), "Nd"(FWCFG_PORT_SEL));
}

static inline uint8_t FwCfgRead8()
{
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(FWCFG_PORT_DATA));
    return val;
}

static void FwCfgReadBytes(void* buf, uint32_t size)
{
    auto* dst = static_cast<uint8_t*>(buf);
    for (uint32_t i = 0; i < size; ++i)
        dst[i] = FwCfgRead8();
}

void FwCfgReadRaw(uint16_t key, void* buf, uint32_t size)
{
    FwCfgSelect(key);
    FwCfgReadBytes(buf, size);
}

bool FwCfgInit()
{
    // Check signature: "QEMU"
    char sig[4];
    FwCfgReadRaw(FWCFG_SIGNATURE, sig, 4);

    if (sig[0] != 'Q' || sig[1] != 'E' || sig[2] != 'M' || sig[3] != 'U')
    {
        SerialPuts("FW_CFG: not available (signature mismatch)\n");
        g_fwCfgAvailable = false;
        return false;
    }

    // Read interface ID/capabilities
    uint32_t id = 0;
    FwCfgReadRaw(FWCFG_ID, &id, 4);
    id = Bswap32(id);

    g_fwCfgAvailable = true;

    SerialPrintf("FW_CFG: available (id=0x%x, %s DMA)\n",
                 id, (id & 2) ? "with" : "no");

    // Dump directory for debugging
    FwCfgSelect(FWCFG_FILE_DIR);
    uint32_t count;
    FwCfgReadBytes(&count, 4);
    count = Bswap32(count);

    DbgPrintf("FW_CFG: %u files:\n", count);
    for (uint32_t i = 0; i < count && i < 64; ++i)
    {
        FwCfgFile entry;
        FwCfgReadBytes(&entry, sizeof(entry));
        uint32_t fsize = Bswap32(entry.size);
        uint16_t fsel  = Bswap16(entry.select);
        (void)fsize; (void)fsel;
        DbgPrintf("  [0x%04x] %s (%u bytes)\n", fsel, entry.name, fsize);
    }

    return true;
}

bool FwCfgAvailable()
{
    return g_fwCfgAvailable;
}

uint32_t FwCfgReadFile(const char* name, void* buf, uint32_t bufSize)
{
    if (!g_fwCfgAvailable || !name || !buf) return 0;

    // Scan the directory for the named file
    FwCfgSelect(FWCFG_FILE_DIR);
    uint32_t count;
    FwCfgReadBytes(&count, 4);
    count = Bswap32(count);

    for (uint32_t i = 0; i < count; ++i)
    {
        FwCfgFile entry;
        FwCfgReadBytes(&entry, sizeof(entry));

        // Compare names
        bool match = true;
        for (uint32_t c = 0; c < 56; ++c)
        {
            if (name[c] != entry.name[c]) { match = false; break; }
            if (name[c] == '\0') break;
        }

        if (match)
        {
            uint32_t fsize = Bswap32(entry.size);
            uint16_t fsel  = Bswap16(entry.select);
            uint32_t toRead = (fsize < bufSize) ? fsize : bufSize;

            FwCfgSelect(fsel);
            FwCfgReadBytes(buf, toRead);

            return toRead;
        }
    }

    return 0;
}

} // namespace brook
