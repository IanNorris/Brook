// intel_hda_mod.cpp — Intel High Definition Audio driver for ICH9 (QEMU q35)
//
// Implements:
//   - Controller reset and CORB/RIRB command buffers
//   - Codec discovery (STATESTS) and basic widget enumeration
//   - Output stream 0 with Buffer Descriptor List (BDL) for PCM playback
//   - Simple kernel API: HdaPlayPcm(samples, count, rate, channels, bits)
//
// Reference: Intel High Definition Audio Specification Rev 1.0a (June 2010)
// QEMU uses a STAC9220-like codec (Sigmatel)

#include "module_abi.h"
#include "pci.h"
#include "serial.h"
#include "kprintf.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "memory/address.h"
#include "mem_tag.h"

MODULE_IMPORT_SYMBOL(PciFindDevice);
MODULE_IMPORT_SYMBOL(PciEnableMemSpace);
MODULE_IMPORT_SYMBOL(PciEnableBusMaster);
MODULE_IMPORT_SYMBOL(PciConfigRead32);
MODULE_IMPORT_SYMBOL(PciConfigRead16);
MODULE_IMPORT_SYMBOL(PciConfigRead8);
MODULE_IMPORT_SYMBOL(PciConfigWrite16);
MODULE_IMPORT_SYMBOL(SerialPrintf);
MODULE_IMPORT_SYMBOL(SerialPuts);
MODULE_IMPORT_SYMBOL(KPrintf);
MODULE_IMPORT_SYMBOL(VmmAllocPages);
MODULE_IMPORT_SYMBOL(VmmVirtToPhys);
MODULE_IMPORT_SYMBOL(VmmMapPage);
MODULE_IMPORT_SYMBOL(PmmAllocPage);

using namespace brook;

// ---------------------------------------------------------------------------
// HDA controller registers (BAR0 MMIO, Intel spec §3)
// ---------------------------------------------------------------------------

enum HdaReg : uint32_t {
    GCAP        = 0x00,  // Global capabilities (16-bit)
    VMIN        = 0x02,  // Minor version (8-bit)
    VMAJ        = 0x03,  // Major version (8-bit)
    OUTPAY      = 0x04,  // Output payload capability (16-bit)
    INPAY       = 0x06,  // Input payload capability (16-bit)
    GCTL        = 0x08,  // Global control (32-bit)
    WAKEEN      = 0x0C,  // Wake enable (16-bit)
    STATESTS    = 0x0E,  // State change status (16-bit)

    INTCTL      = 0x20,  // Interrupt control (32-bit)
    INTSTS      = 0x24,  // Interrupt status (32-bit)

    WALCLK      = 0x30,  // Wall clock counter (32-bit)
    SSYNC       = 0x38,  // Stream sync (32-bit)

    // CORB registers
    CORBLBASE   = 0x40,  // CORB lower base address (32-bit)
    CORBUBASE   = 0x44,  // CORB upper base address (32-bit)
    CORBWP      = 0x48,  // CORB write pointer (16-bit)
    CORBRP      = 0x4A,  // CORB read pointer (16-bit)
    CORBCTL     = 0x4C,  // CORB control (8-bit)
    CORBSTS     = 0x4E,  // CORB status (8-bit)
    CORBSIZE    = 0x4F,  // CORB size (8-bit)

    // RIRB registers
    RIRBLBASE   = 0x50,  // RIRB lower base address (32-bit)
    RIRBUBASE   = 0x54,  // RIRB upper base address (32-bit)
    RIRBWP      = 0x58,  // RIRB write pointer (16-bit)
    RINTCNT     = 0x5A,  // RIRB interrupt count (16-bit)
    RIRBCTL     = 0x5C,  // RIRB control (8-bit)
    RIRBSTS     = 0x5D,  // RIRB status (8-bit)
    RIRBSIZE    = 0x5E,  // RIRB size (8-bit)

    // Stream descriptor base (each stream descriptor is 0x20 bytes)
    // SD0 starts at 0x80 (input stream 0)
    // Output streams start after input streams
    SD_BASE     = 0x80,
};

// Stream descriptor offsets (within each 0x20-byte block)
enum SdReg : uint32_t {
    SD_CTL      = 0x00,  // Control (24-bit, write as 32-bit with STS)
    SD_STS      = 0x03,  // Status (8-bit)
    SD_LPIB     = 0x04,  // Link position in buffer (32-bit)
    SD_CBL      = 0x08,  // Cyclic buffer length (32-bit)
    SD_LVI      = 0x0C,  // Last valid index (16-bit)
    SD_FIFOS    = 0x10,  // FIFO size (16-bit)
    SD_FMT      = 0x12,  // Format (16-bit)
    SD_BDLPL    = 0x18,  // BDL pointer lower (32-bit)
    SD_BDLPU    = 0x1C,  // BDL pointer upper (32-bit)
};

// Stream control bits
static constexpr uint32_t SD_CTL_SRST   = (1 << 0);   // Stream reset
static constexpr uint32_t SD_CTL_RUN    = (1 << 1);   // Stream run
static constexpr uint32_t SD_CTL_IOCE   = (1 << 2);   // Interrupt on completion enable

// Stream status bits
static constexpr uint8_t SD_STS_BCIS  = (1 << 2);  // Buffer completion interrupt
static constexpr uint8_t SD_STS_FIFOE = (1 << 3);  // FIFO error
static constexpr uint8_t SD_STS_DESE  = (1 << 4);  // Descriptor error

// GCTL bits
static constexpr uint32_t GCTL_CRST = (1 << 0);  // Controller reset

// CORBCTL bits
static constexpr uint8_t CORBCTL_RUN  = (1 << 1);

// RIRBCTL bits
static constexpr uint8_t RIRBCTL_RUN  = (1 << 1);

// INTCTL bits
static constexpr uint32_t INTCTL_GIE = (1u << 31);  // Global interrupt enable
static constexpr uint32_t INTCTL_CIE = (1u << 30);  // Controller interrupt enable

// ---------------------------------------------------------------------------
// HDA codec verb helpers
// ---------------------------------------------------------------------------

// Codec verb format: [codec_addr:4][indirect:1][nid:7][verb:12/20][payload:8/0]
// For get/set parameter verbs: [codec:4][0][nid:7][verb_id:12][payload:8]

static inline uint32_t HdaVerb(uint8_t codec, uint8_t nid,
                                uint32_t verb, uint8_t payload)
{
    return (static_cast<uint32_t>(codec) << 28)
         | (static_cast<uint32_t>(nid) << 20)
         | ((verb & 0xFFF) << 8)
         | payload;
}

// 4-bit verb format: [codec:4][0][nid:7][verb_4:4][payload:16]
static inline uint32_t HdaVerb4(uint8_t codec, uint8_t nid,
                                 uint8_t verb4, uint16_t payload)
{
    return (static_cast<uint32_t>(codec) << 28)
         | (static_cast<uint32_t>(nid) << 20)
         | (static_cast<uint32_t>(verb4) << 16)
         | payload;
}

// Common get-parameter verb IDs (12-bit)
static constexpr uint32_t VERB_GET_PARAM       = 0xF00;

// Common set-parameter verb IDs
static constexpr uint32_t VERB_SET_AMP_GAIN    = 0x3;  // 4-bit verb
static constexpr uint32_t VERB_SET_PIN_CTRL    = 0x707;
static constexpr uint32_t VERB_SET_EAPDBTL     = 0x70C;
static constexpr uint32_t VERB_SET_POWER_STATE = 0x705;
static constexpr uint32_t VERB_SET_CONV_STREAM = 0x706;
static constexpr uint32_t VERB_SET_CONV_FMT    = 0x2;  // 4-bit verb

// Parameter IDs (for VERB_GET_PARAM)
static constexpr uint8_t PARAM_VENDOR_ID      = 0x00;
static constexpr uint8_t PARAM_SUB_NODE_COUNT = 0x04;
static constexpr uint8_t PARAM_FN_GROUP_TYPE  = 0x05;
static constexpr uint8_t PARAM_WIDGET_CAPS    = 0x09;

// Widget type from caps (bits 23:20)
enum WidgetType : uint8_t {
    WT_AUDIO_OUTPUT = 0,
    WT_AUDIO_INPUT  = 1,
    WT_AUDIO_MIXER  = 2,
    WT_AUDIO_SELECT = 3,
    WT_PIN_COMPLEX  = 4,
    WT_POWER        = 5,
    WT_VOLUME_KNOB  = 6,
    WT_BEEP_GEN     = 7,
};

// ---------------------------------------------------------------------------
// MMIO accessors
// ---------------------------------------------------------------------------

static volatile uint8_t* g_mmioBase = nullptr;

static inline void hda_write8(uint32_t off, uint8_t v)
{ *reinterpret_cast<volatile uint8_t*>(g_mmioBase + off) = v; }
static inline void hda_write16(uint32_t off, uint16_t v)
{ *reinterpret_cast<volatile uint16_t*>(g_mmioBase + off) = v; }
static inline void hda_write32(uint32_t off, uint32_t v)
{ *reinterpret_cast<volatile uint32_t*>(g_mmioBase + off) = v; }

static inline uint8_t hda_read8(uint32_t off)
{ return *reinterpret_cast<volatile uint8_t*>(g_mmioBase + off); }
static inline uint16_t hda_read16(uint32_t off)
{ return *reinterpret_cast<volatile uint16_t*>(g_mmioBase + off); }
static inline uint32_t hda_read32(uint32_t off)
{ return *reinterpret_cast<volatile uint32_t*>(g_mmioBase + off); }

// ---------------------------------------------------------------------------
// CORB/RIRB (command/response ring buffers)
// ---------------------------------------------------------------------------

static constexpr uint32_t CORB_ENTRIES = 256;
static constexpr uint32_t RIRB_ENTRIES = 256;

static uint32_t* g_corb = nullptr;      // CORB: 256 × 4 bytes = 1KB
static uint64_t  g_corbPhys = 0;

struct RirbEntry {
    uint32_t response;
    uint32_t responseEx; // codec addr in bits 3:0, unsolicited in bit 4
};

static RirbEntry* g_rirb = nullptr;     // RIRB: 256 × 8 bytes = 2KB
static uint64_t   g_rirbPhys = 0;
static uint16_t   g_rirbReadIdx = 0;

// ---------------------------------------------------------------------------
// Buffer Descriptor List for output stream
// ---------------------------------------------------------------------------

struct __attribute__((packed)) BdlEntry {
    uint64_t addr;   // Physical address of buffer
    uint32_t length; // Length in bytes
    uint32_t ioc;    // Interrupt on completion (bit 0)
};

static BdlEntry* g_bdl = nullptr;
static uint64_t  g_bdlPhys = 0;

// Audio buffer
static constexpr uint32_t AUDIO_BUF_SIZE = 64 * 1024; // 64KB audio buffer
static uint8_t*  g_audioBuf = nullptr;
static uint64_t  g_audioBufPhys = 0;

// Output stream descriptor offset (depends on GCAP)
static uint32_t g_outStreamBase = 0;
static uint8_t  g_numInputStreams = 0;
static uint8_t  g_numOutputStreams = 0;

// Discovered codec/widget info
static uint8_t g_codecAddr = 0;
static uint8_t g_dacNid = 0;       // DAC widget NID
static uint8_t g_pinNid = 0;       // Output pin widget NID
static bool    g_initialized = false;

// ---------------------------------------------------------------------------
// Busy-wait helper
// ---------------------------------------------------------------------------

static void BusyWait(uint32_t iters)
{
    for (volatile uint32_t i = 0; i < iters; i++)
        __asm__ volatile("pause");
}

// ---------------------------------------------------------------------------
// CORB/RIRB command interface
// ---------------------------------------------------------------------------

static bool CorbSendVerb(uint32_t verb)
{
    uint16_t wp = hda_read16(CORBWP) & 0xFF;
    wp = (wp + 1) % CORB_ENTRIES;
    g_corb[wp] = verb;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    hda_write16(CORBWP, wp);
    return true;
}

static bool RirbRecvResponse(uint32_t& response, uint32_t timeoutIters = 500000)
{
    for (uint32_t i = 0; i < timeoutIters; i++)
    {
        uint16_t wp = hda_read16(RIRBWP) & 0xFF;
        if (wp != g_rirbReadIdx)
        {
            g_rirbReadIdx = (g_rirbReadIdx + 1) % RIRB_ENTRIES;
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            response = g_rirb[g_rirbReadIdx].response;

            // Clear RIRB interrupt status
            hda_write8(RIRBSTS, 0x05);
            return true;
        }
        __asm__ volatile("pause");
    }
    return false;
}

static bool HdaCommand(uint32_t verb, uint32_t& response)
{
    CorbSendVerb(verb);
    return RirbRecvResponse(response);
}

// Convenience: send verb, ignore response
static bool HdaCommandNoResp(uint32_t verb)
{
    uint32_t dummy;
    return HdaCommand(verb, dummy);
}

// ---------------------------------------------------------------------------
// Controller initialization
// ---------------------------------------------------------------------------

static bool HdaControllerReset()
{
    // Put controller in reset
    hda_write32(GCTL, 0);
    BusyWait(10000);

    // Wait for reset to take effect
    for (int i = 0; i < 100; i++)
    {
        if ((hda_read32(GCTL) & GCTL_CRST) == 0) break;
        BusyWait(10000);
    }

    // Take out of reset
    hda_write32(GCTL, GCTL_CRST);
    BusyWait(10000);

    // Wait for controller to come out of reset
    for (int i = 0; i < 100; i++)
    {
        if (hda_read32(GCTL) & GCTL_CRST) break;
        BusyWait(10000);
    }

    if (!(hda_read32(GCTL) & GCTL_CRST))
    {
        SerialPuts("intel_hda: controller failed to come out of reset\n");
        return false;
    }

    // Wait for codecs to enumerate (give them time after reset)
    BusyWait(100000);

    return true;
}

static bool SetupCorbRirb()
{
    // Allocate CORB (1 page for both CORB + RIRB: 1KB + 2KB = 3KB < 4KB)
    auto page = VmmAllocPages(1, VMM_WRITABLE | VMM_CACHE_DISABLE, MemTag::Device, KernelPid);
    if (!page) return false;

    uint8_t* base = reinterpret_cast<uint8_t*>(page.raw());
    for (int i = 0; i < 4096; i++) base[i] = 0;

    g_corb = reinterpret_cast<uint32_t*>(base);
    g_rirb = reinterpret_cast<RirbEntry*>(base + 1024);

    g_corbPhys = VmmVirtToPhys(KernelPageTable, page).raw();
    g_rirbPhys = g_corbPhys + 1024;

    // Stop CORB and RIRB
    hda_write8(CORBCTL, 0);
    hda_write8(RIRBCTL, 0);
    BusyWait(10000);

    // Set CORB base address
    hda_write32(CORBLBASE, static_cast<uint32_t>(g_corbPhys));
    hda_write32(CORBUBASE, static_cast<uint32_t>(g_corbPhys >> 32));

    // Set CORB size to 256 entries
    hda_write8(CORBSIZE, 0x02); // 256 entries

    // Reset CORB read pointer
    hda_write16(CORBRP, 0x8000); // Set reset bit
    BusyWait(10000);
    hda_write16(CORBRP, 0x0000); // Clear reset bit
    BusyWait(10000);

    // Reset CORB write pointer
    hda_write16(CORBWP, 0);

    // Set RIRB base address
    hda_write32(RIRBLBASE, static_cast<uint32_t>(g_rirbPhys));
    hda_write32(RIRBUBASE, static_cast<uint32_t>(g_rirbPhys >> 32));

    // Set RIRB size to 256 entries
    hda_write8(RIRBSIZE, 0x02);

    // Reset RIRB write pointer
    hda_write16(RIRBWP, 0x8000); // Reset
    BusyWait(1000);

    g_rirbReadIdx = 0;

    // Start CORB and RIRB DMA
    hda_write8(CORBCTL, CORBCTL_RUN);
    hda_write8(RIRBCTL, RIRBCTL_RUN);
    BusyWait(10000);

    return true;
}

// ---------------------------------------------------------------------------
// Codec discovery and widget enumeration
// ---------------------------------------------------------------------------

static bool DiscoverCodec()
{
    uint16_t statests = hda_read16(STATESTS);
    if (!statests)
    {
        SerialPuts("intel_hda: no codecs detected (STATESTS=0)\n");
        return false;
    }

    // Find first codec
    for (int i = 0; i < 15; i++)
    {
        if (statests & (1 << i))
        {
            g_codecAddr = i;
            break;
        }
    }

    // Clear status
    hda_write16(STATESTS, statests);

    // Get vendor ID
    uint32_t vendorId = 0;
    if (HdaCommand(HdaVerb(g_codecAddr, 0, VERB_GET_PARAM, PARAM_VENDOR_ID), vendorId))
    {
        KPrintf("intel_hda: codec %d vendor=%04x device=%04x\n",
                g_codecAddr, vendorId >> 16, vendorId & 0xFFFF);
    }

    return true;
}

static bool EnumerateWidgets()
{
    // Get root node's sub-node count
    uint32_t response;
    if (!HdaCommand(HdaVerb(g_codecAddr, 0, VERB_GET_PARAM, PARAM_SUB_NODE_COUNT), response))
        return false;

    uint8_t startNid = (response >> 16) & 0xFF;
    uint8_t count    = response & 0xFF;

    SerialPrintf("intel_hda: root node: start=%d count=%d\n", startNid, count);

    // Find audio function group (type 1)
    uint8_t afg = 0;
    for (uint8_t i = 0; i < count; i++)
    {
        uint8_t nid = startNid + i;
        if (!HdaCommand(HdaVerb(g_codecAddr, nid, VERB_GET_PARAM, PARAM_FN_GROUP_TYPE), response))
            continue;

        uint8_t groupType = response & 0xFF;
        if (groupType == 1)
        {
            afg = nid;
            SerialPrintf("intel_hda: audio function group at NID %d\n", afg);
            break;
        }
    }

    if (!afg)
    {
        SerialPuts("intel_hda: no audio function group found\n");
        return false;
    }

    // Power up the AFG
    HdaCommandNoResp(HdaVerb(g_codecAddr, afg, VERB_SET_POWER_STATE, 0x00));
    BusyWait(10000);

    // Get AFG's sub-nodes (widgets)
    if (!HdaCommand(HdaVerb(g_codecAddr, afg, VERB_GET_PARAM, PARAM_SUB_NODE_COUNT), response))
        return false;

    startNid = (response >> 16) & 0xFF;
    count    = response & 0xFF;

    SerialPrintf("intel_hda: AFG widgets: start=%d count=%d\n", startNid, count);

    // Scan widgets for DAC and output pin
    for (uint8_t i = 0; i < count; i++)
    {
        uint8_t nid = startNid + i;
        if (!HdaCommand(HdaVerb(g_codecAddr, nid, VERB_GET_PARAM, PARAM_WIDGET_CAPS), response))
            continue;

        uint8_t wtype = (response >> 20) & 0xF;

        if (wtype == WT_AUDIO_OUTPUT && g_dacNid == 0)
        {
            g_dacNid = nid;
            SerialPrintf("intel_hda: DAC at NID %d\n", nid);
        }
        else if (wtype == WT_PIN_COMPLEX && g_pinNid == 0)
        {
            // Check if this is an output pin (default config)
            uint32_t pinCfg;
            if (HdaCommand(HdaVerb(g_codecAddr, nid, VERB_GET_PARAM, 0x1C), pinCfg))
            {
                // Default association in bits 7:4, sequence in bits 3:0
                // Check default device type in bits 23:20
                uint8_t defaultDevice = (pinCfg >> 20) & 0xF;
                // 0=Line Out, 1=Speaker, 2=HP Out
                if (defaultDevice <= 2)
                {
                    g_pinNid = nid;
                    SerialPrintf("intel_hda: output pin at NID %d (type=%d)\n",
                                nid, defaultDevice);
                }
            }
        }
    }

    if (!g_dacNid)
    {
        SerialPuts("intel_hda: no DAC widget found\n");
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Configure output path: Pin → [Mixer] → DAC
// ---------------------------------------------------------------------------

static bool ConfigureOutputPath()
{
    // Power up DAC
    HdaCommandNoResp(HdaVerb(g_codecAddr, g_dacNid, VERB_SET_POWER_STATE, 0x00));

    // Enable output pin
    if (g_pinNid)
    {
        // Power up pin
        HdaCommandNoResp(HdaVerb(g_codecAddr, g_pinNid, VERB_SET_POWER_STATE, 0x00));
        // Set pin control: output enable (bit 6)
        HdaCommandNoResp(HdaVerb(g_codecAddr, g_pinNid, VERB_SET_PIN_CTRL, 0x40));
        // Enable EAPD if available
        HdaCommandNoResp(HdaVerb(g_codecAddr, g_pinNid, VERB_SET_EAPDBTL, 0x02));
    }

    // Set DAC output amplifier gain (unmute, 0dB)
    // Set output amp: left+right, output, gain=0x40 (varies by codec)
    HdaCommandNoResp(HdaVerb4(g_codecAddr, g_dacNid, VERB_SET_AMP_GAIN,
                               0xB040)); // output, L+R, gain=0x40

    // Set pin output amp if it has one
    if (g_pinNid)
    {
        HdaCommandNoResp(HdaVerb4(g_codecAddr, g_pinNid, VERB_SET_AMP_GAIN,
                                   0xB040));
    }

    return true;
}

// ---------------------------------------------------------------------------
// Stream format encoding (Intel HDA spec §3.7.1)
// ---------------------------------------------------------------------------

static uint16_t EncodeStreamFormat(uint32_t sampleRate, uint8_t channels, uint8_t bits)
{
    uint16_t fmt = 0;

    // Sample rate base + multiplier/divisor
    // Base rate: bit 14: 0=48kHz, 1=44.1kHz
    // Multiplier: bits 13:11 (0=x1, 1=x2, 2=x3, 3=x4)
    // Divisor: bits 10:8 (0=/1, 1=/2, ..., 6=/7, 7=/8)
    switch (sampleRate) {
        case 8000:  fmt = (0 << 14) | (0 << 11) | (5 << 8); break; // 48k / 6
        case 11025: fmt = (1 << 14) | (0 << 11) | (3 << 8); break; // 44.1k / 4
        case 16000: fmt = (0 << 14) | (0 << 11) | (2 << 8); break; // 48k / 3
        case 22050: fmt = (1 << 14) | (0 << 11) | (1 << 8); break; // 44.1k / 2
        case 32000: fmt = (0 << 14) | (1 << 11) | (2 << 8); break; // 48k * 2 / 3
        case 44100: fmt = (1 << 14) | (0 << 11) | (0 << 8); break; // 44.1k
        case 48000: fmt = (0 << 14) | (0 << 11) | (0 << 8); break; // 48k
        case 96000: fmt = (0 << 14) | (1 << 11) | (0 << 8); break; // 48k * 2
        default:    fmt = (0 << 14) | (0 << 11) | (0 << 8); break; // default 48k
    }

    // Bits per sample: bits 6:4
    switch (bits) {
        case 8:  fmt |= (0 << 4); break;
        case 16: fmt |= (1 << 4); break;
        case 20: fmt |= (2 << 4); break;
        case 24: fmt |= (3 << 4); break;
        case 32: fmt |= (4 << 4); break;
        default: fmt |= (1 << 4); break; // default 16-bit
    }

    // Channels: bits 3:0 (value = channels - 1)
    fmt |= (channels - 1) & 0xF;

    return fmt;
}

// ---------------------------------------------------------------------------
// Output stream setup
// ---------------------------------------------------------------------------

static bool SetupOutputStream(uint32_t sampleRate, uint8_t channels, uint8_t bits)
{
    // Output stream 0 is at SD_BASE + numInputStreams * 0x20
    uint32_t sdBase = g_outStreamBase;
    uint8_t streamTag = 1; // Stream tag 1 (0 is reserved)

    // Reset stream
    hda_write32(sdBase + SD_CTL, SD_CTL_SRST);
    BusyWait(10000);

    // Wait for reset
    for (int i = 0; i < 100; i++)
    {
        if (hda_read32(sdBase + SD_CTL) & SD_CTL_SRST) break;
        BusyWait(1000);
    }

    // Clear reset
    hda_write32(sdBase + SD_CTL, 0);
    BusyWait(10000);

    for (int i = 0; i < 100; i++)
    {
        if (!(hda_read32(sdBase + SD_CTL) & SD_CTL_SRST)) break;
        BusyWait(1000);
    }

    // Clear status
    hda_write8(sdBase + SD_STS, SD_STS_BCIS | SD_STS_FIFOE | SD_STS_DESE);

    // Set stream format
    uint16_t fmt = EncodeStreamFormat(sampleRate, channels, bits);
    hda_write16(sdBase + SD_FMT, fmt);

    // Configure DAC: set stream tag and channel
    // Verb: Set Converter Stream, Channel (bits 7:4 = stream, bits 3:0 = channel)
    HdaCommandNoResp(HdaVerb(g_codecAddr, g_dacNid, VERB_SET_CONV_STREAM,
                              (streamTag << 4) | 0));

    // Set DAC format to match stream
    HdaCommandNoResp(HdaVerb4(g_codecAddr, g_dacNid, VERB_SET_CONV_FMT, fmt));

    // Set up BDL — single entry pointing to audio buffer
    g_bdl[0].addr   = g_audioBufPhys;
    g_bdl[0].length = AUDIO_BUF_SIZE;
    g_bdl[0].ioc    = 1;

    // Set BDL address
    hda_write32(sdBase + SD_BDLPL, static_cast<uint32_t>(g_bdlPhys));
    hda_write32(sdBase + SD_BDLPU, static_cast<uint32_t>(g_bdlPhys >> 32));

    // Set cyclic buffer length
    hda_write32(sdBase + SD_CBL, AUDIO_BUF_SIZE);

    // Set last valid index (0 = 1 entry)
    hda_write16(sdBase + SD_LVI, 0);

    // Set stream control: stream tag in bits 23:20, run, IOC enable
    uint32_t ctl = (static_cast<uint32_t>(streamTag) << 20) | SD_CTL_IOCE | SD_CTL_RUN;
    hda_write32(sdBase + SD_CTL, ctl);

    return true;
}

static void StopOutputStream()
{
    uint32_t sdBase = g_outStreamBase;
    uint32_t ctl = hda_read32(sdBase + SD_CTL);
    ctl &= ~SD_CTL_RUN;
    hda_write32(sdBase + SD_CTL, ctl);
    BusyWait(10000);
}

// ---------------------------------------------------------------------------
// Public API: play PCM audio
// ---------------------------------------------------------------------------

// Play PCM samples through the HDA output.
// samples: pointer to PCM data (signed 16-bit interleaved)
// byteCount: total bytes of PCM data
// sampleRate: e.g. 44100, 48000
// channels: 1=mono, 2=stereo
// bitsPerSample: 8, 16, 24, 32
extern "C" int HdaPlayPcm(const void* samples, uint32_t byteCount,
                           uint32_t sampleRate, uint8_t channels,
                           uint8_t bitsPerSample)
{
    if (!g_initialized || !g_dacNid) return -1;
    if (byteCount > AUDIO_BUF_SIZE) byteCount = AUDIO_BUF_SIZE;

    // Copy audio data to DMA buffer
    const uint8_t* src = static_cast<const uint8_t*>(samples);
    for (uint32_t i = 0; i < byteCount; i++)
        g_audioBuf[i] = src[i];

    // Zero remaining buffer
    for (uint32_t i = byteCount; i < AUDIO_BUF_SIZE; i++)
        g_audioBuf[i] = 0;

    // Update BDL entry length
    g_bdl[0].length = byteCount;
    hda_write32(g_outStreamBase + SD_CBL, byteCount);

    // Configure and start stream
    SetupOutputStream(sampleRate, channels, bitsPerSample);

    return static_cast<int>(byteCount);
}

// Stop audio playback
extern "C" void HdaStop()
{
    if (!g_initialized) return;
    StopOutputStream();
}

// Check if audio is currently playing
extern "C" bool HdaIsPlaying()
{
    if (!g_initialized) return false;
    uint32_t ctl = hda_read32(g_outStreamBase + SD_CTL);
    return (ctl & SD_CTL_RUN) != 0;
}

// Get current playback position in bytes
extern "C" uint32_t HdaGetPosition()
{
    if (!g_initialized) return 0;
    return hda_read32(g_outStreamBase + SD_LPIB);
}

// ---------------------------------------------------------------------------
// Module init / exit
// ---------------------------------------------------------------------------

static int IntelHdaInit()
{
    PciDevice dev;
    // ICH9 HDA: vendor 0x8086, device 0x293E
    if (!PciFindDevice(0x8086, 0x293E, dev))
    {
        // Try ICH10
        if (!PciFindDevice(0x8086, 0x3A3E, dev))
        {
            SerialPuts("intel_hda: PCI device not found\n");
            return -1;
        }
    }

    KPrintf("intel_hda: found at PCI %02x:%02x.%x\n",
            dev.bus, dev.dev, dev.fn);

    PciEnableMemSpace(dev);
    PciEnableBusMaster(dev);

    // Get BAR0 (MMIO)
    uint32_t bar0 = dev.bar[0] & ~0xFu;
    if (!bar0)
    {
        SerialPuts("intel_hda: BAR0 is 0\n");
        return -1;
    }

    // Map MMIO pages (HDA registers span ~16KB typically)
    uint64_t mmioAddr = bar0;
    uint32_t mmioPages = 4; // 16KB
    for (uint32_t p = 0; p < mmioPages; p++)
    {
        VmmMapPage(KernelPageTable,
                   VirtualAddress(mmioAddr + p * 4096),
                   PhysicalAddress(mmioAddr + p * 4096),
                   VMM_WRITABLE | VMM_CACHE_DISABLE);
    }
    g_mmioBase = reinterpret_cast<volatile uint8_t*>(mmioAddr);

    // Read capabilities
    uint16_t gcap = hda_read16(GCAP);
    g_numInputStreams  = (gcap >> 8) & 0xF;
    g_numOutputStreams = (gcap >> 12) & 0xF;
    uint8_t vmaj = hda_read8(VMAJ);
    uint8_t vmin = hda_read8(VMIN);

    KPrintf("intel_hda: HDA v%d.%d, ISS=%d OSS=%d\n",
            vmaj, vmin, g_numInputStreams, g_numOutputStreams);

    // Output stream 0 descriptor base
    g_outStreamBase = SD_BASE + g_numInputStreams * 0x20;

    // Reset controller
    if (!HdaControllerReset())
        return -1;

    // Setup CORB/RIRB
    if (!SetupCorbRirb())
        return -1;

    // Discover codecs
    if (!DiscoverCodec())
        return -1;

    // Enumerate widgets
    if (!EnumerateWidgets())
    {
        SerialPuts("intel_hda: widget enumeration failed (non-fatal)\n");
        // Continue — we may still work with minimal config
    }

    // Configure output path
    ConfigureOutputPath();

    // Allocate BDL
    auto bdlPage = VmmAllocPages(1, VMM_WRITABLE | VMM_CACHE_DISABLE, MemTag::Device, KernelPid);
    if (!bdlPage)
    {
        SerialPuts("intel_hda: BDL alloc failed\n");
        return -1;
    }
    g_bdl = reinterpret_cast<BdlEntry*>(bdlPage.raw());
    g_bdlPhys = VmmVirtToPhys(KernelPageTable, bdlPage).raw();
    for (int i = 0; i < 4096 / 4; i++)
        reinterpret_cast<uint32_t*>(g_bdl)[i] = 0;

    // Allocate audio buffer (16 pages = 64KB)
    uint32_t audioPages = AUDIO_BUF_SIZE / 4096;
    auto audioBufAddr = VmmAllocPages(audioPages, VMM_WRITABLE, MemTag::Device, KernelPid);
    if (!audioBufAddr)
    {
        SerialPuts("intel_hda: audio buffer alloc failed\n");
        return -1;
    }
    g_audioBuf = reinterpret_cast<uint8_t*>(audioBufAddr.raw());
    g_audioBufPhys = VmmVirtToPhys(KernelPageTable, audioBufAddr).raw();

    // Enable interrupts (global + controller + stream 0)
    hda_write32(INTCTL, INTCTL_GIE | INTCTL_CIE |
                         (1 << (g_numInputStreams)));

    g_initialized = true;

    KPrintf("intel_hda: initialised — DAC=%d PIN=%d\n", g_dacNid, g_pinNid);
    return 0;
}

static void IntelHdaExit()
{
    if (g_initialized)
    {
        StopOutputStream();
        // Disable interrupts
        hda_write32(INTCTL, 0);
        // Stop CORB/RIRB
        hda_write8(CORBCTL, 0);
        hda_write8(RIRBCTL, 0);
    }
    g_initialized = false;
    SerialPuts("intel_hda: unloaded\n");
}

DECLARE_MODULE("intel_hda", IntelHdaInit, IntelHdaExit,
               "Intel HD Audio controller driver (ICH9)");
