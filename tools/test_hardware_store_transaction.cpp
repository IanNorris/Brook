#include "hardware_store_transaction.h"
#include "../src/kernel/src/writable_desktop_plan.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
int main(int argc,char **argv) {
    assert(argc==4);int image=open(argv[1],O_RDONLY);assert(image>=0);
    for(size_t n=0;n<strlen(argv[2]);++n){brook::WritableDesktopPlan partial{};assert(!brook::WritableDesktopPlanParse(argv[2],n,partial));}
    struct stat st{};assert(!fstat(image,&st));uint8_t buffer[65536],digest[32];BrookSha256 hash{};BrookShaInit(&hash);
    for(;;){ssize_t n=read(image,buffer,sizeof(buffer));assert(n>=0);if(!n)break;BrookShaAdd(&hash,buffer,n);}BrookShaFinish(&hash,digest);
    assert(!mkdir(argv[3],0700));int dir=open(argv[3],O_RDONLY|O_DIRECTORY);assert(dir>=0);
    {
        HardwareStoreTransaction t;assert(!t.begin(dir,argv[2],strlen(argv[2]),digest));
        assert(!lseek(image,0,SEEK_SET));for(;;){ssize_t n=read(image,buffer,sizeof(buffer));assert(n>=0);if(!n)break;assert(!t.append(buffer,n));}
        assert(!t.stage());struct stat marker{};assert(fstatat(dir,"STORE.OK",&marker,0)<0&&errno==ENOENT);
        assert(!t.marker_published()&&!t.marker_durable());
        assert(!t.finalize());assert(t.marker_published()&&t.marker_durable());assert(!fstatat(dir,"STORE.OK",&marker,0));assert(t.finalize()==-EINVAL);
    }
    {HardwareStoreTransaction t;assert(t.begin(dir,argv[2],strlen(argv[2]),digest)==-EEXIST);}
    assert(!unlinkat(dir,"STORE.OK",0));assert(!unlinkat(dir,"STORE.IMG",0));
#ifdef BROOK_STORE_TEST
    for (const char *point : {"before-create", "after-create"}) {
        HardwareStoreTransaction t;assert(!t.begin(dir,argv[2],strlen(argv[2]),digest));
        assert(!lseek(image,0,SEEK_SET));for(;;){ssize_t n=read(image,buffer,sizeof(buffer));assert(n>=0);if(!n)break;assert(!t.append(buffer,n));}
        assert(!t.stage());assert(!setenv("BROOK_STORE_TEST_FAILURE",point,1));assert(t.finalize()==-EIO);unsetenv("BROOK_STORE_TEST_FAILURE");
        bool published=!strcmp(point,"after-create");assert(t.marker_published()==published&&!t.marker_durable());
        struct stat marker{};if(published){assert(!fstatat(dir,"STORE.OK",&marker,0));assert(!unlinkat(dir,"STORE.OK",0));}
        else assert(fstatat(dir,"STORE.OK",&marker,0)<0&&errno==ENOENT);
        assert(!unlinkat(dir,"STORE.IMG",0));
    }
#endif
    {
        uint8_t wrong[32];memcpy(wrong,digest,32);wrong[0]^=1;HardwareStoreTransaction t;
        assert(!t.begin(dir,argv[2],strlen(argv[2]),wrong));assert(!lseek(image,0,SEEK_SET));
        for(;;){ssize_t n=read(image,buffer,sizeof(buffer));assert(n>=0);if(!n)break;assert(!t.append(buffer,n));}
        assert(t.stage()==-EBADMSG);assert(t.stage()==-EINVAL);assert(t.finalize()==-EINVAL);
        struct stat marker{};assert(fstatat(dir,"STORE.OK",&marker,0)<0&&errno==ENOENT);
    }
    assert(!unlinkat(dir,"STORE.IMG",0));assert(!symlinkat("missing",dir,"STORE.OK"));
    {HardwareStoreTransaction t;assert(t.begin(dir,argv[2],strlen(argv[2]),digest)==-EEXIST);}
    assert(!unlinkat(dir,"STORE.OK",0));close(dir);close(image);assert(!rmdir(argv[3]));puts("store transaction checks passed");
}
