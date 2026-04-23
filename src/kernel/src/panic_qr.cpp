// QR code panic screen — renders CPU state as a scannable QR code.
//
// Pipeline: PanicCPURegs → binary buffer → Base45 → Nayuki QR → framebuffer

#include "panic_qr.h"
#include "serial.h"

extern "C" {
#include "qrcodegen.h"
}

namespace brook {

// Assemble the binary panic packet: header + CPU regs + stack trace + optional extras
static uint32_t BuildPanicPacket(uint8_t* buf, uint32_t bufLen,
                                 const PanicCPURegs* regs,
                                 const PanicStackTrace* trace,
                                 const PanicExceptionInfo* excInfo,
                                 const PanicProcessList* procList)
{
    uint32_t tracePayloadSize = 1 + trace->depth * 8; // depth byte + RIPs

    uint32_t needed = sizeof(PanicHeader)
                    + sizeof(PanicPacketHeader) + sizeof(PanicCPURegs)
                    + sizeof(PanicPacketHeader) + tracePayloadSize;
    if (excInfo) needed += sizeof(PanicPacketHeader) + sizeof(PanicExceptionInfo);
    if (procList) needed += sizeof(PanicPacketHeader) + 1 + procList->count * sizeof(PanicProcessEntry);

    if (bufLen < needed) return 0;

    uint32_t off = 0;
    auto appendRaw = [&](const void* data, uint32_t size) {
        auto* p = static_cast<const uint8_t*>(data);
        for (uint32_t i = 0; i < size; ++i) buf[off++] = p[i];
    };

    // Panic header
    PanicHeader hdr;
    hdr.magic     = QR_MAGIC_BYTE;
    hdr.version   = QR_VERSION;
    hdr.page      = 0;
    hdr.pageCount = 1;
    hdr.pad       = QR_HEADER_PAD;
    appendRaw(&hdr, sizeof(hdr));

    // Packet 1: CPU registers
    PanicPacketHeader ph;
    ph.type = QR_PACKET_TYPE_CPU_REGS;
    ph.size = sizeof(PanicCPURegs);
    appendRaw(&ph, sizeof(ph));
    appendRaw(regs, sizeof(PanicCPURegs));

    // Packet 2: Stack trace
    ph.type = QR_PACKET_TYPE_STACK_TRACE;
    ph.size = tracePayloadSize;
    appendRaw(&ph, sizeof(ph));
    buf[off++] = trace->depth;
    for (uint8_t d = 0; d < trace->depth; d++)
        appendRaw(&trace->rip[d], 8);

    // Packet 3: Exception info (optional)
    if (excInfo)
    {
        ph.type = QR_PACKET_TYPE_EXCEPTION_INFO;
        ph.size = sizeof(PanicExceptionInfo);
        appendRaw(&ph, sizeof(ph));
        appendRaw(excInfo, sizeof(PanicExceptionInfo));
    }

    // Packet 4: Process list (optional)
    if (procList && procList->count > 0)
    {
        uint32_t plSize = 1 + procList->count * sizeof(PanicProcessEntry);
        ph.type = QR_PACKET_TYPE_PROCESS_LIST;
        ph.size = plSize;
        appendRaw(&ph, sizeof(ph));
        buf[off++] = procList->count;
        for (uint8_t i = 0; i < procList->count; i++)
            appendRaw(&procList->entries[i], sizeof(PanicProcessEntry));
    }

    return off;
}

// Render QR code to framebuffer — positioned in the right portion of the screen.
static void RenderQRToFramebuffer(uint32_t* fb, uint32_t fbWidth, uint32_t fbHeight,
                                   uint32_t fbStride, const uint8_t* qrcode)
{
    int size = qrcodegen_getSize(qrcode);
    uint32_t strideQuads = fbStride / 4;

    const uint32_t black = 0x00000000;
    const uint32_t white = 0xFFFFFFFF - (QR_CONTRAST * 0x11111111);

    // Position the QR in the right column, vertically centred
    uint32_t qrPixelSize = static_cast<uint32_t>(size + 2 * QR_BORDER_WIDTH) * QR_PIXELS_PER_MODULE;
    uint32_t startX = fbWidth > qrPixelSize + 40
                      ? fbWidth * 55 / 100 + (fbWidth * 45 / 100 - qrPixelSize) / 2
                      : QR_START_X;
    uint32_t startY = fbHeight > qrPixelSize + 100
                      ? (fbHeight - qrPixelSize) / 2
                      : QR_START_Y;

    for (int y = -QR_BORDER_WIDTH; y < size + QR_BORDER_WIDTH; ++y)
    {
        for (int x = -QR_BORDER_WIDTH; x < size + QR_BORDER_WIDTH; ++x)
        {
            bool valid = (x >= 0 && x < size && y >= 0 && y < size);
            bool set = valid && qrcodegen_getModule(qrcode, x, y);
            uint32_t colour = set ? black : white;

            int posX = x * static_cast<int>(QR_PIXELS_PER_MODULE) + QR_BORDER_WIDTH;
            int posY = y * static_cast<int>(QR_PIXELS_PER_MODULE) + QR_BORDER_WIDTH;

            for (uint32_t my = 0; my < QR_PIXELS_PER_MODULE; ++my)
            {
                for (uint32_t mx = 0; mx < QR_PIXELS_PER_MODULE; ++mx)
                {
                    uint32_t px = startX + posX + mx;
                    uint32_t py = startY + posY + my;
                    if (px < fbWidth && py < fbHeight)
                        fb[py * strideQuads + px] = colour;
                }
            }
        }
    }
}

void PanicRenderQR(uint32_t* fbBase, uint32_t fbWidth, uint32_t fbHeight,
                   uint32_t fbStride, const PanicCPURegs* regs,
                   const PanicStackTrace* trace,
                   const PanicExceptionInfo* excInfo,
                   const PanicProcessList* procList)
{
    // Step 1: Build binary panic packet
    static uint8_t packetBuf[1024];
    uint32_t packetLen = BuildPanicPacket(packetBuf, sizeof(packetBuf),
                                          regs, trace, excInfo, procList);
    if (packetLen == 0)
    {
        SerialPuts("PANIC QR: packet build failed\n");
        return;
    }

    SerialPrintf("PANIC QR: packet %u bytes\n", packetLen);

    // Step 2: Generate QR code using binary mode (no Base45 overhead)
    // encodeBinary uses dataAndTemp as both input AND scratch space,
    // so we copy the packet data into the temp buffer first.
    static uint8_t qrBuf[qrcodegen_BUFFER_LEN_MAX];
    static uint8_t tempBuf[qrcodegen_BUFFER_LEN_MAX];

    for (uint32_t i = 0; i < packetLen; ++i)
        tempBuf[i] = packetBuf[i];

    SerialPuts("PANIC QR: calling qrcodegen_encodeBinary...\n");

    bool ok = qrcodegen_encodeBinary(tempBuf, packetLen, qrBuf,
                                      qrcodegen_Ecc_LOW,
                                      qrcodegen_VERSION_MIN,
                                      qrcodegen_VERSION_MAX,
                                      qrcodegen_Mask_AUTO, true);

    SerialPrintf("PANIC QR: encodeBinary returned %d\n", ok ? 1 : 0);
    if (!ok)
    {
        SerialPuts("PANIC QR: QR generation failed\n");
        return;
    }

    int qrSize = qrcodegen_getSize(qrBuf);
    SerialPrintf("PANIC QR: QR version size %d modules\n", qrSize);

    // Step 4: Render to framebuffer
    RenderQRToFramebuffer(fbBase, fbWidth, fbHeight, fbStride, qrBuf);
    SerialPuts("PANIC QR: rendered to framebuffer\n");
}

} // namespace brook
