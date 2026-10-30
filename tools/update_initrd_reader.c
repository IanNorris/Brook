/* Updater metadata inspection uses the same vendored FatFs parser as Brook,
 * built separately with an immutable RAM backend and no write interfaces. */
#include "update_initrd_reader.h"
#include <limits.h>
#include <string.h>
#include "ff.h"
#include "diskio.h"
static const uint8_t* active_image;
static size_t active_bytes;
static size_t read_budget;
static unsigned active;
static unsigned io_failed;
DSTATUS disk_initialize(BYTE drive){return drive||!active_image?STA_NOINIT|STA_NODISK:STA_PROTECT;}
DSTATUS disk_status(BYTE drive){return disk_initialize(drive);}
DRESULT disk_read(BYTE drive,BYTE* output,LBA_t sector,UINT count) {
    if(drive||!active_image||!output||!count||count>read_budget){io_failed=1;return RES_ERROR;}
    uint64_t start=(uint64_t)sector*512,length=(uint64_t)count*512;
    if(start>active_bytes||length>active_bytes-start){io_failed=1;return RES_PARERR;}
    read_budget-=count;memcpy(output,active_image+(size_t)start,(size_t)length);return RES_OK;
}
DRESULT disk_write(BYTE drive,const BYTE* input,LBA_t sector,UINT count) {
    (void)drive;(void)input;(void)sector;(void)count;return RES_WRPRT;
}
DRESULT disk_ioctl(BYTE drive,BYTE command,void* arg) {
    if(drive||!active_image)return RES_NOTRDY;
    if(command==CTRL_SYNC)return RES_OK;
    if(!arg)return RES_PARERR;
    if(command==GET_SECTOR_COUNT){*(LBA_t*)arg=(LBA_t)(active_bytes/512);return RES_OK;}
    if(command==GET_SECTOR_SIZE){*(WORD*)arg=512;return RES_OK;}
    if(command==GET_BLOCK_SIZE){*(DWORD*)arg=1;return RES_OK;}
    return RES_PARERR;
}
static int allowed(const char* path) {
    static const char* paths[]={"0:/ETC/boot-app-snapshot","0:/ETC/boot-app-image","0:/ETC/hardware-maintenance", "0:/BOOT/INIT.RC"};
    if(!path)return 0;
    for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);i++)if(!strcmp(path,paths[i]))return 1;
    return 0;
}
int BrookInitrdMetadata(const uint8_t* image,size_t image_bytes,const char* path,
                       uint8_t* output,size_t capacity,size_t* out_bytes) {
    if(out_bytes)*out_bytes=0;
    if(active||!image||image_bytes<4096||image_bytes>64u*1024u*1024u||(image_bytes&511)||
       !allowed(path)||!output||!capacity||capacity>4096||!out_bytes)return BROOK_INITRD_INVALID;
    /* Initrd is a FAT superfloppy, not a partition-table container. Bound the
     * declared medium before FatFs can accept metadata inside a truncated image. */
    if(image[510]!=0x55||image[511]!=0xaa||image[11]!=0||image[12]!=2)return BROOK_INITRD_INVALID;
    uint32_t sectors=(uint32_t)image[19]|(uint32_t)image[20]<<8;
    if(!sectors)sectors=(uint32_t)image[32]|(uint32_t)image[33]<<8|(uint32_t)image[34]<<16|(uint32_t)image[35]<<24;
    if(!sectors||sectors>image_bytes/512)return BROOK_INITRD_INVALID;
    active=1;io_failed=0;active_image=image;active_bytes=image_bytes;
    // Bound corrupt/cyclic directory/FAT traversal independently of FatFs's
    // own validation. Legitimate metadata walks fit within four media passes.
    read_budget=4*(image_bytes/512);
    FATFS fs;FIL file;memset(&fs,0,sizeof(fs));memset(&file,0,sizeof(file));
    int result=BROOK_INITRD_INVALID;int opened=0;
    FRESULT status=f_mount(&fs,"0:",1);
    if(status!=FR_OK)goto done;
    status=f_open(&file,path,FA_READ);
    if(status==FR_NO_FILE||status==FR_NO_PATH){result=BROOK_INITRD_MISSING;goto done;}
    if(status!=FR_OK)goto done;
    opened=1;
    if(f_size(&file)>capacity){result=BROOK_INITRD_TOO_LARGE;goto done;}
    UINT bytes=0;
    if(f_read(&file,output,(UINT)f_size(&file),&bytes)!=FR_OK||bytes!=f_size(&file))goto done;
    *out_bytes=bytes;result=BROOK_INITRD_OK;
done:
    if(opened&&f_close(&file)!=FR_OK){result=BROOK_INITRD_INVALID;*out_bytes=0;}
    // f_mount can retain the passed FATFS even when immediate mount fails.
    // Always detach its stack pointer before clearing this invocation's view.
    f_mount(NULL,"0:",0);
    if(io_failed){result=BROOK_INITRD_INVALID;*out_bytes=0;}
    active_image=NULL;active_bytes=0;read_budget=0;active=0;io_failed=0;
    return result;
}
