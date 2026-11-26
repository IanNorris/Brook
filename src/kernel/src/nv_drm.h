#pragma once
// M5.1: /dev/dri/renderD130, the nouveau uAPI subset for Mesa NVK (nv_drm_core.h). Enabled
// only after GSP-RM publishes the device (NvDrmPublish) and with /etc/nv-drm present; on
// every other image it stays off and all of these refuse.
#include <stdint.h>
#include "nv_drm_core.h"

namespace brook {
void NvDrmPublish(const nvdrm::DeviceInfo& info);   // nv_gsp_live, once RM is up
void NvDrmRetract();                                 // before RM unload
bool NvDrmNodeEnabled();
const char* NvDrmSysAttrContent(const char* path);   // /sys/dev/char/226:130/device/*
bool NvDrmSysIsDir(const char* path);
struct NvDrmFile;
NvDrmFile* NvDrmFileCreate();
void NvDrmFileRef(NvDrmFile* f);
void NvDrmFileUnref(NvDrmFile* f);
int64_t NvDrmIoctl(NvDrmFile* f, uint64_t cmd, uint64_t arg);
}
