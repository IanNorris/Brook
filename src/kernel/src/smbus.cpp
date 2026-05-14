// Intel PCH SMBus (i801) driver — polling mode.
// Reference: Linux i2c-i801.c, Intel 400-series PCH datasheet.
//
// This driver targets the Comet Lake-H PCH (device 8086:06A3) but includes
// device IDs for common PCH variants. It uses legacy port I/O via BAR4.

#include "smbus.h"
#include "pci.h"
#include "serial.h"
#include "portio.h"

namespace brook {

// --- Register offsets from I/O base ---
static constexpr uint8_t REG_HSTSTS  = 0x00; // Host Status
static constexpr uint8_t REG_HSTCNT  = 0x02; // Host Control
static constexpr uint8_t REG_HSTCMD  = 0x03; // Host Command
static constexpr uint8_t REG_HSTADD  = 0x04; // Host Address
static constexpr uint8_t REG_HSTDAT0 = 0x05; // Host Data 0
static constexpr uint8_t REG_HSTDAT1 = 0x06; // Host Data 1
static constexpr uint8_t REG_BLKDAT  = 0x07; // Block Data
static constexpr uint8_t REG_AUXCTL  = 0x0D; // Auxiliary Control

// --- HSTSTS bits ---
static constexpr uint8_t STS_BYTE_DONE = (1 << 7);
static constexpr uint8_t STS_FAILED    = (1 << 4);
static constexpr uint8_t STS_BUS_ERR   = (1 << 3);
static constexpr uint8_t STS_DEV_ERR   = (1 << 2);
static constexpr uint8_t STS_INTR      = (1 << 1);
static constexpr uint8_t STS_BUSY      = (1 << 0);
static constexpr uint8_t STS_ERROR     = STS_FAILED | STS_BUS_ERR | STS_DEV_ERR;
static constexpr uint8_t STS_CLEAR     = STS_BYTE_DONE | STS_INTR | STS_ERROR;

// --- HSTCNT bits and transaction types ---
static constexpr uint8_t CNT_START     = (1 << 6);
static constexpr uint8_t CNT_KILL      = (1 << 1);
static constexpr uint8_t CMD_QUICK     = 0x00;
static constexpr uint8_t CMD_BYTE_DATA = 0x08;
static constexpr uint8_t CMD_WORD_DATA = 0x0C;

// --- PCI config offset for host configuration ---
static constexpr uint8_t PCI_SMBHSTCFG = 0x40;
static constexpr uint8_t CFG_HST_EN    = (1 << 0);
static constexpr uint8_t CFG_I2C_EN    = (1 << 2);

// --- Supported PCI device IDs (Intel PCH SMBus) ---
static constexpr uint16_t SMBUS_DEVICE_IDS[] = {
    0x06A3, // Comet Lake-H (target: i7-10700, Z490/H470/B460)
    0x02A3, // Comet Lake-U
    0xA3A3, // Comet Lake-V
    0xA323, // Cannon Lake-H
    0xA2A3, // Kaby Lake-H
    0xA123, // Sunrise Point-H (Skylake)
    0x8C22, // Lynx Point (Haswell)
    0x1C22, // 6 Series PCH (Sandy Bridge)
    0xA0A3, // Tiger Lake-LP
    0x7AA3, // Alder Lake-S
    0x7A23, // Raptor Lake-S
};

// --- Driver state ---
static uint16_t g_smbBase = 0;
static bool     g_smbInit = false;

// --- Low-level I/O ---
static inline uint8_t smb_inb(uint8_t reg) {
    return inb(g_smbBase + reg);
}
static inline void smb_outb(uint8_t reg, uint8_t val) {
    outb(g_smbBase + reg, val);
}

// Microsecond delay (busy-wait via LAPIC tick or TSC).
extern volatile uint64_t g_lapicTickCount;
static void udelay(uint32_t us) {
    // Each LAPIC tick is ~1ms in Brook; for sub-ms delays, spin on TSC.
    // For coarse delays (>= 1ms), use tick count.
    if (us >= 1000) {
        uint64_t target = g_lapicTickCount + (us / 1000) + 1;
        while (g_lapicTickCount < target)
            __asm__ volatile("pause");
    } else {
        // ~3 GHz TSC → ~3000 cycles/us. Conservative: spin 4000 cycles/us.
        uint64_t cycles = (uint64_t)us * 4000;
        uint64_t start;
        __asm__ volatile("rdtsc" : "=A"(start));
        // For x86-64, rdtsc returns in edx:eax. Use rdtscp for serialization.
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        uint64_t tsc_start = ((uint64_t)hi << 32) | lo;
        while (true) {
            __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
            uint64_t now = ((uint64_t)hi << 32) | lo;
            if (now - tsc_start >= cycles) break;
            __asm__ volatile("pause");
        }
    }
}

// --- Transaction helpers ---

// Wait for transaction to complete (not busy + status set).
// Returns status flags on completion, or -1 on timeout.
static int smbus_wait(void) {
    // 25ms timeout at ~250us per iteration
    for (int i = 0; i < 100; i++) {
        uint8_t st = smb_inb(REG_HSTSTS);
        if (!(st & STS_BUSY) && (st & (STS_ERROR | STS_INTR)))
            return st;
        udelay(250);
    }
    // Kill stuck transaction
    smb_outb(REG_HSTCNT, CNT_KILL);
    udelay(1000);
    smb_outb(REG_HSTCNT, 0);
    return -1; // timeout
}

// Clear stale status and check bus is free.
static int smbus_pre(void) {
    uint8_t st = smb_inb(REG_HSTSTS);
    if (st & STS_BUSY) return -16; // -EBUSY
    if (st & STS_CLEAR)
        smb_outb(REG_HSTSTS, st & STS_CLEAR); // write-1-to-clear
    return 0;
}

// Convert raw status to error code.
static int smbus_status_to_error(int st) {
    if (st < 0) return -110; // ETIMEDOUT
    if (st & STS_DEV_ERR) return -6;  // ENXIO (no device / NAK)
    if (st & STS_BUS_ERR) return -11; // EAGAIN (arbitration lost)
    if (st & STS_FAILED)  return -5;  // EIO
    return 0; // success
}

// --- Public API ---

int SmbusInit() {
    if (g_smbInit) return 0;

    PciDevice dev;
    bool found = false;

    for (uint16_t devId : SMBUS_DEVICE_IDS) {
        if (PciFindDevice(0x8086, devId, dev)) {
            found = true;
            break;
        }
    }

    if (!found) {
        SerialPrintf("smbus: no Intel PCH SMBus controller found\n");
        return -19; // ENODEV
    }

    SerialPrintf("smbus: found controller PCI %02x:%02x.%x device %04x:%04x\n",
                 dev.bus, dev.dev, dev.fn, dev.vendorId, dev.deviceId);

    // Read BAR4 (I/O base)
    uint32_t bar4 = PciConfigRead32(dev.bus, dev.dev, dev.fn, 0x20);
    if (!PciBarIsIo(bar4) || (bar4 & ~0x1F) == 0) {
        SerialPrintf("smbus: BAR4 invalid (0x%08x), cannot use I/O method\n", bar4);
        return -22; // EINVAL
    }
    g_smbBase = PciBarIoBase(bar4);

    // Enable host controller (read-modify-write via 16-bit access since no Write8)
    uint8_t hstcfg = PciConfigRead8(dev.bus, dev.dev, dev.fn, PCI_SMBHSTCFG);
    SerialPrintf("smbus: HSTCFG=0x%02x base=0x%04x\n", hstcfg, g_smbBase);

    hstcfg &= ~CFG_I2C_EN; // SMBus timing mode
    hstcfg |= CFG_HST_EN;  // enable controller
    // Write back via 16-bit preserving the adjacent byte
    uint16_t hstcfg16 = PciConfigRead16(dev.bus, dev.dev, dev.fn, PCI_SMBHSTCFG);
    hstcfg16 = (hstcfg16 & 0xFF00) | hstcfg;
    PciConfigWrite16(dev.bus, dev.dev, dev.fn, PCI_SMBHSTCFG, hstcfg16);

    // Clear auxiliary control
    smb_outb(REG_AUXCTL, smb_inb(REG_AUXCTL) & 0xFC);
    // Clear stale status
    smb_outb(REG_HSTSTS, STS_CLEAR);

    g_smbInit = true;
    SerialPrintf("smbus: initialized, I/O base 0x%04x\n", g_smbBase);
    return 0;
}

int SmbusProbe(uint8_t addr) {
    if (!g_smbInit) return -19;
    if (addr < 0x03 || addr > 0x77) return -22;

    int rc = smbus_pre();
    if (rc < 0) return rc;

    smb_outb(REG_HSTADD, (addr << 1) | 0); // Quick Write
    smb_outb(REG_HSTCNT, CMD_QUICK | CNT_START);

    int st = smbus_wait();
    smb_outb(REG_HSTSTS, STS_CLEAR);
    return smbus_status_to_error(st);
}

int SmbusReadByte(uint8_t addr, uint8_t reg) {
    if (!g_smbInit) return -19;

    int rc = smbus_pre();
    if (rc < 0) return rc;

    smb_outb(REG_HSTADD, (addr << 1) | 1); // Read
    smb_outb(REG_HSTCMD, reg);
    smb_outb(REG_HSTCNT, CMD_BYTE_DATA | CNT_START);

    int st = smbus_wait();
    uint8_t data = smb_inb(REG_HSTDAT0);
    smb_outb(REG_HSTSTS, STS_CLEAR);

    int err = smbus_status_to_error(st);
    return err < 0 ? err : (int)data;
}

int SmbusWriteByte(uint8_t addr, uint8_t reg, uint8_t val) {
    if (!g_smbInit) return -19;

    int rc = smbus_pre();
    if (rc < 0) return rc;

    smb_outb(REG_HSTADD, (addr << 1) | 0); // Write
    smb_outb(REG_HSTCMD, reg);
    smb_outb(REG_HSTDAT0, val);
    smb_outb(REG_HSTCNT, CMD_BYTE_DATA | CNT_START);

    int st = smbus_wait();
    smb_outb(REG_HSTSTS, STS_CLEAR);
    return smbus_status_to_error(st);
}

int SmbusReadWord(uint8_t addr, uint8_t reg) {
    if (!g_smbInit) return -19;

    int rc = smbus_pre();
    if (rc < 0) return rc;

    smb_outb(REG_HSTADD, (addr << 1) | 1); // Read
    smb_outb(REG_HSTCMD, reg);
    smb_outb(REG_HSTCNT, CMD_WORD_DATA | CNT_START);

    int st = smbus_wait();
    uint8_t lo = smb_inb(REG_HSTDAT0);
    uint8_t hi = smb_inb(REG_HSTDAT1);
    smb_outb(REG_HSTSTS, STS_CLEAR);

    int err = smbus_status_to_error(st);
    return err < 0 ? err : (int)((hi << 8) | lo);
}

int SmbusScan() {
    if (!g_smbInit) return -19;

    SerialPrintf("smbus: scanning bus...\n");
    int found = 0;

    for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
        // Skip reserved ranges
        if (addr >= 0x08 && addr <= 0x0B) continue; // High-speed master codes
        if (addr >= 0x78) break; // 10-bit prefix range

        int rc = SmbusProbe(addr);
        if (rc == 0) {
            const char* desc = "";
            if (addr >= 0x50 && addr <= 0x57) desc = " (SPD EEPROM)";
            else if (addr >= 0x48 && addr <= 0x4F) desc = " (temp sensor?)";
            else if (addr >= 0x60 && addr <= 0x67) desc = " (clock gen?)";
            SerialPrintf("smbus: device at 0x%02x%s\n", addr, desc);
            found++;
        }
    }

    SerialPrintf("smbus: scan complete, %d device(s) found\n", found);
    return found;
}

int SmbusSpdRead(int slot) {
    if (!g_smbInit) return -19;
    if (slot < 0 || slot > 7) return -22;

    uint8_t addr = 0x50 | (uint8_t)slot;

    // Check DIMM present
    int rc = SmbusProbe(addr);
    if (rc < 0) {
        SerialPrintf("smbus: no DIMM in slot %d (addr 0x%02x)\n", slot, addr);
        return rc;
    }

    // Read DRAM type (byte 2)
    int dramType = SmbusReadByte(addr, 2);
    if (dramType < 0) return dramType;

    const char* typeStr = "Unknown";
    if (dramType == 0x0C) typeStr = "DDR4";
    else if (dramType == 0x0B) typeStr = "DDR3";
    else if (dramType == 0x12) typeStr = "DDR5";

    // Read module type (byte 3)
    int moduleType = SmbusReadByte(addr, 3);
    const char* modStr = "Unknown";
    if (moduleType >= 0) {
        switch (moduleType & 0x0F) {
            case 0x01: modStr = "RDIMM"; break;
            case 0x02: modStr = "UDIMM"; break;
            case 0x03: modStr = "SO-DIMM"; break;
            case 0x04: modStr = "LRDIMM"; break;
        }
    }

    // Read bus width (byte 12)
    int busWidth = SmbusReadByte(addr, 12);
    int widthBits = 8; // minimum
    if (busWidth >= 0) {
        switch (busWidth & 0x07) {
            case 0: widthBits = 8; break;
            case 1: widthBits = 16; break;
            case 2: widthBits = 32; break;
            case 3: widthBits = 64; break;
        }
    }
    bool ecc = (busWidth >= 0) && ((busWidth & 0x18) != 0);

    // Read part number (bytes 329–348 for DDR4, offset varies by generation)
    // DDR4: manufacturer part number is at bytes 329–348 (20 chars, ASCII)
    // For simplicity, read bytes 329–348 (within page 1 for DDR4, need page switch)
    // Page 0 covers bytes 0–255. Part number is at 329 = page 1 byte 73.
    // For now, just report what we can from page 0.

    SerialPrintf("smbus: DIMM slot %d: %s %s %d-bit%s\n",
                 slot, typeStr, modStr, widthBits, ecc ? " ECC" : "");

    // Read density (byte 4) to estimate capacity
    int density = SmbusReadByte(addr, 4);
    if (density >= 0) {
        int densityMb = 256 << (density & 0x0F); // per die, Mbit
        int ranks = ((density >> 3) & 0x07) + 1; // logical ranks
        // Rough capacity: density_per_die * bus_width / 8 * ranks
        // This is a simplification; real calc needs bank groups etc.
        SerialPrintf("smbus: DIMM slot %d: ~%d Mbit/die, %d rank(s)\n",
                     slot, densityMb, ranks);
    }

    return 0;
}

} // namespace brook
