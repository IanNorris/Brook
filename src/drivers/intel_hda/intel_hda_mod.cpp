// intel_hda_mod.cpp — Intel High Definition Audio driver for ICH9 (QEMU q35)
//
// Implements:
//   - Controller reset and Immediate Command Interface (ICI) for codec verbs
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
#include "audio.h"
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
MODULE_IMPORT_SYMBOL(AudioRegister);

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
    CORBSTS     = 0x4D,  // CORB status (8-bit)
    CORBSIZE    = 0x4E,  // CORB size (8-bit)

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

    // Immediate Command Interface (ICI) registers
    ICW         = 0x60,  // Immediate Command Write (32-bit)
    IRR         = 0x64,  // Immediate Response Result (32-bit)
    ICS         = 0x68,  // Immediate Command Status (16-bit)
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

// INTCTL bits
static constexpr uint32_t INTCTL_GIE = (1u << 31);  // Global interrupt enable
static constexpr uint32_t INTCTL_CIE = (1u << 30);  // Controller interrupt enable

// ICS (Immediate Command Status) bits
static constexpr uint16_t ICS_ICB = (1 << 0);  // Immediate Command Busy
static constexpr uint16_t ICS_IRV = (1 << 1);  // Immediate Response Valid

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
// Buffer Descriptor List for output stream
// ---------------------------------------------------------------------------

struct __attribute__((packed)) BdlEntry {
    uint64_t addr;   // Physical address of buffer
    uint32_t length; // Length in bytes
    uint32_t ioc;    // Interrupt on completion (bit 0)
};

static BdlEntry* g_bdl = nullptr;
static uint64_t  g_bdlPhys = 0;

// Audio buffer — split into two halves for double-buffering
static constexpr uint32_t AUDIO_BUF_SIZE = 64 * 1024; // 64KB total
static constexpr uint32_t HALF_BUF_SIZE  = AUDIO_BUF_SIZE / 2; // 32KB per half
static uint8_t*  g_audioBuf = nullptr;
static uint64_t  g_audioBufPhys = 0;

// Double-buffer state
static uint32_t  g_writeHalf = 0;      // which half (0 or 1) to write next
static uint32_t  g_halfFilled[2] = {}; // bytes of real data in each half
static bool      g_streamRunning = false;

// Output stream descriptor offset (depends on GCAP)
static uint32_t g_outStreamBase = 0;
static uint8_t  g_numInputStreams = 0;
static uint8_t  g_numOutputStreams = 0;

// Discovered codec/widget info
static uint8_t g_codecAddr = 0;
static uint8_t g_dacNid = 0;       // DAC widget NID
static uint8_t g_pinNid = 0;       // Output pin widget NID
static bool    g_initialized = false;

// Track current stream format to avoid redundant reconfiguration
static uint32_t g_curRate = 0;
static uint8_t  g_curChannels = 0;
static uint8_t  g_curBits = 0;
static bool     g_streamConfigured = false;

// ---------------------------------------------------------------------------
// Busy-wait helper
// ---------------------------------------------------------------------------

static void BusyWait(uint32_t iters)
{
    for (volatile uint32_t i = 0; i < iters; i++)
        __asm__ volatile("pause");
}

// ---------------------------------------------------------------------------
// Immediate Command Interface (ICI) — MMIO-based, no DMA needed
// ---------------------------------------------------------------------------

static bool HdaCommand(uint32_t verb, uint32_t& response)
{
    // Wait for any previous command to complete
    for (int i = 0; i < 10000; i++)
    {
        if (!(hda_read16(ICS) & ICS_ICB)) goto ready;
        __asm__ volatile("pause");
    }
    SerialPrintf("intel_hda: ICI busy timeout (verb=0x%08x)\n", verb);
    return false;

ready:
    // Clear IRV by writing 1
    hda_write16(ICS, ICS_IRV);

    // Write verb
    hda_write32(ICW, verb);

    // Set ICB to start command
    hda_write16(ICS, ICS_ICB);

    // Wait for completion (ICB clears when done)
    for (int i = 0; i < 100000; i++)
    {
        uint16_t sts = hda_read16(ICS);
        if (!(sts & ICS_ICB))
        {
            if (sts & ICS_IRV)
            {
                response = hda_read32(IRR);
                return true;
            }
            // ICB cleared but no response — error
            return false;
        }
        __asm__ volatile("pause");
    }

    SerialPrintf("intel_hda: ICI timeout (verb=0x%08x)\n", verb);
    return false;
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
    else
    {
        SerialPrintf("intel_hda: codec %d — vendor ID query failed (CORB/RIRB timeout)\n",
                     g_codecAddr);
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
            // Get default pin configuration (verb 0xF1C)
            uint32_t pinCfg;
            if (HdaCommand(HdaVerb(g_codecAddr, nid, 0xF1C, 0x00), pinCfg))
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
    uint32_t sdBase = g_outStreamBase;
    uint8_t streamTag = 1;

    // Reset stream
    hda_write32(sdBase + SD_CTL, SD_CTL_SRST);
    BusyWait(10000);
    for (int i = 0; i < 100; i++)
    {
        if (hda_read32(sdBase + SD_CTL) & SD_CTL_SRST) break;
        BusyWait(1000);
    }
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
    HdaCommandNoResp(HdaVerb(g_codecAddr, g_dacNid, VERB_SET_CONV_STREAM,
                              (streamTag << 4) | 0));
    HdaCommandNoResp(HdaVerb4(g_codecAddr, g_dacNid, VERB_SET_CONV_FMT, fmt));

    // Set BDL address
    hda_write32(sdBase + SD_BDLPL, static_cast<uint32_t>(g_bdlPhys));
    hda_write32(sdBase + SD_BDLPU, static_cast<uint32_t>(g_bdlPhys >> 32));

    // Two BDL entries: half A and half B, both with IOC
    g_bdl[0].addr   = g_audioBufPhys;
    g_bdl[0].length = HALF_BUF_SIZE;
    g_bdl[0].ioc    = 1;
    g_bdl[1].addr   = g_audioBufPhys + HALF_BUF_SIZE;
    g_bdl[1].length = HALF_BUF_SIZE;
    g_bdl[1].ioc    = 1;

    // LVI = 1 (two entries: 0 and 1), CBL = total size
    hda_write16(sdBase + SD_LVI, 1);
    hda_write32(sdBase + SD_CBL, AUDIO_BUF_SIZE);

    // Zero both halves
    for (uint32_t i = 0; i < AUDIO_BUF_SIZE; i++)
        g_audioBuf[i] = 0;
    g_halfFilled[0] = 0;
    g_halfFilled[1] = 0;
    g_writeHalf = 0;

    // Record current format
    g_curRate = sampleRate;
    g_curChannels = channels;
    g_curBits = bits;
    g_streamConfigured = true;
    g_streamRunning = false;

    return true;
}

// Start the stream (after setup or buffer update)
static void StartOutputStream()
{
    uint32_t sdBase = g_outStreamBase;
    uint8_t streamTag = 1;
    uint32_t ctl = (static_cast<uint32_t>(streamTag) << 20) | SD_CTL_IOCE | SD_CTL_RUN;
    hda_write32(sdBase + SD_CTL, ctl);
}

static void StopOutputStream()
{
    uint32_t sdBase = g_outStreamBase;
    uint32_t ctl = hda_read32(sdBase + SD_CTL);
    ctl &= ~SD_CTL_RUN;
    hda_write32(sdBase + SD_CTL, ctl);
    BusyWait(1000);
}

// ---------------------------------------------------------------------------
// Public API: play PCM audio
// ---------------------------------------------------------------------------

// Double-buffer write cursor: offset within the current write half
static uint32_t  g_writeOffset = 0;

// Play PCM samples through the HDA output using double-buffered DMA.
// Two BDL entries with IOC — DMA plays A→B→A→B automatically.
// BCIS fires when each entry completes, signaling us to refill it.
extern "C" int HdaPlayPcm(const void* samples, uint32_t byteCount,
                           uint32_t sampleRate, uint8_t channels,
                           uint8_t bitsPerSample)
{
    if (!g_initialized || !g_dacNid) return -1;

    uint32_t sdBase = g_outStreamBase;

    // Full stream setup on first call or format change
    bool needSetup = !g_streamConfigured ||
                     sampleRate != g_curRate ||
                     channels != g_curChannels ||
                     bitsPerSample != g_curBits;

    if (needSetup)
    {
        if (g_streamRunning) StopOutputStream();
        g_streamRunning = false;
        SetupOutputStream(sampleRate, channels, bitsPerSample);
        g_writeOffset = 0;
        g_writeHalf = 0;
    }

    const uint8_t* src = static_cast<const uint8_t*>(samples);
    uint32_t totalWritten = 0;

    while (totalWritten < byteCount)
    {
        uint8_t* dst = g_audioBuf + (g_writeHalf * HALF_BUF_SIZE);

        // Fill remainder of current half
        uint32_t space = HALF_BUF_SIZE - g_writeOffset;
        uint32_t chunk = byteCount - totalWritten;
        if (chunk > space) chunk = space;

        for (uint32_t i = 0; i < chunk; i++)
            dst[g_writeOffset + i] = src[totalWritten + i];
        g_writeOffset += chunk;
        totalWritten += chunk;

        // Half is full — switch to the other half
        if (g_writeOffset >= HALF_BUF_SIZE)
        {
            if (!g_streamRunning)
            {
                // First half filled — zero the other half and start DMA
                uint8_t* other = g_audioBuf + ((1 - g_writeHalf) * HALF_BUF_SIZE);
                for (uint32_t i = 0; i < HALF_BUF_SIZE; i++)
                    other[i] = 0;
                StartOutputStream();
                g_streamRunning = true;
            }
            else
            {
                // Wait for DMA to finish the half we're about to write.
                // Clear BCIS, then wait for it — means one BDL entry completed.
                hda_write8(sdBase + SD_STS, SD_STS_BCIS);
                for (int i = 0; i < 50000000; i++)
                {
                    if (hda_read8(sdBase + SD_STS) & SD_STS_BCIS) break;
                    __asm__ volatile("pause");
                }
            }

            g_writeHalf = 1 - g_writeHalf;
            g_writeOffset = 0;
        }
    }

    return static_cast<int>(totalWritten);
}

// Stop audio playback
extern "C" void HdaStop()
{
    if (!g_initialized) return;
    StopOutputStream();
    g_streamRunning = false;
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

    // Map MMIO pages into kernel virtual address space
    uint64_t mmioPhys = bar0;
    uint32_t mmioPages = 4; // 16KB
    auto mmioVaddr = VmmAllocPages(mmioPages, VMM_WRITABLE | VMM_CACHE_DISABLE,
                                    MemTag::Device, KernelPid);
    if (!mmioVaddr)
    {
        SerialPuts("intel_hda: MMIO alloc failed\n");
        return -1;
    }
    for (uint32_t p = 0; p < mmioPages; p++)
    {
        VmmMapPage(KernelPageTable,
                   VirtualAddress(mmioVaddr.raw() + p * 4096),
                   PhysicalAddress(mmioPhys + p * 4096),
                   VMM_WRITABLE | VMM_CACHE_DISABLE,
                   MemTag::Device, KernelPid);
    }
    g_mmioBase = reinterpret_cast<volatile uint8_t*>(mmioVaddr.raw());

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

    // Discover codecs
    if (!DiscoverCodec())
        return -1;

    // Enumerate widgets (uses ICI — no DMA needed)
    if (!EnumerateWidgets())
    {
        SerialPuts("intel_hda: widget enumeration failed\n");
        return -1;
    }

    // Configure output path
    ConfigureOutputPath();

    // Allocate BDL (uncacheable for DMA)
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

    // Allocate audio buffer (16 pages = 64KB, uncacheable for DMA)
    uint32_t audioPages = AUDIO_BUF_SIZE / 4096;
    auto audioBufAddr = VmmAllocPages(audioPages, VMM_WRITABLE | VMM_CACHE_DISABLE,
                                       MemTag::Device, KernelPid);
    if (!audioBufAddr)
    {
        SerialPuts("intel_hda: audio buffer alloc failed\n");
        return -1;
    }
    g_audioBuf = reinterpret_cast<uint8_t*>(audioBufAddr.raw());
    g_audioBufPhys = VmmVirtToPhys(KernelPageTable, audioBufAddr).raw();

    SerialPrintf("intel_hda: DMA BDL phys=0x%lx audio phys=0x%lx..0x%lx (%u pages)\n",
                 g_bdlPhys, g_audioBufPhys,
                 g_audioBufPhys + AUDIO_BUF_SIZE - 1, audioPages);

    // Enable interrupts (global + controller + output stream 0)
    hda_write32(INTCTL, INTCTL_GIE | INTCTL_CIE |
                         (1 << (g_numInputStreams)));

    g_initialized = true;

    // Register with the audio subsystem
    static const brook::AudioDriver hdaAudioDriver = {
        "intel_hda",
        HdaPlayPcm,
        HdaStop,
        HdaIsPlaying,
        HdaGetPosition,
    };
    AudioRegister(&hdaAudioDriver);

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
    }
    g_initialized = false;
    SerialPuts("intel_hda: unloaded\n");
}

DECLARE_MODULE("intel_hda", IntelHdaInit, IntelHdaExit,
               "Intel HD Audio controller driver (ICH9)");
