#pragma once
#include "ff.h"
#ifdef __cplusplus
extern "C" {
#endif

// Backend coordinates, never a pathname or a pointer into FATFS::win. The VFS
// adapter must pin live objects against unlink/replacement/entry reuse for the
// entire lifetime of these coordinates. All calls require its FatFS mutex.
typedef struct {
    FFOBJID object;
    DWORD parent_cluster;
    DWORD directory_offset;
    LBA_t entry_sector;
    WORD entry_offset;
} FF_OBJECT;
FRESULT f_object_root(FATFS*,FF_OBJECT*);
FRESULT f_object_lookup(const FF_OBJECT* directory,const TCHAR* name,FF_OBJECT*);
FRESULT f_object_parent(const FF_OBJECT* directory,FF_OBJECT*);
FRESULT f_object_from_directory(const DIR*,FF_OBJECT*);
FRESULT f_object_from_file(const FIL*,FF_OBJECT*);
FRESULT f_object_info(const FF_OBJECT*,FF_OBJECT*);
FRESULT f_object_opendir(const FF_OBJECT*,DIR*);
FRESULT f_object_open(const FF_OBJECT*,FIL*,BYTE mode);
/* Exclusively create one child relative to the exact retained directory. */
FRESULT f_object_create(const FF_OBJECT*,const TCHAR* name,FIL*,BYTE mode);
// Fixed-image CLMT: bounded by file length and caller capacity, with strict
// chain validation. Unlike upstream CREATE_LINKMAP, cycles cannot hang it.
FRESULT f_object_image_map(FIL*,DWORD* table,UINT entries);

#ifdef __cplusplus
}
#endif
