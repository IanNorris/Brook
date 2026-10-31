#pragma once
#include "ata_identify.h"
#include <string.h>
namespace brook {
// Only the disposable test kernel uses this gate. It cannot authorize a disk
// in a production kernel, and is not an installation-target identity check.
inline bool AhciDisposableTestAdmission(bool marker,bool kvm,uint32_t port,
                                        const AtaIdentifyInfo& info,bool flushExt) {
    return marker && kvm && port==0 && info.sectors==131072 &&
        !strcmp(info.model,"QEMU HARDDISK") && !strcmp(info.serial,"BROOK-AHCI-TEST-01") && flushExt;
}
}
