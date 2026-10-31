#include "hardware_store_transaction.h"
#include "hardware_store_validate.h"
#include "../src/kernel/src/writable_desktop_plan.h"
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failure() { return errno ? -errno : -EIO; }
static int write_all(int fd, const void *data, size_t bytes) {
    const auto *p=static_cast<const uint8_t *>(data);
    while(bytes) { ssize_t n=write(fd,p,bytes); if(n<0 && errno==EINTR)continue;
        if(n<=0)return failure(); p+=n; bytes-=static_cast<size_t>(n); }
    return 0;
}
static int absent(int dir,const char *name) {
    struct stat st{}; if(!fstatat(dir,name,&st,AT_SYMLINK_NOFOLLOW))return -EEXIST;
    return errno==ENOENT?0:failure();
}
HardwareStoreTransaction::~HardwareStoreTransaction() { if(file_>=0)close(file_); }
int HardwareStoreTransaction::begin(int directory,const char *record,size_t length,const uint8_t digest[32]) {
    if(attempted_)return -EINVAL; attempted_=true;
    brook::WritableDesktopPlan p{};
    if(!digest || length>96 || !brook::WritableDesktopPlanParse(record,static_cast<uint32_t>(length),p))return -EINVAL;
    int e=absent(directory,"STORE.IMG");if(e)return e;
    e=absent(directory,"STORE.OK");if(e)return e;
    e=absent(directory,"STORE.TMP");if(e)return e;
    file_=openat(directory,"STORE.IMG",O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0644);
    if(file_<0)return failure();
    created_=true;
    struct stat st{};if(fstat(file_,&st) || !S_ISREG(st.st_mode)){failed_=true;return -EIO;}
    device_=st.st_dev;inode_=st.st_ino;
    directory_=directory; size_=p.bytes; record_length_=length;
    memcpy(record_,record,length);memcpy(digest_,digest,32);BrookShaInit(&stream_);begun_=true;
    return 0;
}
int HardwareStoreTransaction::append(const void *data,size_t length) {
    if(!begun_||failed_||staged_||!data||!length||length>65536||length>size_-written_)return -EINVAL;
    int e=write_all(file_,data,length);if(e){failed_=true;return e;}
    BrookShaAdd(&stream_,static_cast<const uint8_t *>(data),length);written_+=length;return 0;
}
int HardwareStoreTransaction::stage() {
    if(!begun_||failed_||staged_||written_!=size_)return -EINVAL;
    failed_=true; // every stage failure is terminal; never retry an uncertain write
    uint8_t hash[32];BrookShaFinish(&stream_,hash);
    if(!BrookUpdateEqual(hash,digest_,32))return -EBADMSG;
    if(fsync(file_))return failure();
    if(close(file_)){file_=-1;return failure();}file_=-1;
    file_=openat(directory_,"STORE.IMG",O_RDWR|O_NOFOLLOW|O_CLOEXEC);if(file_<0)return failure();
    struct stat st{};if(fstat(file_,&st)||static_cast<uint64_t>(st.st_dev)!=device_ || static_cast<uint64_t>(st.st_ino)!=inode_)return -EIO;
    int e=hardware_store_validate(file_,record_,record_length_);if(e)return e;
    BrookSha256 verify{};BrookShaInit(&verify);uint8_t buf[65536];uint64_t left=size_;
    while(left) {size_t count=left>sizeof(buf)?sizeof(buf):static_cast<size_t>(left);ssize_t n=read(file_,buf,count);
        if(n<0&&errno==EINTR)continue;if(n<=0)return failure();BrookShaAdd(&verify,buf,static_cast<size_t>(n));left-=static_cast<uint64_t>(n);}
    BrookShaFinish(&verify,hash);if(!BrookUpdateEqual(hash,digest_,32))return -EBADMSG;
    if(close(file_)){file_=-1;return failure();}file_=-1;staged_=true;failed_=false;return 0;
}
int HardwareStoreTransaction::finalize() {
    if(!staged_)return -EINVAL;staged_=false;
#ifdef BROOK_STORE_TEST
    const char *inject=getenv("BROOK_STORE_TEST_FAILURE");
    if(inject && !strcmp(inject,"before-create"))return -EIO;
#endif
    // Boot is the only consumer and accepts only a complete matching record.
    // The image was synced and rehashed before this exclusive creation.
    int fd=openat(directory_,"STORE.OK",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0644);
    if(fd<0)return failure();
    published_=true;
#ifdef BROOK_STORE_TEST
    if(inject && !strcmp(inject,"after-create")){close(fd);return -EIO;}
#endif
    int e=write_all(fd,record_,record_length_);if(!e && fsync(fd))e=failure();
    if(close(fd)&&!e)e=failure();if(e)return e;
    if(fsync(directory_))return failure();
    fd=openat(directory_,"STORE.OK",O_RDWR|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return failure();
    char check[97];ssize_t n=read(fd,check,sizeof(check));e=(n==static_cast<ssize_t>(record_length_) && !memcmp(check,record_,record_length_))?0:-EIO;
    if(close(fd)&&!e)e=failure();if(!e)durable_=true;return e;
}
