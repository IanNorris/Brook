#pragma once
#include <stdint.h>

// Auto-generated FAT16 test image embedded in the kernel binary.
// Contains: BROOK.CFG (root), TEST/HELLO.TXT (subdirectory).
// Generated at build time by scripts/make_fat_image.py.

extern const uint8_t  g_fatTestImage[];
extern const uint32_t g_fatTestImageSize;
