#pragma once
#include <stdint.h>
// Root-only, boot-opted-in, read-only AHCI export. The boot-bound control
// session must independently pin boot identity; this metadata identifies the
// currently initialized disk but is not an authorization token.
enum { BROOK_SYS_AHCI_EXPORT=0xB0F, BROOK_AHCI_EXPORT_CHUNK=65536, BROOK_AHCI_EXPORT_VERSION=1 };
struct BrookAhciExportInfo {
    uint32_t version, logical_sector_bytes, physical_sector_bytes, port;
    uint64_t sectors;
    char model[41], serial[21];
    uint8_t reserved[2];
};
