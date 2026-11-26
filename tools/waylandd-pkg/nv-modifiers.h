/* NVIDIA DRM format modifiers as Mesa NVK (nil/modifiers.rs, 26.0.6) builds them, for the
 * nv-drm (renderD130) dmabuf path. Layout of DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(c, s, g, k, h):
 *   bits 0..3   h  log2 of the block height in GOBs (0..5)
 *   bit  4      1  block linear
 *   bits 12..19 k  PTE kind
 *   bits 20..21 g  GOB kind version (0 Fermi, 1 G80, 2 Turing+)
 *   bit  22, 26..27 s  sector layout (0 Tegra K1, 1 desktop, 2/3 Blackwell 8/16 bpp)
 *   bits 23..25 c  compression (0 none)
 *   bits 56..63 0x03 vendor NVIDIA
 * Turing (TU10x, cls_eng3d TURING_A) colour targets: GOBType::TuringColor2D -> gob kind
 * version 2, desktop sector layout; pte kind NV_MMU_PTE_KIND_GENERIC_MEMORY (0x06) for every
 * colour format when uncompressed. NVK lists heights 5..0 (bigger first) then LINEAR, and
 * accepts only modifiers that match exactly (drm_format_mod_is_supported). */
#pragma once
#include <stdint.h>

#define BROOK_NV_MOD_VENDOR 0x03ull
#define BROOK_NV_TU_KIND_GENERIC 0x06u
#define BROOK_NV_TU_GOB_KIND_VERSION 2u
#define BROOK_NV_SECTOR_DESKTOP 1u
#define BROOK_NV_MOD_COUNT 6u   /* block-linear heights 5..0; LINEAR is separate */

static inline uint64_t brook_nv_block_linear_2d(uint32_t compression, uint32_t sector, uint32_t gob_kind,
                                                uint32_t kind, uint32_t height_log2)
{
    return (uint64_t)(height_log2 & 0xf) | (1ull << 4) | ((uint64_t)(kind & 0xff) << 12) |
           ((uint64_t)(gob_kind & 3) << 20) | ((uint64_t)(sector & 1) << 22) | ((uint64_t)(compression & 7) << 23) |
           ((uint64_t)((sector >> 1) & 3) << 26) | (BROOK_NV_MOD_VENDOR << 56);
}
/* Turing modifier i (0..5) in NVK's order: height_log2 = 5 - i. */
static inline uint64_t brook_nv_turing_modifier(unsigned i)
{
    return brook_nv_block_linear_2d(0, BROOK_NV_SECTOR_DESKTOP, BROOK_NV_TU_GOB_KIND_VERSION, BROOK_NV_TU_KIND_GENERIC, 5u - i);
}
struct brook_nv_block_linear { uint32_t height_log2, kind, gob_kind, sector, compression; };
/* 1 = a well-formed NVIDIA block-linear modifier (fields in *out), 0 = not one. */
static inline int brook_nv_parse_modifier(uint64_t m, struct brook_nv_block_linear *out)
{
    if ((m >> 56) != BROOK_NV_MOD_VENDOR || !(m & (1ull << 4))) return 0;
    if ((m >> 5) & 0x7f) return 0;                    /* reserved 5..11 */
    if ((m >> 28) & ((1ull << 28) - 1)) return 0;      /* reserved 28..55 */
    struct brook_nv_block_linear b = {
        (uint32_t)(m & 0xf), (uint32_t)((m >> 12) & 0xff), (uint32_t)((m >> 20) & 3),
        (uint32_t)(((m >> 22) & 1) | (((m >> 26) & 3) << 1)), (uint32_t)((m >> 23) & 7)};
    if (out) *out = b;
    return 1;
}
/* What NVK on Turing renders and imports for a colour format (exact match). */
static inline int brook_nv_turing_modifier_supported(uint64_t m)
{
    struct brook_nv_block_linear b;
    return brook_nv_parse_modifier(m, &b) && b.height_log2 <= 5 && b.kind == BROOK_NV_TU_KIND_GENERIC &&
           b.gob_kind == BROOK_NV_TU_GOB_KIND_VERSION && b.sector == BROOK_NV_SECTOR_DESKTOP && !b.compression;
}
/* Bytes of a block-linear 2D surface (one level, one layer), for bounds checks: GOBs are
 * 64 B x 8 rows; a block is 1 GOB wide and 2^h GOBs high. `width_bytes` = width * bpp. */
static inline uint64_t brook_nv_block_linear_bytes(uint32_t width_bytes, uint32_t height, uint32_t height_log2)
{
    const uint64_t block_rows = 8ull << height_log2;
    const uint64_t blocks_x = ((uint64_t)width_bytes + 63) / 64, blocks_y = ((uint64_t)height + block_rows - 1) / block_rows;
    return blocks_x * blocks_y * 512ull << height_log2;
}
