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
FRESULT f_object_mkdir(const FF_OBJECT*,const TCHAR* name);
/* Remove one child of the retained directory. Caller excludes live aliases. */
FRESULT f_object_unlink(const FF_OBJECT*,const TCHAR* name);
/* Move to an absent child name; callers enforce alias and cycle checks. */
FRESULT f_object_rename(const FF_OBJECT*,const TCHAR*,const FF_OBJECT*,const TCHAR*);
// Fixed-image CLMT: bounded by file length and caller capacity, with strict
// chain validation. Unlike upstream CREATE_LINKMAP, cycles cannot hang it.
FRESULT f_object_image_map(FIL*,DWORD* table,UINT entries);
// Snapshot a bounded fixed-file sector map relative to its bound disk. This
// does not prove exclusive ownership against crosslinked third-party files.
FRESULT f_object_image_sectors(FIL*,UINT map_entries,uint64_t* out,UINT sectors);
// Caller holds the FAT mutex and owns a synced, mapped 128-byte fixed file.
// Reads its first sector through diskio, bypassing the FIL and FAT windows.
FRESULT f_object_guard_sector(FIL*,LBA_t* out);
FRESULT f_object_guard_readback(FIL*,BYTE out[128]);

#ifdef __cplusplus
}
#endif
