/* Brook object adapter, included after the unmodified FatFs translation unit.
 * It uses the same configuration and LFN workspace as that implementation.
 * No process-global cdir changes and no reconstruction of an opening path.
 */
#include "fatfs_object.h"
#include <stdint.h>

/* These are Brook's current configurations. Changing them requires adapting
 * share-lock/volume-lock/exFAT allocation handling, never silently omitting it.
 */
#if FF_FS_EXFAT || FF_FS_LOCK || FF_FS_REENTRANT || FF_FS_READONLY
#error "Adapt Brook's retained-object adapter before changing FatFs lifetime/configuration"
#endif

static FRESULT object_validate (const FF_OBJECT* object, FATFS** fs)
{
    FFOBJID validation;
    if (!object) return FR_INVALID_OBJECT;
    validation = object->object;
    return validate(&validation, fs);
}

FRESULT f_object_root (FATFS* fs, FF_OBJECT* out)
{
    if (!out) return FR_INVALID_PARAMETER;
    memset(out, 0, sizeof *out);
    if (!fs || !fs->fs_type) return FR_INVALID_OBJECT;
    out->object.fs = fs;
    out->object.id = fs->id;
    out->object.attr = AM_DIR;
    return FR_OK;
}

FRESULT f_object_lookup (const FF_OBJECT* parent, const TCHAR* name, FF_OBJECT* out)
{
    FATFS* fs;
    FRESULT res;
    DIR directory;
    const TCHAR* cursor = name;
    unsigned length = 0;
    DEF_NAMEBUFF
    if (!out) return FR_INVALID_PARAMETER;
    memset(out, 0, sizeof *out);
    if (!name || !name[0]) return FR_INVALID_NAME;
    for (; name[length]; ++length) {
        if (IsSeparator(name[length]) || name[length] == ':' ||
            (unsigned)name[length] < 0x20 || length >= 255) return FR_INVALID_NAME;
    }
    if ((name[0] == '.' && !name[1]) ||
        (name[0] == '.' && name[1] == '.' && !name[2])) return FR_INVALID_NAME;
    res = object_validate(parent, &fs);
    if (res != FR_OK) return res;
    if (!(parent->object.attr & AM_DIR)) return FR_NO_PATH;
    memset(&directory, 0, sizeof directory);
    directory.obj.fs = fs;
    directory.obj.sclust = parent->object.sclust;
    INIT_NAMEBUFF(fs);
    res = create_name(&directory, &cursor);
    if (res == FR_OK && !(directory.fn[NSFLAG] & NS_LAST)) res = FR_INVALID_NAME;
    if (res == FR_OK) res = dir_find(&directory);
    if (res == FR_OK) {
        out->object.fs = fs;
        out->object.id = fs->id;
        out->object.attr = directory.obj.attr;
        out->object.sclust = ld_clust(fs, directory.dir);
        out->object.objsize = ld_32(directory.dir + DIR_FileSize);
        out->parent_cluster = parent->object.sclust;
        out->directory_offset = directory.dptr;
        out->entry_sector = directory.sect;
        out->entry_offset = (WORD)(directory.dptr % SS(fs));
    }
    FREE_NAMEBUFF();
    return res;
}

FRESULT f_object_from_directory (const DIR* directory, FF_OBJECT* out)
{
    FATFS* fs;
    FRESULT res;
    FFOBJID validation;
    if (!out) return FR_INVALID_PARAMETER;
    memset(out, 0, sizeof *out);
    if (!directory) return FR_INVALID_OBJECT;
    validation = directory->obj;
    res = validate(&validation, &fs);
    if (res != FR_OK) return res;
    out->object.fs = fs;
    out->object.id = fs->id;
    out->object.sclust = directory->obj.sclust;
    out->object.attr = AM_DIR;
    return FR_OK;
}

FRESULT f_object_from_file (const FIL* file, FF_OBJECT* out)
{
    FATFS* fs;
    FRESULT res;
    uintptr_t offset;
    FFOBJID validation;
    if (!out) return FR_INVALID_PARAMETER;
    memset(out, 0, sizeof *out);
    if (!file) return FR_INVALID_OBJECT;
    validation = file->obj;
    res = validate(&validation, &fs);
    if (res != FR_OK) return res;
    offset = (uintptr_t)file->dir_ptr - (uintptr_t)fs->win;
    if (!file->dir_sect || offset >= SS(fs) || offset % SZDIRE) return FR_INVALID_OBJECT;
    out->object = file->obj;
    out->entry_sector = file->dir_sect;
    out->entry_offset = (WORD)offset;
    return FR_OK;
}

FRESULT f_object_info (const FF_OBJECT* object, FF_OBJECT* out)
{
    FATFS* fs;
    FRESULT res;
    BYTE* entry;
    FF_OBJECT result;
    if (!out) return FR_INVALID_PARAMETER;
    /* Permit in-place refresh without clearing the borrowed input first. */
    if (!object) { memset(out, 0, sizeof *out); return FR_INVALID_OBJECT; }
    result = *object;
    res = object_validate(object, &fs);
    if (res != FR_OK) { memset(out, 0, sizeof *out); return res; }
    if (object->object.attr & AM_DIR) { *out = result; return FR_OK; }
    if (!object->entry_sector || object->entry_offset > SS(fs) - SZDIRE ||
        object->entry_offset % SZDIRE) { memset(out, 0, sizeof *out); return FR_INVALID_OBJECT; }
    res = move_window(fs, object->entry_sector);
    if (res != FR_OK) { memset(out, 0, sizeof *out); return res; }
    entry = fs->win + object->entry_offset;
    if (!entry[DIR_Name] || entry[DIR_Name] == DDEM || (entry[DIR_Attr] & (AM_DIR | AM_VOL))) {
        memset(out, 0, sizeof *out); return FR_INVALID_OBJECT;
    }
    result.object.attr = entry[DIR_Attr];
    result.object.sclust = ld_clust(fs, entry);
    result.object.objsize = ld_32(entry + DIR_FileSize);
    *out = result;
    return FR_OK;
}

FRESULT f_object_parent (const FF_OBJECT* object, FF_OBJECT* out)
{
    FATFS* fs;
    FRESULT res;
    DIR directory;
    DWORD cluster;
    if (!out) return FR_INVALID_PARAMETER;
    memset(out, 0, sizeof *out);
    res = object_validate(object, &fs);
    if (res != FR_OK) return res;
    if (!(object->object.attr & AM_DIR)) return FR_NO_PATH;
    if (!object->object.sclust) return f_object_root(fs, out);
    memset(&directory, 0, sizeof directory);
    directory.obj.fs = fs;
    directory.obj.sclust = object->object.sclust;
    res = dir_sdi(&directory, SZDIRE);
    if (res == FR_OK) res = move_window(fs, directory.sect);
    if (res != FR_OK) return res;
    if (memcmp(directory.dir, "..         ", 11) || !(directory.dir[DIR_Attr] & AM_DIR)) return FR_INT_ERR;
    cluster = ld_clust(fs, directory.dir);
    if (fs->fs_type == FS_FAT32 && cluster == (DWORD)fs->dirbase) cluster = 0;
    if (cluster && (cluster < 2 || cluster >= fs->n_fatent)) return FR_INT_ERR;
    out->object.fs = fs;
    out->object.id = fs->id;
    out->object.attr = AM_DIR;
    out->object.sclust = cluster;
    return FR_OK;
}

FRESULT f_object_opendir (const FF_OBJECT* object, DIR* out)
{
    FATFS* fs;
    FRESULT res;
    if (!out) return FR_INVALID_PARAMETER;
    memset(out, 0, sizeof *out);
    res = object_validate(object, &fs);
    if (res != FR_OK) return res;
    if (!(object->object.attr & AM_DIR)) return FR_NO_PATH;
    out->obj = object->object;
    res = dir_sdi(out, 0);
    if (res != FR_OK) out->obj.fs = 0;
    return res;
}

FRESULT f_object_open (const FF_OBJECT* object, FIL* out, BYTE mode)
{
    FF_OBJECT current;
    FRESULT res;
    if (!out) return FR_INVALID_PARAMETER;
    memset(out, 0, sizeof *out);
    if (!(mode & (FA_READ | FA_WRITE)) || (mode & ~(FA_READ | FA_WRITE))) return FR_INVALID_PARAMETER;
    res = f_object_info(object, &current);
    if (res != FR_OK) return res;
    if (current.object.attr & AM_DIR) return FR_DENIED;
    if ((current.object.attr & AM_RDO) && (mode & FA_WRITE)) return FR_DENIED;
    out->obj = current.object;
    out->dir_sect = current.entry_sector;
    out->dir_ptr = current.object.fs->win + current.entry_offset;
    out->flag = mode;
    /* fptr, sect, err, sector buffer and configured fast-seek state are zeroed
       just as on an ordinary existing-file f_open. No create/truncate/append
       flags or share locks are acquired here. */
    return FR_OK;
}

FRESULT f_object_image_map (FIL* fp, DWORD* table, UINT entries)
{
#if FF_USE_FASTSEEK
    FATFS* fs;
    FRESULT result;
    DWORD cluster, previous = 0, run_start = 0, run_length = 0, steps, needed;
    UINT at = 1;
    if (!fp || !table || entries < 4 || fp->cltbl) return FR_INVALID_PARAMETER;
    result = validate(&fp->obj, &fs);
    if (result != FR_OK) return result;
    if (fp->err) return (FRESULT)fp->err;
    if ((fs->fs_type != FS_FAT16 && fs->fs_type != FS_FAT32) ||
        !fs->csize || !fp->obj.objsize) return FR_INVALID_PARAMETER;
    needed = (DWORD)((fp->obj.objsize - 1) / ((FSIZE_t)fs->csize * SS(fs)) + 1);
    if (needed > fs->n_fatent - 2) return FR_INT_ERR;
    cluster = fp->obj.sclust;
    for (steps = 0; steps < needed; ++steps) {
        DWORD next;
        const DWORD reserved = fs->fs_type == FS_FAT16 ? 0xfff0 : 0x0ffffff0;
        const DWORD eoc = fs->fs_type == FS_FAT16 ? 0xfff8 : 0x0ffffff8;
        if (cluster < 2 || cluster >= fs->n_fatent || cluster >= reserved) return FR_INT_ERR;
        if (!run_length) {
            if (at + 2 >= entries) return FR_NOT_ENOUGH_CORE;
            run_start = cluster; run_length = 1;
        } else if (cluster == previous + 1) {
            ++run_length;
        } else {
            table[at++] = run_length; table[at++] = run_start;
            if (at + 2 >= entries) return FR_NOT_ENOUGH_CORE;
            run_start = cluster; run_length = 1;
        }
        next = get_fat(&fp->obj, cluster);
        if (next == 0xffffffff) return FR_DISK_ERR;
        if (steps + 1 == needed) {
            if (next < eoc) return FR_INT_ERR; /* no hidden allocation tail */
        } else {
            if (next < 2 || next >= fs->n_fatent || next >= reserved ||
                next == cluster || (steps && next == previous)) return FR_INT_ERR;
        }
        previous = cluster; cluster = next;
    }
    table[at++] = run_length; table[at++] = run_start; table[at++] = 0;
    table[0] = at; fp->cltbl = table;
    return FR_OK;
#else
    (void)fp; (void)table; (void)entries;
    return FR_INVALID_PARAMETER;
#endif
}
