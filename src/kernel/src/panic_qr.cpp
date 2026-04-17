// QR code panic screen — renders CPU state as a scannable QR code.
//
// Pipeline: PanicCPURegs → binary buffer → Base45 → Nayuki QR → framebuffer

#include "panic_qr.h"
#include "base45.h"
#include "serial.h"

extern "C" {
#include "qrcodegen.h"
}

namespace brook {

// Assemble the binary panic packet: header + CPU regs packet + stack trace packet
static uint32_t BuildPanicPacket(uint8_t* buf, uint32_t bufLen,
                                 const PanicCPURegs* regs,
                                 const PanicStackTrace* trace)
{
    // Calculate total size: header + (regs packet) + (stack trace packet)
    uint32_t tracePayloadSize = 1 + trace->depth * 8; // depth byte + RIPs
    uint32_t needed = sizeof(PanicHeader)
                    + sizeof(PanicPacketHeader) + sizeof(PanicCPURegs)
                    + sizeof(PanicPacketHeader) + tracePayloadSize;
    if (bufLen < needed) return 0;

    uint32_t off = 0;

    // Panic header
    PanicHeader hdr;
    hdr.magic     = QR_MAGIC_BYTE;
    hdr.version   = QR_VERSION;
    hdr.page      = 0;
    hdr.pageCount = 1;
    hdr.pad       = QR_HEADER_PAD;

    auto* p = reinterpret_cast<const uint8_t*>(&hdr);
    for (uint32_t i = 0; i < sizeof(hdr); ++i) buf[off++] = p[i];

    // Packet 1: CPU registers
    PanicPacketHeader ph;
    ph.type = QR_PACKET_TYPE_CPU_REGS;
    ph.size = sizeof(PanicCPURegs);

    p = reinterpret_cast<const uint8_t*>(&ph);
    for (uint32_t i = 0; i < sizeof(ph); ++i) buf[off++] = p[i];

    p = reinterpret_cast<const uint8_t*>(regs);
    for (uint32_t i = 0; i < sizeof(PanicCPURegs); ++i) buf[off++] = p[i];

    // Packet 2: Stack trace
    PanicPacketHeader ph2;
    ph2.type = QR_PACKET_TYPE_STACK_TRACE;
    ph2.size = tracePayloadSize;

    p = reinterpret_cast<const uint8_t*>(&ph2);
    for (uint32_t i = 0; i < sizeof(ph2); ++i) buf[off++] = p[i];

    // depth byte
    buf[off++] = trace->depth;

    // RIP values (only the valid ones)
    for (uint8_t d = 0; d < trace->depth; d++) {
        p = reinterpret_cast<const uint8_t*>(&trace->rip[d]);
        for (uint32_t i = 0; i < 8; ++i) buf[off++] = p[i];
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
                   const PanicStackTrace* trace)
{
    // Step 1: Build binary panic packet (regs + stack trace)
    uint8_t packetBuf[512];
    uint32_t packetLen = BuildPanicPacket(packetBuf, sizeof(packetBuf), regs, trace);
    if (packetLen == 0)
    {
        SerialPuts("PANIC QR: packet build failed\n");
        return;
    }

    SerialPrintf("PANIC QR: packet %u bytes\n", packetLen);

    // Step 2: Base45 encode (no compression for now — adds ~50% overhead)
    char encoded[2048];
    int encLen = Base45Encode(encoded, sizeof(encoded), packetBuf, packetLen);
    if (encLen <= 0)
    {
        SerialPuts("PANIC QR: Base45 encode failed\n");
        return;
    }

    SerialPrintf("PANIC QR: Base45 encoded %d chars\n", encLen);

    // Step 3: Generate QR code using Nayuki library
    uint8_t qrBuf[qrcodegen_BUFFER_LEN_MAX];
    uint8_t tempBuf[qrcodegen_BUFFER_LEN_MAX];

    bool ok = qrcodegen_encodeText(encoded, tempBuf, qrBuf,
                                    qrcodegen_Ecc_LOW,
                                    qrcodegen_VERSION_MIN,
                                    qrcodegen_VERSION_MAX,
                                    qrcodegen_Mask_AUTO, true);
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
