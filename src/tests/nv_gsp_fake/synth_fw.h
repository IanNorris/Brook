#pragma once
// Small synthetic firmware containers with the same layouts as the 570.144
// blobs and the TU104 VBIOS, so the GSP M0 tests run without the NVIDIA
// files (which stay outside the repo). Real-file checks are separate and
// enabled by environment variables (see test_nv_gsp_fw).
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace synth_fw {

using Bytes = std::vector<uint8_t>;
inline void P8(Bytes& b, size_t o, uint8_t v) { if (b.size() < o + 1) b.resize(o + 1); b[o] = v; }
inline void P16(Bytes& b, size_t o, uint16_t v) { P8(b, o, uint8_t(v)); P8(b, o + 1, uint8_t(v >> 8)); }
inline void P32(Bytes& b, size_t o, uint32_t v) { P16(b, o, uint16_t(v)); P16(b, o + 2, uint16_t(v >> 16)); }
inline void P64(Bytes& b, size_t o, uint64_t v) { P32(b, o, uint32_t(v)); P32(b, o + 4, uint32_t(v >> 32)); }
inline void Fill(Bytes& b, size_t o, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; ++i) P8(b, o + i, uint8_t(seed + i * 7));
}

// ELF64 with .fwimage, .fwsignature_tu10x, .shstrtab
inline Bytes Elf(uint32_t image_size, uint32_t sig_size) {
    Bytes b(0x40, 0);
    b[0] = 0x7f; b[1] = 'E'; b[2] = 'L'; b[3] = 'F'; b[4] = 2; b[5] = 1; b[6] = 1;
    const size_t img = 0x40, sig = img + image_size, str = sig + sig_size;
    Fill(b, img, image_size, 0x11);
    Fill(b, sig, sig_size, 0x55);
    const char names[] = "\0.fwimage\0.fwsignature_tu10x\0.shstrtab\0";
    for (size_t i = 0; i < sizeof(names); ++i) P8(b, str + i, uint8_t(names[i]));
    const size_t sh = (str + sizeof(names) + 7) & ~size_t(7);
    struct S { uint32_t name; uint64_t off, size; } s[4] = {{0, 0, 0}, {1, img, image_size}, {10, sig, sig_size},
                                                            {29, str, sizeof(names)}};
    for (int i = 0; i < 4; ++i) {
        const size_t h = sh + size_t(i) * 0x40;
        P32(b, h, s[i].name); P32(b, h + 4, i ? 1 : 0); P64(b, h + 0x18, s[i].off); P64(b, h + 0x20, s[i].size);
        P8(b, h + 0x3f, 0);
    }
    P64(b, 0x28, sh); P16(b, 0x3a, 0x40); P16(b, 0x3c, 4); P16(b, 0x3e, 3);
    return b;
}

// bin_hdr + HS v2 booter: os code 0+0x100, app0 0x100+0x300, data 0x400+0x200,
// one 16-byte prod signature patched at image offset 0x500.
inline Bytes Booter() {
    Bytes b(0x700, 0);
    P32(b, 0, 0x10de); P32(b, 4, 1); P32(b, 8, 0x700); P32(b, 12, 0x18); P32(b, 16, 0x100); P32(b, 20, 0x600);
    const uint32_t hs[9] = {0x60, 0x10, 0x80, 0x84, 0, 0, 0x88, 0x90, 0x24};
    for (int i = 0; i < 9; ++i) P32(b, 0x18 + 4 * i, hs[i]);
    Fill(b, 0x60, 0x10, 0xa0);
    P32(b, 0x80, 0x500); P32(b, 0x84, 0); P32(b, 0x88, 1);
    const uint32_t lh[9] = {0, 0x100, 0x400, 0x200, 1, 0x100, 0x300, 0, 0};
    for (int i = 0; i < 9; ++i) P32(b, 0x90 + 4 * i, lh[i]);
    Fill(b, 0x100, 0x600, 0x21);
    return b;
}

// acr/bl.bin: start_tag 0xfd, 0x200 bytes of code.
inline Bytes AcrBl() {
    Bytes b(0x300, 0);
    P32(b, 0, 0x10de); P32(b, 4, 1); P32(b, 8, 0x300); P32(b, 12, 0x18); P32(b, 16, 0x40); P32(b, 20, 0x2c0);
    const uint32_t d[6] = {0xfd, 0, 0, 0x200, 0x200, 0xc0};
    for (int i = 0; i < 6; ++i) P32(b, 0x18 + 4 * i, d[i]);
    Fill(b, 0x40, 0x2c0, 0x33);
    return b;
}

// RISC-V bootloader: desc v4 (appVersion 0, like 570.144), 0x1000 payload.
inline Bytes RiscvBl() {
    Bytes b(0x1064, 0);
    P32(b, 0, 0x10de); P32(b, 4, 1); P32(b, 8, 0x1100); P32(b, 12, 0x18); P32(b, 16, 0x64); P32(b, 20, 0x1000);
    P32(b, 0x18, 4); P32(b, 0x18 + 8, 0x488); P32(b, 0x18 + 12, 0x488); P32(b, 0x18 + 16, 0x10);
    Fill(b, 0x64, 0x1000, 0x44);
    return b;
}

// VBIOS: image 0 (legacy, 0x1000), image 1 (EFI 0x03, 0x1000), image 2
// (0xE0, 0x3000, last). BIT 'p' in image 0 points past image 0, so the
// falcon ucode table and the FWSEC v2 descriptor need the 0xE0 remap
// (+0x1000). FWSEC: IMEM 0x400 (secure 0x100+0x300), DMEM 0x100 at 0x400,
// interface at DMEM 0x10 -> DMEM mapper at 0x40 -> command buffer at 0x80.
struct SynthVbios { Bytes rom; uint32_t desc_addr, ucode_addr, interface_offset; };
inline void Image(Bytes& b, uint32_t base, uint16_t sig, const char* pcir, uint32_t size, uint8_t type, bool last) {
    P16(b, base, sig); P16(b, base + 0x18, 0x40);
    for (int i = 0; i < 4; ++i) P8(b, base + 0x40 + i, uint8_t(pcir[i]));
    P16(b, base + 0x4a, 0x18); P16(b, base + 0x50, uint16_t(size / 512)); P8(b, base + 0x54, type);
    P8(b, base + 0x55, last ? 0x80 : 0);
    const uint32_t n = (base + 0x40 + 0x18 + 0x0f) & ~0x0fu;
    for (int i = 0; i < 4; ++i) P8(b, n + i, uint8_t("NPDE"[i]));
    P16(b, n + 8, uint16_t(size / 512)); P8(b, n + 0x0a, last ? 0x80 : 0);
}
inline SynthVbios Vbios(bool with_e0 = true, int desc_version = 2) {
    SynthVbios v;
    Bytes& b = v.rom;
    b.assign(with_e0 ? 0x5000 : 0x2000, 0);
    Image(b, 0x0000, 0xaa55, "PCIR", 0x1000, 0x00, false);
    Image(b, 0x1000, 0xaa55, "PCIR", 0x1000, 0x03, !with_e0);
    if (with_e0) Image(b, 0x2000, 0x4e56, "NPDS", 0x3000, 0xe0, true);
    const uint32_t bit = 0x100;
    P8(b, bit, 0xff); P8(b, bit + 1, 0xb8); P8(b, bit + 2, 'B'); P8(b, bit + 3, 'I'); P8(b, bit + 4, 'T');
    P8(b, bit + 9, 6); P8(b, bit + 10, 2);
    P8(b, bit + 12, 'i'); P8(b, bit + 13, 1); P16(b, bit + 14, 0); P16(b, bit + 16, 0);
    P8(b, bit + 18, 'p'); P8(b, bit + 19, 2); P16(b, bit + 20, 4); P16(b, bit + 22, 0x200);
    P32(b, 0x200, 0x1100);                       // falcon ucode table ptr (needs remap)
    if (!with_e0) return v;
    const uint32_t t = 0x2100;                   // 0x1100 - 0x1000 + 0x2000
    P8(b, t, 1); P8(b, t + 1, 6); P8(b, t + 2, 6); P8(b, t + 3, 2);
    P8(b, t + 6, 0x01); P32(b, t + 8, 0);
    P8(b, t + 12, 0x85); P32(b, t + 14, 0x1200);
    v.desc_addr = 0x2200;
    v.interface_offset = 0x10;
    if (desc_version == 2) {
        const uint32_t d[15] = {(0x3cu << 16) | (2u << 8) | 1u, 0x500, 0x500, 0, 0x10, 0, 0x400, 0, 0x100,
                                0x300, 0x400, 0, 0x100, 0, 0};
        for (int i = 0; i < 15; ++i) P32(b, v.desc_addr + 4 * i, d[i]);
        v.ucode_addr = v.desc_addr + 0x3c;
        Fill(b, v.ucode_addr, 0x400, 0x66);
        const uint32_t dm = v.ucode_addr + 0x400;
        for (uint32_t i = 0; i < 0x100; ++i) P8(b, dm + i, 0);
        P8(b, dm + 0x10, 1); P8(b, dm + 0x11, 4); P8(b, dm + 0x12, 8); P8(b, dm + 0x13, 1);
        P32(b, dm + 0x14, 4); P32(b, dm + 0x18, 0x40);
        P32(b, dm + 0x40, 0x4e564d44); P32(b, dm + 0x48, 0x80); P32(b, dm + 0x4c, 0x40);
        P32(b, dm + 0x40 + 0x2c, 0xffffffff);
    } else {
        // v3: 44-byte header, one 384-byte signature, IMEM 0x400, DMEM 0x300, PKC at DMEM 0x100
        P32(b, v.desc_addr, (0x1acu << 16) | (3u << 8) | 1u);   // size 0x2c + 384
        const uint32_t d[8] = {0x700, 0x100, 0x10, 0, 0x400, 0, 0, 0x300};
        for (int i = 0; i < 8; ++i) P32(b, v.desc_addr + 4 + 4 * i, d[i]);
        P32(b, v.desc_addr + 36, 0x00010001u | (1u << 24));   // EngineIdMask 1, UcodeId 1, SignatureCount 1
        P32(b, v.desc_addr + 40, 0x0001);                     // SignatureVersions
        Fill(b, v.desc_addr + 0x2c, 384, 0x99);
        v.ucode_addr = v.desc_addr + 0x1ac;
        Fill(b, v.ucode_addr, 0x400, 0x66);
        const uint32_t dm = v.ucode_addr + 0x400;
        for (uint32_t i = 0; i < 0x300; ++i) P8(b, dm + i, 0);
        P8(b, dm + 0x10, 1); P8(b, dm + 0x11, 4); P8(b, dm + 0x12, 8); P8(b, dm + 0x13, 1);
        P32(b, dm + 0x14, 4); P32(b, dm + 0x18, 0x40);
        P32(b, dm + 0x48, 0x80); P32(b, dm + 0x4c, 0x40);
    }
    return v;
}

}  // namespace synth_fw
