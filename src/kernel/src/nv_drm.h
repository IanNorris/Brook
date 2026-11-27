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
// The file's core under its lock (nullptr if the file has no backend); always unlock.
nvdrm::File* NvDrmFileLock(NvDrmFile* f);
void NvDrmFileUnlock(NvDrmFile* f);

// PRIME (nv_drm_prime.cpp, nv_drm_prime.h). renderD130 PRIME_HANDLE_TO_FD/FD_TO_HANDLE and
// SYNCOBJ_HANDLE_TO_FD/FD_TO_HANDLE; FdType::NvPrime descriptors hold one Shared reference.
int64_t NvDrmPrimeIoctl(NvDrmFile* f, uint64_t cmd, uint64_t arg);
void NvPrimeRef(void* shared);
void NvPrimeUnref(void* shared);
uint64_t NvPrimeBytes(void* shared);
uint64_t NvPrimeIdentity(void* shared);
int64_t NvPrimeFdIoctl(void* shared, uint64_t cmd, uint64_t arg);   // DMA_BUF_IOCTL_*
// Check an opaque, pitch-linear VRAM PRIME source up to length bytes. The
// caller must hold a reference and keep its producer idle through the copy.
bool NvPrimeLinearVram(void* shared, uint64_t length, uint64_t* phys);
bool NvPrimeMapGart(void* shared, uint64_t offset, uint64_t length, uint64_t* phys, MappingBacking** life);
}
