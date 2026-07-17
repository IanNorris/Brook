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
#include "debug_overlay.h"

extern "C" {
#include "qrcodegen.h"
#include "lz4.h"
}

namespace brook {

// Custom diagnostic blob storage (see PanicSetCustomBlob / panic_qr.h). A bug
// site fills this before KernelPanic; the payload builder emits it once. Static
// so it's usable from any context with no allocation.
static char     g_customBlobTag[8]   = {0};
static uint16_t g_customBlobFormat   = 0;
static uint32_t g_customBlobSize     = 0;
static uint8_t  g_customBlobData[PANIC_CUSTOM_BLOB_MAX];

void PanicSetCustomBlob(const char tag[8], uint16_t format,
                        const void* data, uint32_t size)
{
    if (size > PANIC_CUSTOM_BLOB_MAX) size = PANIC_CUSTOM_BLOB_MAX;
    for (int i = 0; i < 8; i++) g_customBlobTag[i] = tag ? tag[i] : 0;
    g_customBlobFormat = format;
    const uint8_t* s = static_cast<const uint8_t*>(data);
    for (uint32_t i = 0; i < size; i++) g_customBlobData[i] = s[i];
    g_customBlobSize = size;   // set last: a reader sees a complete blob
}

const void* PanicGetCustomBlob(char outTag[8], uint16_t* outFormat, uint32_t* outSize)
{
    if (g_customBlobSize == 0) return nullptr;
    if (outTag)    for (int i = 0; i < 8; i++) outTag[i] = g_customBlobTag[i];
    if (outFormat) *outFormat = g_customBlobFormat;
    if (outSize)   *outSize = g_customBlobSize;
    return g_customBlobData;
}

// --- Temporal multi-page cycling state -------------------------------------
// The panic path renders ONE payload QR page at a time (each fills the full
// column, so it's large and scannable regardless of page count) and cycles
// pages from the final panic spin loop via PanicQrCyclePage(). The document is
// built once by PanicRenderQR(); all storage is static so it stays valid after
// that call returns — the panic never unwinds.
struct PanicQrDoc {
    uint32_t* fb;
    uint32_t  fbWidth, fbHeight, fbStride;
    uint32_t  areaX, areaY, areaW, areaH;   // payload region (below the URL band)
    uint32_t  px;                           // pixels/module, fixed across pages
    uint32_t  dataLen;
    uint32_t  maxPerPage;
    uint8_t   pageCount;
    uint8_t   curPage;
    uint8_t   qrVersion;
    bool      valid;
};
static PanicQrDoc g_qrDoc;
static uint8_t    g_qrDocData[PANIC_PAYLOAD_BUF_MAX];  // owned copy of the payload

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
                                  const PanicStackDump* stackDump,
                                  const PanicCpuList* cpuList)
{
    uint32_t tracePayloadSize = 1 + trace->depth * 8;

    uint32_t needed = sizeof(PanicPacketHeader) + sizeof(PanicCPURegs)
                    + sizeof(PanicPacketHeader) + tracePayloadSize;
    if (excInfo) needed += sizeof(PanicPacketHeader) + sizeof(PanicExceptionInfo);
    if (procList) needed += sizeof(PanicPacketHeader) + 1
                          + procList->count * PANIC_PROCESS_ENTRY_WIRE_SIZE
                          + sizeof(PanicPacketHeader) + 1
                          + procList->count * sizeof(PanicProcessExtEntry);
    if (sysInfo) needed += sizeof(PanicPacketHeader) + sizeof(PanicSystemInfo);
    if (cpuList) needed += sizeof(PanicPacketHeader) + 1
                         + cpuList->count * sizeof(PanicCpuEntry);
    if (stackDump) needed += sizeof(PanicPacketHeader) + 8 + 2 + stackDump->length;
    {
        char t[8]; uint16_t f; uint32_t bl = 0;
        if (PanicGetCustomBlob(t, &f, &bl))
            needed += sizeof(PanicPacketHeader) + sizeof(PanicCustomBlobHeader) + bl;
    }

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

    // Packet 4: Process list (optional) — STABLE 24-byte wire entries only.
    if (procList && procList->count > 0)
    {
        uint32_t plSize = 1 + procList->count * PANIC_PROCESS_ENTRY_WIRE_SIZE;
        ph.type = QR_PACKET_TYPE_PROCESS_LIST;
        ph.size = plSize;
        appendRaw(&ph, sizeof(ph));
        buf[off++] = procList->count;
        for (uint8_t i = 0; i < procList->count; i++)
            // Only the first 24 bytes (pid/state/cpu/name/rip) are on the wire.
            appendRaw(&procList->entries[i], PANIC_PROCESS_ENTRY_WIRE_SIZE);

        // Packet 4b: Process reap-gate extension (BRO-176). Optional, self-
        // describing TLV — old decoders skip it; new ones merge by pid.
        uint32_t extSize = 1 + procList->count * sizeof(PanicProcessExtEntry);
        ph.type = QR_PACKET_TYPE_PROCESS_EXT;
        ph.size = extSize;
        appendRaw(&ph, sizeof(ph));
        buf[off++] = procList->count;
        for (uint8_t i = 0; i < procList->count; i++)
        {
            const PanicProcessEntry& e = procList->entries[i];
            PanicProcessExtEntry x;
            x.pid           = e.pid;
            x.tgid          = e.tgid;
            x.asLiveThreads = e.asLiveThreads;
            x.refCount      = e.refCount;
            x.flags         = e.flags;
            x.reserved      = 0;
            appendRaw(&x, sizeof(x));
        }
    }

    // Packet 5: System info (optional)
    if (sysInfo)
    {
        ph.type = QR_PACKET_TYPE_SYSTEM_INFO;
        ph.size = sizeof(PanicSystemInfo);
        appendRaw(&ph, sizeof(ph));
        appendRaw(sysInfo, sizeof(PanicSystemInfo));
    }

    // Packet 5b: Per-CPU state (optional TLV extension)
    if (cpuList && cpuList->count > 0)
    {
        uint32_t clSize = 1 + cpuList->count * sizeof(PanicCpuEntry);
        ph.type = QR_PACKET_TYPE_CPU_STATE;
        ph.size = clSize;
        appendRaw(&ph, sizeof(ph));
        buf[off++] = cpuList->count;
        for (uint8_t i = 0; i < cpuList->count; i++)
            appendRaw(&cpuList->entries[i], sizeof(PanicCpuEntry));
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

    // Packet 7: Custom diagnostic blob (optional) — a bug site stashed this
    // before KernelPanic (PanicSetCustomBlob). Emitted verbatim so the QR is a
    // self-contained capture of bug-specific state (e.g. BRO-208 ownership ring).
    {
        char tag[8]; uint16_t fmt = 0; uint32_t blobLen = 0;
        const void* blob = PanicGetCustomBlob(tag, &fmt, &blobLen);
        if (blob && blobLen > 0 &&
            off + sizeof(PanicPacketHeader) + sizeof(PanicCustomBlobHeader) + blobLen <= bufLen)
        {
            PanicCustomBlobHeader cbh;
            for (int i = 0; i < 8; i++) cbh.tag[i] = tag[i];
            cbh.format = fmt;
            cbh.reserved = 0;
            ph.type = QR_PACKET_TYPE_CUSTOM_BLOB;
            ph.size = sizeof(PanicCustomBlobHeader) + blobLen;
            appendRaw(&ph, sizeof(ph));
            appendRaw(&cbh, sizeof(cbh));
            appendRaw(blob, blobLen);
        }
    }

    // Packet 8: Recent kernel-log lines (from the debug_overlay ring) — captured
    // here so a crash carries its own preceding log context. Panic-safe try-lock
    // snapshot (never blocks; a parked CPU may hold the ring lock). Oldest lines
    // are dropped first to stay within PANIC_DEBUG_LOG_MAX; the dropped count is
    // recorded so the decoder can show "N earlier lines omitted".
    {
        static char logText[PANIC_DEBUG_LOG_MAX];
        uint32_t omitted = 0;
        uint32_t textLen = DebugOverlaySnapshotTail(
            logText, sizeof(logText), PANIC_DEBUG_LOG_LINES, &omitted);
        if (textLen > 0 &&
            off + sizeof(PanicPacketHeader) + sizeof(PanicDebugLogHeader) + textLen <= bufLen)
        {
            // Count lines actually packed (newline separators).
            uint16_t lines = 0;
            for (uint32_t i = 0; i < textLen; i++)
                if (logText[i] == '\n') lines++;
            PanicDebugLogHeader dlh;
            dlh.lineCount    = lines;
            dlh.omittedLines = static_cast<uint16_t>(omitted > 0xFFFF ? 0xFFFF : omitted);
            dlh.textLen      = textLen;
            ph.type = QR_PACKET_TYPE_DEBUG_LOG;
            ph.size = sizeof(PanicDebugLogHeader) + textLen;
            appendRaw(&ph, sizeof(ph));
            appendRaw(&dlh, sizeof(dlh));
            appendRaw(logText, textLen);
        }
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

// Fill a rectangle with a solid colour (clipped to the framebuffer). Used to
// clear the payload region before drawing a new page so the previous page can't
// bleed through around a smaller QR.
static void FillRect(uint32_t* fb, uint32_t fbWidth, uint32_t fbHeight,
                     uint32_t fbStride, uint32_t x0, uint32_t y0,
                     uint32_t w, uint32_t h, uint32_t colour)
{
    uint32_t strideQuads = fbStride / 4;
    for (uint32_t y = y0; y < y0 + h && y < fbHeight; ++y)
        for (uint32_t x = x0; x < x0 + w && x < fbWidth; ++x)
            fb[y * strideQuads + x] = colour;
}

// Draw a row of page-indicator dots centred at the bottom of the payload area:
// one square per page, the current page filled bright, the rest dim. The page
// number is ALSO encoded in each QR header, so a scanner reassembles correctly
// regardless — these dots are just a human cue that pages are cycling.
static void RenderPageDots(const PanicQrDoc& d, uint8_t current)
{
    if (d.pageCount <= 1) return;
    const uint32_t dot = 14, gap = 10;
    uint32_t totalW = d.pageCount * dot + (d.pageCount - 1) * gap;
    uint32_t x0 = d.areaX + (d.areaW > totalW ? (d.areaW - totalW) / 2 : 0);
    uint32_t y0 = d.areaY + d.areaH - dot - 6;
    for (uint8_t i = 0; i < d.pageCount; ++i)
    {
        uint32_t c = (i == current) ? 0x00FFFFFF : 0x00404040;
        FillRect(d.fb, d.fbWidth, d.fbHeight, d.fbStride,
                 x0 + i * (dot + gap), y0, dot, dot, c);
    }
}

// Render a single payload page into the (cleared) payload region, centred, at
// the document's fixed module size. Re-encodes Base45 + QR per call — cheap, and
// keeps only the compact payload in static storage rather than N QR bitmaps.
static void RenderPayloadPage(uint8_t page)
{
    PanicQrDoc& d = g_qrDoc;
    if (!d.valid || page >= d.pageCount) return;

    static char    b45[8192];
    static uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    static uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];

    // Clear the payload region to the panic-screen background (black); the QR's
    // own quiet zone paints white on top.
    FillRect(d.fb, d.fbWidth, d.fbHeight, d.fbStride,
             d.areaX, d.areaY, d.areaW, d.areaH, 0x00000000);

    uint32_t chunkStart = static_cast<uint32_t>(page) * d.maxPerPage;
    if (chunkStart >= d.dataLen) return;
    uint32_t chunkLen = d.dataLen - chunkStart;
    if (chunkLen > d.maxPerPage) chunkLen = d.maxPerPage;

    uint32_t bl = BuildBase45Page(b45, sizeof(b45), g_qrDocData + chunkStart,
                                  chunkLen, page, d.pageCount, d.qrVersion);
    if (bl == 0) return;
    if (!qrcodegen_encodeText(b45, tmp, qr, qrcodegen_Ecc_LOW,
                              qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                              qrcodegen_Mask_AUTO, true))
        return;

    int qrSize = qrcodegen_getSize(qr);
    uint32_t qrPx = static_cast<uint32_t>(qrSize + 2 * QR_BORDER_WIDTH) * d.px;
    uint32_t startX = d.areaX + (d.areaW > qrPx ? (d.areaW - qrPx) / 2 : 0);
    uint32_t startY = d.areaY + (d.areaH > qrPx ? (d.areaH - qrPx) / 2 : 0);
    RenderQRToFramebuffer(d.fb, d.fbWidth, d.fbHeight, d.fbStride, qr,
                          startX, startY, d.px);
    RenderPageDots(d, page);
}

// Advance to the next payload page and render it. Called from the final panic
// spin loop to cycle multi-page payloads. Returns false if there's nothing to
// cycle (<=1 page). The caller must DisplayFlush() after a true return.
bool PanicQrCyclePage()
{
    if (!g_qrDoc.valid || g_qrDoc.pageCount <= 1) return false;
    g_qrDoc.curPage = (g_qrDoc.curPage + 1) % g_qrDoc.pageCount;
    RenderPayloadPage(g_qrDoc.curPage);
    return true;
}

uint8_t PanicQrPageCount()
{
    return g_qrDoc.valid ? g_qrDoc.pageCount : 0;
}

void PanicRenderQR(uint32_t* fbBase, uint32_t fbWidth, uint32_t fbHeight,
                   uint32_t fbStride, const PanicCPURegs* regs,
                   const PanicStackTrace* trace,
                   const PanicExceptionInfo* excInfo,
                   const PanicProcessList* procList,
                   const PanicSystemInfo* sysInfo,
                   const PanicStackDump* stackDump,
                   const PanicCpuList* cpuList)
{
    // Pixels-per-module is chosen dynamically once the QR sizes and page count
    // are known (see below) so the codes fill the column and stay scannable.
    uint32_t QR_PIXELS_PER_MODULE = QR_PIXELS_PER_MODULE_MIN;

    // Step 1: Build binary TLV payload (no header yet — added per page). Sized to
    // hold the fixed packets plus up to PANIC_DEBUG_LOG_MAX of log text; keep the
    // compressed scratch at LZ4_compressBound so incompressible input can't
    // overflow it.
    static uint8_t payloadBuf[PANIC_PAYLOAD_BUF_MAX];
    uint32_t payloadLen = BuildPanicPayload(payloadBuf, sizeof(payloadBuf),
                                            regs, trace, excInfo, procList,
                                            sysInfo, stackDump, cpuList);
    if (payloadLen == 0)
    {
        SerialPuts("PANIC QR: payload build failed\n");
        return;
    }

    SerialPrintf("PANIC QR: payload %u bytes\n", payloadLen);

    // Step 1b: Compress with LZ4
    static uint8_t compressedBuf[PANIC_PAYLOAD_BUF_MAX];
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
        static uint8_t serialBuf[8192];
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

    // Calculate QR layout: position pages horizontally in the right column
    uint32_t qrAreaX = fbWidth * 55 / 100;
    uint32_t qrAreaW = fbWidth - qrAreaX;

    // Step 2a: pick the on-screen module size dynamically. Measure the ACTUAL
    // payload QR (page 0 is a full page = the densest) and the ingest-URL QR,
    // then choose the largest pixels-per-module that fits BOTH the column width
    // and the vertical budget shared by the URL band and every stacked payload
    // page. The old fixed 3px/module left over half the column empty and was too
    // small to scan without enlarging the window (Ian's report); this fills it.
    static char b45Buf[8192];
    static uint8_t qrBuf[qrcodegen_BUFFER_LEN_MAX];
    static uint8_t tempBuf[qrcodegen_BUFFER_LEN_MAX];
    // Use v2 (compressed) or v1 (raw) version in QR headers.
    uint8_t qrVersion = (dataToEncode == compressedBuf) ? QR_VERSION : QR_VERSION_RAW;

    uint32_t payloadModules = 117 + 2 * QR_BORDER_WIDTH; // version-25 cap (safe fallback)
    uint32_t urlModules     = 33  + 2 * QR_BORDER_WIDTH; // ~version-4 URL QR (est fallback)
    {
        uint32_t c0 = dataLen; if (c0 > maxPerPage) c0 = maxPerPage;
        uint32_t bl = BuildBase45Page(b45Buf, sizeof(b45Buf), dataToEncode, c0,
                                      0, pageCount, qrVersion);
        if (bl && qrcodegen_encodeText(b45Buf, tempBuf, qrBuf, qrcodegen_Ecc_LOW,
                                       qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                                       qrcodegen_Mask_AUTO, true))
            payloadModules = static_cast<uint32_t>(qrcodegen_getSize(qrBuf))
                             + 2 * QR_BORDER_WIDTH;
        if (qrcodegen_encodeText(PANIC_INGEST_URL, tempBuf, qrBuf,
                                 qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                                 qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true))
            urlModules = static_cast<uint32_t>(qrcodegen_getSize(qrBuf))
                         + 2 * QR_BORDER_WIDTH;
    }

    // Width budget: the payload (the widest QR) must fit the column.
    uint32_t scaleW = payloadModules ? qrAreaW / payloadModules
                                     : QR_PIXELS_PER_MODULE_MIN;
    // Height budget: top margin + URL band + gap + ONE payload page. With
    // temporal cycling only a single payload QR is on screen at a time, so it
    // gets the full column height regardless of pageCount — that's what keeps
    // each page big and scannable no matter how much data we carry.
    uint32_t vMarginPx = 20 + 16 + 24; // top + gap + dot-indicator band
    uint32_t vModules  = urlModules + payloadModules;
    uint32_t vAvailPx  = (fbHeight > vMarginPx) ? fbHeight - vMarginPx : fbHeight;
    uint32_t scaleH    = vModules ? vAvailPx / vModules : QR_PIXELS_PER_MODULE_MIN;

    QR_PIXELS_PER_MODULE = scaleW < scaleH ? scaleW : scaleH;
    if (QR_PIXELS_PER_MODULE < QR_PIXELS_PER_MODULE_MIN)
        QR_PIXELS_PER_MODULE = QR_PIXELS_PER_MODULE_MIN;
    if (QR_PIXELS_PER_MODULE > QR_PIXELS_PER_MODULE_MAX)
        QR_PIXELS_PER_MODULE = QR_PIXELS_PER_MODULE_MAX;
    SerialPrintf("PANIC QR: %ux%u fb, payload=%u url=%u modules, %u pages -> %u px/module\n",
                 fbWidth, fbHeight, payloadModules, urlModules, pageCount,
                 QR_PIXELS_PER_MODULE);

    // Step 2b: render a small STATIC "ingest URL" QR at the top of the QR column.
    // Scanning it opens the Brook panic scanner site, which then reads the dense
    // payload QR(s) below.  Kept separate so the payload stays full-density (see
    // PANIC_INGEST_URL note in panic_qr.h).  Drawn ONCE; the cycler never
    // redraws it (stable geometry aids camera autofocus). Failure is non-fatal.
    uint32_t urlBandH = 0;
    {
        static uint8_t urlQrBuf[qrcodegen_BUFFER_LEN_MAX];
        static uint8_t urlTmpBuf[qrcodegen_BUFFER_LEN_MAX];
        if (qrcodegen_encodeText(PANIC_INGEST_URL, urlTmpBuf, urlQrBuf,
                                 qrcodegen_Ecc_MEDIUM,
                                 qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                                 qrcodegen_Mask_AUTO, true))
        {
            int urlSize = qrcodegen_getSize(urlQrBuf);
            uint32_t urlPx = static_cast<uint32_t>(urlSize + 2 * QR_BORDER_WIDTH) * QR_PIXELS_PER_MODULE;
            uint32_t urlX = qrAreaX + (qrAreaW > urlPx ? (qrAreaW - urlPx) / 2 : 0);
            uint32_t urlY = 20;
            RenderQRToFramebuffer(fbBase, fbWidth, fbHeight, fbStride, urlQrBuf,
                                  urlX, urlY, QR_PIXELS_PER_MODULE);
            urlBandH = urlY + urlPx + 16;  // reserve this band; payload QRs start below
            SerialPuts("PANIC QR: ingest URL QR rendered (" PANIC_INGEST_URL ")\n");
        }
    }

    // Step 3: dump EVERY page's Base45 to serial (the host-side decoder oracle,
    // independent of what's on screen), then set up the cycling document and
    // render page 0. On screen we show one full-area page at a time; the panic
    // spin loop calls PanicQrCyclePage() to advance through the rest.
    for (uint8_t page = 0; page < pageCount; page++)
    {
        uint32_t chunkStart = page * maxPerPage;
        uint32_t chunkLen = dataLen - chunkStart;
        if (chunkLen > maxPerPage) chunkLen = maxPerPage;

        uint32_t b45Len = BuildBase45Page(b45Buf, sizeof(b45Buf),
                                          dataToEncode + chunkStart, chunkLen,
                                          page, pageCount, qrVersion);
        if (b45Len == 0)
        {
            SerialPrintf("PANIC QR: page %u Base45 encode failed\n", page);
            continue;
        }
        SerialPrintf("PANIC QR: page %u: %u bytes -> %u Base45 chars\n",
                     page, chunkLen, b45Len);
        SerialPuts("PANIC_B45_P");
        {
            char pageStr[2] = { static_cast<char>('0' + page), '\0' };
            SerialPuts(pageStr);
        }
        SerialPuts(":");
        SerialPuts(b45Buf);
        SerialPuts("\n");
    }

    // Publish the cycling document. dataToEncode points at a function-local
    // static (payloadBuf/compressedBuf), which outlives this call, but copy it
    // into dedicated storage so the cycler is self-contained and unambiguous.
    uint32_t copyLen = dataLen < sizeof(g_qrDocData) ? dataLen : sizeof(g_qrDocData);
    for (uint32_t i = 0; i < copyLen; ++i) g_qrDocData[i] = dataToEncode[i];
    g_qrDoc.fb         = fbBase;
    g_qrDoc.fbWidth    = fbWidth;
    g_qrDoc.fbHeight   = fbHeight;
    g_qrDoc.fbStride   = fbStride;
    g_qrDoc.areaX      = qrAreaX;
    g_qrDoc.areaY      = urlBandH ? urlBandH : QR_START_Y;
    g_qrDoc.areaW      = qrAreaW;
    g_qrDoc.areaH      = (fbHeight > g_qrDoc.areaY) ? fbHeight - g_qrDoc.areaY : 0;
    g_qrDoc.px         = QR_PIXELS_PER_MODULE;
    g_qrDoc.dataLen    = copyLen;
    g_qrDoc.maxPerPage = maxPerPage;
    g_qrDoc.pageCount  = pageCount;
    g_qrDoc.curPage    = 0;
    g_qrDoc.qrVersion  = qrVersion;
    g_qrDoc.valid      = true;

    RenderPayloadPage(0);
    SerialPrintf("PANIC QR: rendered page 1/%u to framebuffer (temporal cycle)\n",
                 pageCount);
    return;
}

#if 0  // Legacy stacked multi-page renderer — replaced by temporal cycling above.
void PanicRenderQR_stacked_unused()
{
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

        // Position this QR code — single page centred, multi-page stacked vertically.
        // All payload QRs are kept below the static ingest-URL QR band at top.
        uint32_t qrPixelSize = static_cast<uint32_t>(qrSize + 2 * QR_BORDER_WIDTH) * QR_PIXELS_PER_MODULE;
        uint32_t startX = qrAreaX + (qrAreaW > qrPixelSize ? (qrAreaW - qrPixelSize) / 2 : 0);
        uint32_t startY;
        if (pageCount == 1)
        {
            // Centre vertically within the area below the URL band.
            uint32_t availTop = urlBandH;
            uint32_t availH = fbHeight > availTop ? fbHeight - availTop : 0;
            startY = (availH > qrPixelSize + 100)
                     ? availTop + (availH - qrPixelSize) / 2
                     : (urlBandH ? urlBandH : QR_START_Y);
        }
        else
        {
            // Stack pages vertically with spacing, starting below the URL band.
            uint32_t totalH = pageCount * qrPixelSize + (pageCount - 1) * 16;
            uint32_t availTop = urlBandH;
            uint32_t availH = fbHeight > availTop ? fbHeight - availTop : 0;
            uint32_t baseY = (availH > totalH + 40)
                             ? availTop + (availH - totalH) / 2
                             : (urlBandH ? urlBandH : 20);
            startY = baseY + page * (qrPixelSize + 16);
        }

        RenderQRToFramebuffer(fbBase, fbWidth, fbHeight, fbStride, qrBuf, startX, startY, QR_PIXELS_PER_MODULE);
    }

    SerialPuts("PANIC QR: rendered to framebuffer\n");
}
#endif  // legacy stacked renderer

} // namespace brook
