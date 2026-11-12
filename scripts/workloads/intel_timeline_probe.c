// Marker-gated i915 timeline extension and GPU store diagnostic.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#define IOC(n,s) ((3u<<30)|((unsigned)(s)<<16)|('d'<<8)|(n))
#define CHECK(x) do {if(!(x)){fprintf(stderr,"INTEL_TIMELINE_FAIL line=%d errno=%d\n",__LINE__,errno);return 1;}} while(0)
struct create {uint64_t size;uint32_t handle,pad;};
struct map {uint32_t handle,pad;uint64_t offset,flags,extensions;};
struct object {uint32_t handle,reloc_count;uint64_t relocs,alignment,offset,flags,reserved1,reserved2;};
struct exec {uint64_t objects;uint32_t count,start,length,dr1,dr4,clip_count;uint64_t clips,flags,context,fences;};
struct sync {uint32_t handle,flags;};
struct capability {uint64_t id,value;};
struct getparam {int32_t param,pad;uint64_t value;};
struct fence {uint32_t handle,flags;};
struct extension {uint64_t next;uint32_t name,flags,reserved[4];uint64_t count,handles,points;};
struct timeline_array {uint64_t handles,points;uint32_t count,flags;};
struct timeline_wait {uint64_t handles,points;int64_t timeout;uint32_t count,flags,first,pad;uint64_t deadline;};
struct sync_fd {uint32_t handle,flags;int32_t fd;uint32_t pad;uint64_t point;};
struct sync_transfer {uint32_t source,destination;uint64_t source_point,destination_point;uint32_t flags,pad;};
struct reset_stats {uint32_t id,flags,resets,active,pending,pad;};
struct sync_file_info {char name[32];int32_t status;uint32_t flags,count,pad;uint64_t fences;};
struct sync_wait {uint64_t handles;int64_t timeout;uint32_t count,flags,first,pad;uint64_t deadline;};
int main(void){
    int fd=open("/dev/dri/renderD129",O_RDWR|O_CLOEXEC);CHECK(fd>=0);
    if(getenv("BROOK_EXPECT_SYNCOBJ_CAP")){
        struct capability cap={0x13,0};CHECK(!ioctl(fd,IOC(0x0c,sizeof(cap)),&cap) && cap.value==1);
        cap.id=0x14;cap.value=99;CHECK(!ioctl(fd,IOC(0x0c,sizeof(cap)),&cap) && cap.value==0);
    }
    if(getenv("BROOK_EXPECT_TIMELINE_EXEC")){
        int32_t value=-1;struct getparam param={55,0,(uintptr_t)&value};
        CHECK(!ioctl(fd,IOC(0x46,sizeof(param)),&param) && value==1);
        param.param=45;value=-1;CHECK(!ioctl(fd,IOC(0x46,sizeof(param)),&param) && value==0);
    }
    const size_t bytes=getenv("BROOK_GPU_CAPTURE_TWO_PAGE")?8192:4096;
    struct create data={bytes,0,0},batch={bytes,0,0};
    CHECK(!ioctl(fd,IOC(0x5b,sizeof(data)),&data) && !ioctl(fd,IOC(0x5b,sizeof(batch)),&batch));
    struct map dm={data.handle,0,0,2,0},bm={batch.handle,0,0,2,0};
    CHECK(!ioctl(fd,IOC(0x64,sizeof(dm)),&dm) && !ioctl(fd,IOC(0x64,sizeof(bm)),&bm));
    volatile uint32_t *dst=mmap(0,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,dm.offset);
    volatile uint32_t *cmd=mmap(0,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,bm.offset);
    CHECK(dst!=MAP_FAILED && cmd!=MAP_FAILED);
    if(getenv("BROOK_GPU_CAPTURE_RANDOM")){
        uint32_t random=0x12345678;
        for(unsigned i=0;i<bytes/4;++i){random^=random<<13;random^=random>>17;random^=random<<5;dst[i]=random;
            random^=random<<13;random^=random>>17;random^=random<<5;cmd[i]=random;}
    }
    *dst=0;
    cmd[0]=0x10000002;cmd[1]=0x40011000;cmd[2]=0;cmd[3]=0x54494d45;cmd[4]=0x05000000;cmd[5]=0;
    struct sync input={0,0},output={0,0};
    CHECK(!ioctl(fd,IOC(0xbf,sizeof(input)),&input) && !ioctl(fd,IOC(0xbf,sizeof(output)),&output));
    uint64_t in_point=1,out_point=2;
    struct timeline_array signal={(uintptr_t)&input.handle,(uintptr_t)&in_point,1,0};
    CHECK(!ioctl(fd,IOC(0xcd,sizeof(signal)),&signal));
    struct fence fences[]={{input.handle,1},{output.handle,2}};uint64_t points[]={in_point,out_point};
    struct extension ext={0,0,0,{0,0,0,0},2,(uintptr_t)fences,(uintptr_t)points};
    struct object objects[]={{data.handle,0,0,4096,0x40011000,20|128,0,0},{batch.handle,0,0,4096,0x10000,16|128,0,0}};
    struct exec request={(uintptr_t)objects,2,0,24,0,0,0,(uintptr_t)&ext,(1u<<11)|(1u<<21),0,0};
    CHECK(!ioctl(fd,IOC(0x69,sizeof(request)),&request));
    struct sync transferred={0,0};CHECK(!ioctl(fd,IOC(0xbf,sizeof(transferred)),&transferred));
    struct sync_transfer transfer={output.handle,transferred.handle,out_point,0,0,0};
    CHECK(!ioctl(fd,IOC(0xcc,sizeof(transfer)),&transfer));
    // A sync-file is a retained snapshot of the submitted fence, independent
    // of later changes to the sync object. Exercise real descriptor transfer.
    struct sync_fd sync_file={output.handle,1,-1,0,0};
    CHECK(!ioctl(fd,IOC(0xc1,sizeof(sync_file)),&sync_file) && sync_file.fd>=0);
    struct sync_fd point_file={output.handle,3,-1,0,out_point};
    CHECK(!ioctl(fd,IOC(0xc1,sizeof(point_file)),&point_file) && point_file.fd>=0);
    struct sync_fd zero_point_file={output.handle,3,-1,0,0};
    CHECK(!ioctl(fd,IOC(0xc1,sizeof(zero_point_file)),&zero_point_file) && zero_point_file.fd>=0);
    struct sync zero_import={0,0};CHECK(!ioctl(fd,IOC(0xbf,sizeof(zero_import)),&zero_import));
    struct sync_fd zero_point_import={zero_import.handle,3,zero_point_file.fd,0,0};
    CHECK(!ioctl(fd,IOC(0xc2,sizeof(zero_point_import)),&zero_point_import));
    struct timeline_array zero_signal={(uintptr_t)&zero_import.handle,0,1,0};
    CHECK(!ioctl(fd,IOC(0xcd,sizeof(zero_signal)),&zero_signal));
    struct sync imported_point={0,0};CHECK(!ioctl(fd,IOC(0xbf,sizeof(imported_point)),&imported_point));
    uint64_t imported_value=20;struct sync_fd point_import={imported_point.handle,3,point_file.fd,0,imported_value};
    CHECK(!ioctl(fd,IOC(0xc2,sizeof(point_import)),&point_import));
    struct sync transferred_point={0,0};CHECK(!ioctl(fd,IOC(0xbf,sizeof(transferred_point)),&transferred_point));
    uint64_t transferred_value=30;struct sync_transfer timeline_transfer={output.handle,transferred_point.handle,out_point,transferred_value,0,0};
    CHECK(!ioctl(fd,IOC(0xcc,sizeof(timeline_transfer)),&timeline_transfer));
    timeline_transfer.destination_point=10;CHECK(!ioctl(fd,IOC(0xcc,sizeof(timeline_transfer)),&timeline_transfer));
    uint64_t last_submitted=0;struct timeline_array submitted_query={(uintptr_t)&transferred_point.handle,(uintptr_t)&last_submitted,1,1};
    CHECK(!ioctl(fd,IOC(0xcb,sizeof(submitted_query)),&submitted_query) && last_submitted==30);
    CHECK(fcntl(sync_file.fd,F_GETFD)&FD_CLOEXEC);
    int sp[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,sp));
    char byte='s',rights[CMSG_SPACE(sizeof(int))]={0};struct iovec riov={&byte,1};
    struct msghdr rights_msg={0};rights_msg.msg_iov=&riov;rights_msg.msg_iovlen=1;
    rights_msg.msg_control=rights;rights_msg.msg_controllen=sizeof(rights);
    struct cmsghdr *rh=CMSG_FIRSTHDR(&rights_msg);rh->cmsg_level=SOL_SOCKET;rh->cmsg_type=SCM_RIGHTS;
    rh->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(rh),&sync_file.fd,sizeof(int));
    CHECK(sendmsg(sp[0],&rights_msg,0)==1);close(sync_file.fd);
    memset(rights,0,sizeof(rights));rights_msg.msg_controllen=sizeof(rights);
    CHECK(recvmsg(sp[1],&rights_msg,0)==1);rh=CMSG_FIRSTHDR(&rights_msg);
    CHECK(rh && rh->cmsg_level==SOL_SOCKET && rh->cmsg_type==SCM_RIGHTS);
    int received_file=-1;memcpy(&received_file,CMSG_DATA(rh),sizeof(int));CHECK(received_file>=0);
    int ep=epoll_create1(EPOLL_CLOEXEC);CHECK(ep>=0);
    struct epoll_event interest={.events=EPOLLIN|EPOLLET,.data.u64=0x53464e43},event={0};
    CHECK(!epoll_ctl(ep,EPOLL_CTL_ADD,received_file,&interest));
    struct sync imported_file={0,0};CHECK(!ioctl(fd,IOC(0xbf,sizeof(imported_file)),&imported_file));
    struct sync_fd import_file={imported_file.handle,1,received_file,0,0};
    CHECK(!ioctl(fd,IOC(0xc2,sizeof(import_file)),&import_file));
    struct pollfd pfd={received_file,POLLIN,0};CHECK(poll(&pfd,1,10000)==1 && pfd.revents==POLLIN);
    struct pollfd point_poll={point_file.fd,POLLIN,0};CHECK(poll(&point_poll,1,0)==1 && point_poll.revents==POLLIN);
    CHECK(epoll_wait(ep,&event,1,10000)==1 && event.events==EPOLLIN && event.data.u64==0x53464e43);
    CHECK(epoll_wait(ep,&event,1,0)==0);
    CHECK(!epoll_ctl(ep,EPOLL_CTL_MOD,received_file,&interest));
    CHECK(epoll_wait(ep,&event,1,0)==1 && event.events==EPOLLIN);
    struct sync_file_info finfo={0};CHECK(!ioctl(received_file,0xc0383e04,&finfo) && finfo.status==1 && finfo.count==1);
    struct timespec file_now;CHECK(!clock_gettime(CLOCK_MONOTONIC,&file_now));
    struct timeline_wait imported_wait={(uintptr_t)&imported_point.handle,(uintptr_t)&imported_value,(int64_t)file_now.tv_sec*1000000000ll+file_now.tv_nsec+5000000000ll,1,0,0,0,0};
    CHECK(!ioctl(fd,IOC(0xca,sizeof(imported_wait)),&imported_wait));
    imported_wait.handles=(uintptr_t)&transferred_point.handle;imported_wait.points=(uintptr_t)&transferred_value;
    CHECK(!ioctl(fd,IOC(0xca,sizeof(imported_wait)),&imported_wait));
    CHECK(!ioctl(fd,IOC(0xc0,sizeof(imported_point)),&imported_point));
    CHECK(!ioctl(fd,IOC(0xc0,sizeof(transferred_point)),&transferred_point));
    struct sync_wait binary_wait={(uintptr_t)&imported_file.handle,(int64_t)file_now.tv_sec*1000000000ll+file_now.tv_nsec+5000000000ll,1,1,0,0,0};
    CHECK(!ioctl(fd,IOC(0xc3,sizeof(binary_wait)),&binary_wait));
    binary_wait.handles=(uintptr_t)&zero_import.handle;
    CHECK(!ioctl(fd,IOC(0xc3,sizeof(binary_wait)),&binary_wait));
    CHECK(!ioctl(fd,IOC(0xc0,sizeof(zero_import)),&zero_import));close(zero_point_file.fd);
    binary_wait.handles=(uintptr_t)&transferred.handle;
    CHECK(!ioctl(fd,IOC(0xc3,sizeof(binary_wait)),&binary_wait));
    CHECK(!ioctl(fd,IOC(0xc0,sizeof(transferred)),&transferred));
    CHECK(!ioctl(fd,IOC(0xc0,sizeof(imported_file)),&imported_file));close(point_file.fd);close(ep);close(received_file);close(sp[0]);close(sp[1]);
    enum { MANY_FENCES=32 };struct sync many[MANY_FENCES]={0};struct fence many_fences[MANY_FENCES];
    uint64_t many_points[MANY_FENCES];uint32_t many_handles[MANY_FENCES];
    for(unsigned i=0;i<MANY_FENCES;++i){CHECK(!ioctl(fd,IOC(0xbf,sizeof(many[i])),&many[i]));
        many_handles[i]=many[i].handle;many_fences[i]=(struct fence){many[i].handle,2};many_points[i]=i+1;}
    struct extension many_ext={0,0,0,{0,0,0,0},MANY_FENCES,(uintptr_t)many_fences,(uintptr_t)many_points};
    request.clips=(uintptr_t)&many_ext;CHECK(!ioctl(fd,IOC(0x69,sizeof(request)),&request));
    struct timespec many_now;CHECK(!clock_gettime(CLOCK_MONOTONIC,&many_now));
    struct timeline_wait many_wait={(uintptr_t)many_handles,(uintptr_t)many_points,(int64_t)many_now.tv_sec*1000000000ll+many_now.tv_nsec+5000000000ll,MANY_FENCES,1,0,0,0};
    CHECK(!ioctl(fd,IOC(0xca,sizeof(many_wait)),&many_wait));
    for(unsigned i=0;i<MANY_FENCES;++i)CHECK(!ioctl(fd,IOC(0xc0,sizeof(many[i])),&many[i]));
    struct sync shared={0,0};CHECK(!ioctl(fd,IOC(0xbf,sizeof(shared)),&shared));
    struct {uint64_t handles;uint32_t count,pad;} binary_signal={(uintptr_t)&shared.handle,1,0};
    CHECK(!ioctl(fd,IOC(0xc5,sizeof(binary_signal)),&binary_signal));
    struct sync_fd shared_fd={shared.handle,0,-1,0,0};CHECK(!ioctl(fd,IOC(0xc1,sizeof(shared_fd)),&shared_fd));
    struct sync_fd alias_fd={0,0,shared_fd.fd,0,0};CHECK(!ioctl(fd,IOC(0xc2,sizeof(alias_fd)),&alias_fd));
    CHECK(alias_fd.handle && alias_fd.handle!=shared.handle);close(shared_fd.fd);
    struct fence alias_fences[]={{shared.handle,1},{alias_fd.handle,1},{alias_fd.handle,2},{shared.handle,2}};
    uint64_t alias_points[]={0,0,2,3};
    struct extension alias_ext={0,0,0,{0,0,0,0},4,(uintptr_t)alias_fences,(uintptr_t)alias_points};
    request.clips=(uintptr_t)&alias_ext;CHECK(!ioctl(fd,IOC(0x69,sizeof(request)),&request));
    uint64_t alias_point=3;struct timeline_wait alias_wait={(uintptr_t)&shared.handle,(uintptr_t)&alias_point,(int64_t)many_now.tv_sec*1000000000ll+many_now.tv_nsec+5000000000ll,1,0,0,0,0};
    CHECK(!ioctl(fd,IOC(0xca,sizeof(alias_wait)),&alias_wait));
    struct sync alias_handle={alias_fd.handle,0};CHECK(!ioctl(fd,IOC(0xc0,sizeof(alias_handle)),&alias_handle));
    CHECK(!ioctl(fd,IOC(0xc0,sizeof(shared)),&shared));
    struct timespec now;CHECK(!clock_gettime(CLOCK_MONOTONIC,&now));
    struct timeline_wait wait={(uintptr_t)&output.handle,(uintptr_t)&out_point,(int64_t)now.tv_sec*1000000000ll+now.tv_nsec+5000000000ll,1,0,0,0,0};
    CHECK(!ioctl(fd,IOC(0xca,sizeof(wait)),&wait) && *dst==0x54494d45);
    if(getenv("BROOK_GPU_DEEP_TIMELINE")){
        uint32_t source=output.handle;uint64_t source_point=out_point,point=1;
        for(unsigned i=0;i<96;++i){struct sync next={0,0};CHECK(!ioctl(fd,IOC(0xbf,sizeof(next)),&next));
            struct sync_transfer deep={source,next.handle,source_point,point,0,0};CHECK(!ioctl(fd,IOC(0xcc,sizeof(deep)),&deep));
            if(i){struct sync old={source,0};CHECK(!ioctl(fd,IOC(0xc0,sizeof(old)),&old));}
            source=next.handle;source_point=point;
        }
        CHECK(!clock_gettime(CLOCK_MONOTONIC,&now));
        struct timeline_wait deep_wait={(uintptr_t)&source,(uintptr_t)&point,(int64_t)now.tv_sec*1000000000ll+now.tv_nsec+5000000000ll,1,0,0,0,0};
        CHECK(!ioctl(fd,IOC(0xca,sizeof(deep_wait)),&deep_wait));
        struct sync last={source,0};CHECK(!ioctl(fd,IOC(0xc0,sizeof(last)),&last));
    }
    struct reset_stats stats={0};CHECK(!ioctl(fd,IOC(0x72,sizeof(stats)),&stats) && !stats.active && !stats.pending);
    uint64_t value=0;struct timeline_array query={(uintptr_t)&output.handle,(uintptr_t)&value,1,0};
    CHECK(!ioctl(fd,IOC(0xcb,sizeof(query)),&query) && value==2);
    struct sync_fd exported={output.handle,0,-1,0,0};CHECK(!ioctl(fd,IOC(0xc1,sizeof(exported)),&exported));
    CHECK(fcntl(exported.fd,F_GETFD)&FD_CLOEXEC);
    int pair[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
    char c='x',control[CMSG_SPACE(sizeof(int))]={0};struct iovec iov={&c,1};
    struct msghdr msg={0};msg.msg_iov=&iov;msg.msg_iovlen=1;msg.msg_control=control;msg.msg_controllen=sizeof(control);
    struct cmsghdr *header=CMSG_FIRSTHDR(&msg);header->cmsg_level=SOL_SOCKET;header->cmsg_type=SCM_RIGHTS;
    header->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(header),&exported.fd,sizeof(int));
    CHECK(sendmsg(pair[0],&msg,0)==1);close(exported.fd);
    CHECK(!ioctl(fd,IOC(0xc0,sizeof(output)),&output));
    memset(control,0,sizeof(control));msg.msg_controllen=sizeof(control);CHECK(recvmsg(pair[1],&msg,0)==1);
    header=CMSG_FIRSTHDR(&msg);CHECK(header && header->cmsg_level==SOL_SOCKET && header->cmsg_type==SCM_RIGHTS);
    int received=-1;memcpy(&received,CMSG_DATA(header),sizeof(received));CHECK(received>=0);
    int other=open("/dev/dri/renderD129",O_RDWR|O_CLOEXEC);CHECK(other>=0);
    struct sync_fd imported={0,0,received,0,0};CHECK(!ioctl(other,IOC(0xc2,sizeof(imported)),&imported));
    close(received);value=0;query.handles=(uintptr_t)&imported.handle;
    CHECK(!ioctl(other,IOC(0xcb,sizeof(query)),&query) && value==2);
    struct sync imported_handle={imported.handle,0};CHECK(!ioctl(other,IOC(0xc0,sizeof(imported_handle)),&imported_handle));
    close(other);close(pair[0]);close(pair[1]);CHECK(!ioctl(fd,IOC(0xc0,sizeof(input)),&input));
    CHECK(!munmap((void*)cmd,bytes) && !munmap((void*)dst,bytes) && !close(fd));
    puts("INTEL_TIMELINE_GPU_PASS");return 0;
}
