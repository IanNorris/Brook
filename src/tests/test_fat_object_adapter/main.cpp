#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include "fatfs_object.h"
#include "diskio.h"
static std::vector<unsigned char> disk;
static bool failRead=false;
extern "C" DSTATUS disk_initialize(BYTE drive){return drive?STA_NOINIT:0;}
extern "C" DSTATUS disk_status(BYTE drive){return drive?STA_NOINIT:0;}
extern "C" DRESULT disk_read(BYTE drive,BYTE* out,LBA_t sector,UINT count){
    uint64_t offset=uint64_t(sector)*512,bytes=uint64_t(count)*512;
    if(drive || failRead || offset>disk.size() || bytes>disk.size()-offset)return RES_ERROR;
    std::memcpy(out,disk.data()+offset,bytes);return RES_OK;
}
extern "C" DRESULT disk_write(BYTE drive,const BYTE* in,LBA_t sector,UINT count){
    uint64_t offset=uint64_t(sector)*512,bytes=uint64_t(count)*512;
    if(drive || offset>disk.size() || bytes>disk.size()-offset)return RES_ERROR;
    std::memcpy(disk.data()+offset,in,bytes);return RES_OK;
}
extern "C" DRESULT disk_ioctl(BYTE drive,BYTE command,void* out){
    if(drive)return RES_PARERR;
    if(command==CTRL_SYNC)return RES_OK;
    if(command==GET_SECTOR_COUNT){*static_cast<LBA_t*>(out)=disk.size()/512;return RES_OK;}
    if(command==GET_SECTOR_SIZE){*static_cast<WORD*>(out)=512;return RES_OK;}
    if(command==GET_BLOCK_SIZE){*static_cast<DWORD*>(out)=1;return RES_OK;}
    return RES_PARERR;
}
extern "C" DWORD get_fattime(){return (DWORD(46)<<25)|(DWORD(10)<<21)|(DWORD(22)<<16);}
static void makeFile(const char* path,const char* contents){
    FIL file{};assert(f_open(&file,path,FA_WRITE|FA_CREATE_NEW)==FR_OK);
    UINT wrote=0;assert(f_write(&file,contents,std::strlen(contents),&wrote)==FR_OK && wrote==std::strlen(contents));assert(f_close(&file)==FR_OK);
}
int main(int argc,char** argv){
    assert(argc==2);std::ifstream input(argv[1],std::ios::binary);assert(input);
    disk.assign(std::istreambuf_iterator<char>(input),{});input.close();
    FATFS fs{};assert(f_mount(&fs,"0:",1)==FR_OK);
    assert(f_mkdir("0:/dir")==FR_OK);assert(f_mkdir("0:/parent")==FR_OK);
    makeFile("0:/dir/meaningfully-long-name.txt","original");makeFile("0:/dir/empty","");
    FF_OBJECT root{},held{},file{},empty{},after{},parent{};
    assert(f_object_root(&fs,&root)==FR_OK);
    assert(f_object_lookup(&root,"dir",&held)==FR_OK);
    assert(f_object_lookup(&held,"meaningfully-long-name.txt",&file)==FR_OK);
    assert(f_object_lookup(&held,"empty",&empty)==FR_OK && empty.object.sclust==0);
    assert(file.entry_sector!=empty.entry_sector || file.entry_offset!=empty.entry_offset);
    // Rename keeps the original directory cluster authoritative; an adversarial
    // directory at the opening name must not affect direct child lookup.
    assert(f_rename("0:/dir","0:/parent/moved")==FR_OK);assert(f_mkdir("0:/dir")==FR_OK);
    makeFile("0:/dir/meaningfully-long-name.txt","replacement");
    assert(f_object_lookup(&held,"meaningfully-long-name.txt",&after)==FR_OK);
    assert(file.entry_sector==after.entry_sector && file.entry_offset==after.entry_offset);
    assert(f_object_parent(&held,&parent)==FR_OK);
    FF_OBJECT expectedParent{};assert(f_object_lookup(&root,"parent",&expectedParent)==FR_OK);
    assert(parent.object.sclust==expectedParent.object.sclust);
    FIL first{},second{};assert(f_object_open(&file,&first,FA_READ|FA_WRITE)==FR_OK);
    assert(f_object_open(&file,&second,FA_READ)==FR_OK);
    char bytes[32]{};UINT count=0;
    assert(f_read(&first,bytes,8,&count)==FR_OK && count==8 && !std::memcmp(bytes,"original",8));
    assert(f_read(&second,bytes,8,&count)==FR_OK && count==8 && !std::memcmp(bytes,"original",8));
    assert(f_write(&first,"-tail",5,&count)==FR_OK && count==5);assert(f_sync(&first)==FR_OK);
    assert(f_object_info(&file,&after)==FR_OK && after.object.objsize==13);
    assert(f_close(&second)==FR_OK && f_close(&first)==FR_OK);
    assert(f_object_open(&file,&first,FA_READ)==FR_OK);
    assert(f_read(&first,bytes,13,&count)==FR_OK && count==13 && !std::memcmp(bytes,"original-tail",13));
    FF_OBJECT fromFile{};assert(f_object_from_file(&first,&fromFile)==FR_OK && fromFile.entry_sector==file.entry_sector && fromFile.entry_offset==file.entry_offset);
    assert(f_close(&first)==FR_OK);
    DIR directoryA{},directoryB{};assert(f_object_opendir(&held,&directoryA)==FR_OK && f_object_opendir(&held,&directoryB)==FR_OK);
    FILINFO a{},b{};assert(f_readdir(&directoryA,&a)==FR_OK && f_readdir(&directoryB,&b)==FR_OK && !std::strcmp(a.fname,b.fname));
    assert(f_closedir(&directoryA)==FR_OK && f_closedir(&directoryB)==FR_OK);
    assert(f_object_lookup(&file,"child",&after)==FR_NO_PATH && !after.object.fs);
    for(const char* invalid: {".","..","child/name","child\\name","0:name","name\nrest"})assert(f_object_lookup(&held,invalid,&after)==FR_INVALID_NAME && !after.object.fs);
    FF_OBJECT stale=file;--stale.object.id;
    assert(f_object_open(&stale,&first,FA_READ)==FR_INVALID_OBJECT && !first.obj.fs);
    assert(f_object_open(&file,&first,FA_CREATE_ALWAYS|FA_WRITE)==FR_INVALID_PARAMETER && !first.obj.fs);
    fs.winsect=LBA_t(-1);failRead=true;
    assert(f_object_lookup(&held,"empty",&after)==FR_DISK_ERR && !after.object.fs);
    assert(f_object_open(&file,&first,FA_READ)==FR_DISK_ERR && !first.obj.fs);
    failRead=false;
    assert(f_mount(nullptr,"0:",0)==FR_OK);
    std::ofstream output(argv[1],std::ios::binary|std::ios::trunc);output.write(reinterpret_cast<char*>(disk.data()),disk.size());assert(output);output.close();
    std::puts("FAT_OBJECT_ADAPTER_PASS component-identity/rename/LFN/parent/exact-open/offsets/fsync/errors");
}
