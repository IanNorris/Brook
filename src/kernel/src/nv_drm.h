#pragma once
// M5.1: /dev/dri/renderD130, the nouveau uAPI subset for Mesa NVK (nv_drm_core.h). Enabled
// only after GSP-RM publishes the device (NvDrmPublish) and with /etc/nv-drm present; on
// every other image it stays off and all of these refuse.
#include <stdint.h>
#include "nv_drm_core.h"

namespace brook {
struct MappingBacking;
// GART BO pages (live backend sys_alloc cookie): contiguous, freed when `life` drops to zero
// (the BO and every CPU mapping of it hold a reference).
struct NvDrmSysPages { uint64_t phys, bytes; MappingBacking* life; };
void NvDrmPublish(const nvdrm::DeviceInfo& info, const nvdrm::Backend* backend);   // nv_gsp_live, once RM is up
void NvDrmRetract();                                 // before RM unload
bool NvDrmNodeEnabled();
const char* NvDrmSysAttrContent(const char* path);   // /sys/dev/char/226:130/device/*
bool NvDrmSysIsDir(const char* path);
struct NvDrmFile;
NvDrmFile* NvDrmFileCreate();
void NvDrmFileRef(NvDrmFile* f);
void NvDrmFileUnref(NvDrmFile* f);
int64_t NvDrmIoctl(NvDrmFile* f, uint64_t cmd, uint64_t arg);
// mmap(fd, offset): the GART BO behind a GEM map token. Returns the physical base for
// [offset, offset+length) and a referenced lifetime (the caller's mapping drops it), or false
// (unknown token, past the BO, or a VRAM BO: no CPU-visible VRAM yet).
bool NvDrmMapGart(NvDrmFile* f, uint64_t offset, uint64_t length, uint64_t* phys, MappingBacking** life);
}
