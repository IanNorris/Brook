#pragma once
#include "payload.h"

#define BROOK_BOOT_COMPRESSED_MAGIC "BRKCMP01"
#define BROOK_BOOT_COMPRESSED_MAGIC_BYTES 8u

static inline int BrookBootCompressed(const uint8_t* file, BrookUpdateSize bytes) {
    return file && bytes >= BROOK_BOOT_COMPRESSED_MAGIC_BYTES &&
        BrookUpdateEqual(file, (const uint8_t*)BROOK_BOOT_COMPRESSED_MAGIC,
                         BROOK_BOOT_COMPRESSED_MAGIC_BYTES);
}
