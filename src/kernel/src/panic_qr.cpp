// QR code panic screen — renders CPU state as a scannable QR code.
//
// Pipeline: PanicCPURegs → binary packet → Base45 → Nayuki QR → framebuffer
//
// Uses Base45 encoding (alphanumeric mode) so the QR is readable by any
// phone camera app.  Multi-page support splits data across multiple QR
// codes if a single QR cannot hold it all.

#include "panic_qr.h"
#include "base45.h"
#include "serial.h"
#include "string.h"

extern "C" {
#include "qrcodegen.h"
#include "lz4.h"
}

namespace brook {

// Maximum Base45 chars that fit in a single QR (Version 25, Low ECC, alphanumeric)
// Version 25 = 117 modules → very scannable at 3px/module on a 1024x768 screen.
// Alphanumeric capacity at Low ECC: 3057 chars.
// We leave headroom for the per-page PanicHeader (Base45-encoded ~12 chars).
static constexpr uint32_t QR_MAX_ALPHANUMERIC_CHARS = 3000;

// Max binary payload bytes per page (before Base45 expansion).
// Base45 expands 2 bytes → 3 chars, so maxBytes ≈ maxChars * 2 / 3
static constexpr uint32_t QR_MAX_PAYLOAD_BYTES_PER_PAGE =
    (QR_MAX_ALPHANUMERIC_CHARS * 2) / 3 - sizeof(PanicHeader);

// Assemble the binary panic payload (TLV packets, no header — header added per page)
static uint32_t BuildPanicPayload(uint8_t* buf, uint32_t bufLen,
                                  const PanicCPURegs* regs,
                                  const PanicStackTrace* trace,
                                  const PanicExceptionInfo* excInfo,
                                  const PanicProcessList* procList,
                                  const PanicSystemInfo* sysInfo,
                                  const PanicStackDump* stackDump)
{
    uint32_t tracePayloadSize = 1 + trace->depth * 8;

    uint32_t needed = sizeof(PanicPacketHeader) + sizeof(PanicCPURegs)
                    + sizeof(PanicPacketHeader) + tracePayloadSize;
    if (excInfo) needed += sizeof(PanicPacketHeader) + sizeof(PanicExceptionInfo);
    if (procList) needed += sizeof(PanicPacketHeader) + 1 + procList->count * sizeof(PanicProcessEntry);
    if (sysInfo) needed += sizeof(PanicPacketHeader) + sizeof(PanicSystemInfo);
    if (stackDump) needed += sizeof(PanicPacketHeader) + 8 + 2 + stackDump->length;

    if (bufLen < needed) return 0;

    uint32_t off = 0;
    auto appendRaw = [&](const void* data, uint32_t size) {
        auto* p = static_cast<const uint8_t*>(data);
        memcpy(buf + off, p, size);
        off += size;
    };

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

    // Packet 5: System info (optional)
    if (sysInfo)
    {
        ph.type = QR_PACKET_TYPE_SYSTEM_INFO;
        ph.size = sizeof(PanicSystemInfo);
        appendRaw(&ph, sizeof(ph));
        appendRaw(sysInfo, sizeof(PanicSystemInfo));
    }

    // Packet 6: Stack dump (optional)
    if (stackDump && stackDump->length > 0)
    {
        uint32_t sdSize = 8 + 2 + stackDump->length;  // rsp + length + data
        ph.type = QR_PACKET_TYPE_STACK_DUMP;
        ph.size = sdSize;
        appendRaw(&ph, sizeof(ph));
        appendRaw(&stackDump->rsp, 8);
        appendRaw(&stackDump->length, 2);
        appendRaw(stackDump->data, stackDump->length);
    }

    return off;
}

// Emit the binary packet as hex to serial for host-side capture.
static void DumpPacketToSerial(const uint8_t* data, uint32_t len)
{
    SerialPuts("PANIC_HEX:");
    static const char hex[] = "0123456789ABCDEF";
    for (uint32_t i = 0; i < len; i++)
    {
        char buf[3];
        buf[0] = hex[data[i] >> 4];
        buf[1] = hex[data[i] & 0x0F];
        buf[2] = '\0';
        SerialPuts(buf);
    }
    SerialPuts("\n");
}

// Build a page buffer: PanicHeader + chunk of payload, then Base45-encode it.
// Returns length of Base45 string (excluding null terminator), or 0 on failure.
static uint32_t BuildBase45Page(char* b45Buf, uint32_t b45BufLen,
                                const uint8_t* payload, uint32_t payloadLen,
                                uint8_t page, uint8_t pageCount, uint8_t version)
{
    // Prepend the per-page PanicHeader to this chunk
    static uint8_t pageBuf[2048];
    uint32_t pageDataLen = sizeof(PanicHeader) + payloadLen;
    if (pageDataLen > sizeof(pageBuf)) return 0;

    PanicHeader hdr;
    hdr.magic     = QR_MAGIC_BYTE;
    hdr.version   = version;
    hdr.page      = page;
    hdr.pageCount = pageCount;
    hdr.pad       = QR_HEADER_PAD;
    memcpy(pageBuf, &hdr, sizeof(hdr));
    memcpy(pageBuf + sizeof(hdr), payload, payloadLen);

    int result = Base45Encode(b45Buf, b45BufLen, pageBuf, pageDataLen);
    if (result < 0)
    {
        SerialPuts("PANIC QR: Base45 encode failed\n");
        return 0;
    }
    return static_cast<uint32_t>(result);
}

// Render a single QR code to framebuffer at a given X origin.
static void RenderQRToFramebuffer(uint32_t* fb, uint32_t fbWidth, uint32_t fbHeight,
                                   uint32_t fbStride, const uint8_t* qrcode,
                                   uint32_t originX, uint32_t originY,
                                   uint32_t pixelsPerModule)
{
    int size = qrcodegen_getSize(qrcode);
    uint32_t strideQuads = fbStride / 4;

    const uint32_t moduleBlack = 0x00000000;
    const uint32_t moduleWhite = 0xFFFFFFFF - (QR_CONTRAST * 0x11111111);

    // Inverted mode: white modules on black background (like Enkel's QRDump for text)
    const uint32_t moduleColour = QR_INVERT_MODULES ? moduleWhite : moduleBlack;
    const uint32_t bgColour     = QR_INVERT_MODULES ? moduleBlack : moduleWhite;

    for (int y = -QR_BORDER_WIDTH; y < size + QR_BORDER_WIDTH; ++y)
    {
        for (int x = -QR_BORDER_WIDTH; x < size + QR_BORDER_WIDTH; ++x)
        {
            bool valid = (x >= 0 && x < size && y >= 0 && y < size);
            bool set = valid && qrcodegen_getModule(qrcode, x, y);
            uint32_t colour = set ? moduleColour : bgColour;

            for (uint32_t my = 0; my < pixelsPerModule; ++my)
            {
                for (uint32_t mx = 0; mx < pixelsPerModule; ++mx)
                {
                    uint32_t px = originX + static_cast<uint32_t>(
                        (x + QR_BORDER_WIDTH) * static_cast<int>(pixelsPerModule)) + mx;
                    uint32_t py = originY + static_cast<uint32_t>(
                        (y + QR_BORDER_WIDTH) * static_cast<int>(pixelsPerModule)) + my;
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
                   const PanicProcessList* procList,
                   const PanicSystemInfo* sysInfo,
                   const PanicStackDump* stackDump)
{
    // Auto-select pixels per module based on display resolution
    // Low-DPI devices (e.g. Surface Go at 1024x768) need larger modules
    const uint32_t QR_PIXELS_PER_MODULE = (fbWidth <= QR_LODPI_THRESHOLD)
                                          ? QR_PIXELS_PER_MODULE_LODPI
                                          : QR_PIXELS_PER_MODULE_HIDPI;
    SerialPrintf("PANIC QR: %ux%u fb, using %u px/module\n",
                 fbWidth, fbHeight, QR_PIXELS_PER_MODULE);

    // Step 1: Build binary TLV payload (no header yet — added per page)
    static uint8_t payloadBuf[2048];
    uint32_t payloadLen = BuildPanicPayload(payloadBuf, sizeof(payloadBuf),
                                            regs, trace, excInfo, procList,
                                            sysInfo, stackDump);
    if (payloadLen == 0)
    {
        SerialPuts("PANIC QR: payload build failed\n");
        return;
    }

    SerialPrintf("PANIC QR: payload %u bytes\n", payloadLen);

    // Step 1b: Compress with LZ4
    static uint8_t compressedBuf[2048];
    int compressedLen = LZ4_compress_default(
        reinterpret_cast<const char*>(payloadBuf),
        reinterpret_cast<char*>(compressedBuf + 4),  // leave 4 bytes for uncompressed size
        static_cast<int>(payloadLen),
        static_cast<int>(sizeof(compressedBuf) - 4));

    // Prepend uncompressed size (little-endian u32) so decoder knows output buffer size
    const uint8_t* dataToEncode;
    uint32_t dataLen;
    if (compressedLen > 0 && static_cast<uint32_t>(compressedLen + 4) < payloadLen)
    {
        // Compression helped — prefix with original size
        compressedBuf[0] = static_cast<uint8_t>(payloadLen & 0xFF);
        compressedBuf[1] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
        compressedBuf[2] = static_cast<uint8_t>((payloadLen >> 16) & 0xFF);
        compressedBuf[3] = static_cast<uint8_t>((payloadLen >> 24) & 0xFF);
        dataLen = static_cast<uint32_t>(compressedLen) + 4;
        dataToEncode = compressedBuf;
        SerialPrintf("PANIC QR: LZ4 compressed %u → %u bytes\n", payloadLen, dataLen);
    }
    else
    {
        // Compression didn't help — use raw payload
        dataToEncode = payloadBuf;
        dataLen = payloadLen;
        SerialPrintf("PANIC QR: LZ4 compression skipped (no benefit)\n");
    }

    // Also dump hex to serial for host-side capture (works with crash_decoder.py --hex)
    // Use compressed data with v2 header
    {
        static uint8_t serialBuf[2048];
        PanicHeader hdr;
        hdr.magic     = QR_MAGIC_BYTE;
        hdr.version   = (dataToEncode == compressedBuf) ? QR_VERSION : QR_VERSION_RAW;
        hdr.page      = 0;
        hdr.pageCount = 1;
        hdr.pad       = QR_HEADER_PAD;
        memcpy(serialBuf, &hdr, sizeof(hdr));
        memcpy(serialBuf + sizeof(hdr), dataToEncode, dataLen);
        DumpPacketToSerial(serialBuf, sizeof(hdr) + dataLen);
    }

    // Step 2: Calculate pagination
    uint32_t maxPerPage = QR_MAX_PAYLOAD_BYTES_PER_PAGE;
    uint8_t pageCount = static_cast<uint8_t>((dataLen + maxPerPage - 1) / maxPerPage);
    if (pageCount == 0) pageCount = 1;
    if (pageCount > QR_MAX_PAGES) pageCount = QR_MAX_PAGES;

    SerialPrintf("PANIC QR: %u pages (max %u bytes/page)\n", pageCount, maxPerPage);

    // Step 3: Encode and render each page
    static char b45Buf[4096];
    static uint8_t qrBuf[qrcodegen_BUFFER_LEN_MAX];
    static uint8_t tempBuf[qrcodegen_BUFFER_LEN_MAX];

    // Calculate QR layout: position pages horizontally in the right column
    uint32_t qrAreaX = fbWidth * 55 / 100;
    uint32_t qrAreaW = fbWidth - qrAreaX;

    // Use v2 (compressed) or v1 (raw) version in QR headers
    uint8_t qrVersion = (dataToEncode == compressedBuf) ? QR_VERSION : QR_VERSION_RAW;

    for (uint8_t page = 0; page < pageCount; page++)
    {
        uint32_t chunkStart = page * maxPerPage;
        uint32_t chunkLen = dataLen - chunkStart;
        if (chunkLen > maxPerPage) chunkLen = maxPerPage;

        // Base45 encode this page
        uint32_t b45Len = BuildBase45Page(b45Buf, sizeof(b45Buf),
                                          dataToEncode + chunkStart, chunkLen,
                                          page, pageCount, qrVersion);
        if (b45Len == 0)
        {
            SerialPrintf("PANIC QR: page %u Base45 encode failed\n", page);
            continue;
        }

        SerialPrintf("PANIC QR: page %u: %u bytes → %u Base45 chars\n",
                     page, chunkLen, b45Len);

        // Also dump the Base45 text to serial for phone-scan-equivalent testing
        SerialPuts("PANIC_B45_P");
        {
            char pageStr[2] = { static_cast<char>('0' + page), '\0' };
            SerialPuts(pageStr);
        }
        SerialPuts(":");
        SerialPuts(b45Buf);
        SerialPuts("\n");

        // Generate QR code using alphanumeric mode (encodeText auto-selects)
        bool ok = qrcodegen_encodeText(b45Buf, tempBuf, qrBuf,
                                        qrcodegen_Ecc_LOW,
                                        qrcodegen_VERSION_MIN,
                                        qrcodegen_VERSION_MAX,
                                        qrcodegen_Mask_AUTO, true);
        if (!ok)
        {
            SerialPrintf("PANIC QR: page %u QR generation failed\n", page);
            continue;
        }

        int qrSize = qrcodegen_getSize(qrBuf);
        SerialPrintf("PANIC QR: page %u: QR %d modules\n", page, qrSize);

        // Position this QR code — single page centred, multi-page stacked vertically
        uint32_t qrPixelSize = static_cast<uint32_t>(qrSize + 2 * QR_BORDER_WIDTH) * QR_PIXELS_PER_MODULE;
        uint32_t startX = qrAreaX + (qrAreaW > qrPixelSize ? (qrAreaW - qrPixelSize) / 2 : 0);
        uint32_t startY;
        if (pageCount == 1)
        {
            // Centre vertically
            startY = fbHeight > qrPixelSize + 100
                     ? (fbHeight - qrPixelSize) / 2
                     : QR_START_Y;
        }
        else
        {
            // Stack pages vertically with spacing
            uint32_t totalH = pageCount * qrPixelSize + (pageCount - 1) * 16;
            uint32_t baseY = fbHeight > totalH + 40
                             ? (fbHeight - totalH) / 2
                             : 20;
            startY = baseY + page * (qrPixelSize + 16);
        }

        RenderQRToFramebuffer(fbBase, fbWidth, fbHeight, fbStride, qrBuf, startX, startY, QR_PIXELS_PER_MODULE);
    }

    SerialPuts("PANIC QR: rendered to framebuffer\n");
}

} // namespace brook
