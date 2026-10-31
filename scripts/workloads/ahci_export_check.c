#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "brook_ahci_export.h"
#include "update/payload.h"

static long export_call(uint64_t op,void* out,uint64_t bytes,uint64_t offset) {
    return syscall(BROOK_SYS_AHCI_EXPORT,op,out,bytes,offset,0,0);
}
int main(int argc,char** argv) {
    struct BrookAhciExportInfo info={0};
    if(argc==2 && !strcmp(argv[1],"denied")) {
        errno=0;long rc=export_call(0,&info,sizeof(info),0);
        if(rc!=-1 || errno!=EPERM)return 1;
        puts("AHCI_EXPORT_UNPRIVILEGED_DENIED");return 0;
    }
    if(argc==2 && !strcmp(argv[1],"disabled")) {
        errno=0;long rc=export_call(0,&info,sizeof(info),0);
        if(rc!=-1 || errno!=EROFS)return 8;
        puts("AHCI_EXPORT_DISABLED");return 0;
    }
    if(argc!=1 || export_call(0,&info,sizeof(info),0)!=0 || info.version!=BROOK_AHCI_EXPORT_VERSION ||
       info.logical_sector_bytes!=512 || !info.sectors || info.sectors>(UINT64_MAX/512))return 2;
    uint8_t buffer[BROOK_AHCI_EXPORT_CHUNK],hash[32];const uint64_t bytes=info.sectors*512;
    errno=0;if(export_call(1,buffer,512,bytes)!=-1 || errno!=EINVAL)return 3;
    errno=0;if(export_call(1,buffer,512,1)!=-1 || errno!=EINVAL)return 4;
    errno=0;if(export_call(1,buffer,0,0)!=-1 || errno!=EINVAL)return 5;
    errno=0;if(export_call(1,NULL,512,0)!=-1 || errno!=EFAULT)return 6;
    errno=0;if(export_call(1,buffer,BROOK_AHCI_EXPORT_CHUNK+512,0)!=-1 || errno!=EINVAL)return 9;
    errno=0;if(syscall(BROOK_SYS_AHCI_EXPORT,0,&info,sizeof(info),0,1,0)!=-1 || errno!=EINVAL)return 10;
    if(export_call(1,buffer,512,bytes-512)!=512)return 11;
    BrookSha256 sha;BrookShaInit(&sha);
    for(uint64_t pos=0;pos<bytes;) {
        uint32_t n=(uint32_t)((bytes-pos)>sizeof(buffer)?sizeof(buffer):(bytes-pos));
        if(export_call(1,buffer,n,pos)!=(long)n)return 7;
        BrookShaAdd(&sha,buffer,n);pos+=n;
    }
    BrookShaFinish(&sha,hash);
    printf("AHCI_EXPORT_HASH bytes=%"PRIu64" sha256=",bytes);
    for(unsigned i=0;i<32;++i)printf("%02x",hash[i]);
    puts("");return 0;
}
