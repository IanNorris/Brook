// Identical Linux/Brook sync_file ABI probe. Ring0 here verifies descriptor
// semantics only; GPU queue completion is a separate Vulkan readback test.
#include <errno.h>
#include <fcntl.h>
#include <linux/sync_file.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <virtgpu_drm.h>
#define CHECK(test) do { if(!(test)) { fprintf(stderr,"SYNC_FILE_FAIL line=%d errno=%d: %s\n",__LINE__,errno,#test);goto cleanup; } } while(0)
static int describe(int fd) {
    struct sync_file_info info={0};
    if(ioctl(fd,SYNC_IOC_FILE_INFO,&info) || info.status!=1 || info.num_fences!=1)return 0;
    struct sync_fence_info fence={0};info.sync_fence_info=(uintptr_t)&fence;
    if(ioctl(fd,SYNC_IOC_FILE_INFO,&info) || fence.status!=1 || fence.flags || !fence.driver_name[0])return 0;
    return 1;
}
int main(void) {
    int passed=0,drm=-1,fence=-1,alias=-1,epoll=-1,event=-1,sockets[2]={-1,-1};
    drm=open("/dev/dri/renderD128",O_RDWR|O_CLOEXEC);CHECK(drm>=0);
    struct drm_virtgpu_context_set_param params[]={{1,4},{2,64}};
    struct drm_virtgpu_context_init init={.num_params=2,.ctx_set_params=(uintptr_t)params};
    CHECK(!ioctl(drm,DRM_IOCTL_VIRTGPU_CONTEXT_INIT,&init));
    struct drm_virtgpu_execbuffer exec={.flags=VIRTGPU_EXECBUF_RING_IDX|VIRTGPU_EXECBUF_FENCE_FD_OUT,.ring_idx=0,.fence_fd=-1};
    CHECK(!ioctl(drm,DRM_IOCTL_VIRTGPU_EXECBUFFER,&exec));fence=exec.fence_fd;CHECK(fence>=0);
    CHECK((fcntl(fence,F_GETFD)&FD_CLOEXEC)!=0);
    CHECK((fcntl(fence,F_GETFL)&O_ACCMODE)==O_RDONLY);
    struct pollfd pollfd={.fd=fence,.events=POLLIN|POLLOUT};
    CHECK(poll(&pollfd,1,10000)==1 && pollfd.revents==POLLIN);
    CHECK(describe(fence));
    errno=0;CHECK(lseek(fence,0,SEEK_SET)==-1 && errno==ESPIPE);
    CHECK(poll(&pollfd,1,0)==1 && pollfd.revents==POLLIN);
    uint64_t counter=0;errno=0;CHECK(read(fence,&counter,8)==-1 && errno==EINVAL);
    errno=0;CHECK(write(fence,&counter,8)==-1 && errno==EBADF);
    struct sync_file_info bad={.flags=1};errno=0;CHECK(ioctl(fence,SYNC_IOC_FILE_INFO,&bad)==-1 && errno==EINVAL);
    bad=(struct sync_file_info){.pad=1};errno=0;CHECK(ioctl(fence,SYNC_IOC_FILE_INFO,&bad)==-1 && errno==EINVAL);
    bad=(struct sync_file_info){.num_fences=1,.sync_fence_info=1};errno=0;CHECK(ioctl(fence,SYNC_IOC_FILE_INFO,&bad)==-1 && errno==EFAULT);
    errno=0;CHECK(ioctl(fence,_IO('>',99),0)==-1 && errno==ENOTTY);
    event=eventfd(1,EFD_CLOEXEC);CHECK(event>=0);bad=(struct sync_file_info){0};errno=0;
    CHECK(ioctl(event,SYNC_IOC_FILE_INFO,&bad)==-1 && errno==ENOTTY);
    close(event);event=-1;
    epoll=epoll_create1(EPOLL_CLOEXEC);CHECK(epoll>=0);
    struct epoll_event interest={.events=EPOLLIN|EPOLLOUT,.data.u64=0x1234},ready={0};
    CHECK(!epoll_ctl(epoll,EPOLL_CTL_ADD,fence,&interest));
    for(unsigned i=0;i<2;++i)CHECK(epoll_wait(epoll,&ready,1,1000)==1 && ready.events==EPOLLIN && ready.data.u64==0x1234);
    for(unsigned kind=0;kind<2;++kind) {
        interest.events=EPOLLIN|(kind?EPOLLONESHOT:EPOLLET);
        CHECK(!epoll_ctl(epoll,EPOLL_CTL_MOD,fence,&interest));
        CHECK(epoll_wait(epoll,&ready,1,0)==1 && ready.events==EPOLLIN);
        CHECK(epoll_wait(epoll,&ready,1,0)==0);
        CHECK(!epoll_ctl(epoll,EPOLL_CTL_MOD,fence,&interest));
        CHECK(epoll_wait(epoll,&ready,1,0)==1 && ready.events==EPOLLIN);
        CHECK(epoll_wait(epoll,&ready,1,0)==0);
    }
    close(epoll);epoll=-1;
    alias=dup(fence);CHECK(alias>=0);close(fence);fence=-1;close(drm);drm=-1;
    CHECK(describe(alias));
    CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        close(sockets[0]);
        if(!describe(alias))_exit(11);
        char byte='F';struct iovec iov={&byte,1};char control[CMSG_SPACE(sizeof(int))]={0};
        struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof(control)};
        struct cmsghdr *c=CMSG_FIRSTHDR(&msg);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(c),&alias,sizeof(alias));
        _exit(sendmsg(sockets[1],&msg,0)==1?0:12);
    }
    close(alias);alias=-1;close(sockets[1]);sockets[1]=-1;
    char byte=0;struct iovec iov={&byte,1};char control[CMSG_SPACE(sizeof(int))]={0};
    struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof(control)};
    CHECK(recvmsg(sockets[0],&msg,MSG_CMSG_CLOEXEC)==1 && byte=='F' && !(msg.msg_flags&MSG_CTRUNC));
    struct cmsghdr *c=CMSG_FIRSTHDR(&msg);CHECK(c && c->cmsg_level==SOL_SOCKET && c->cmsg_type==SCM_RIGHTS && c->cmsg_len==CMSG_LEN(sizeof(int)));
    memcpy(&alias,CMSG_DATA(c),sizeof(alias));CHECK(alias>=0 && describe(alias));CHECK(fcntl(alias,F_GETFD)&FD_CLOEXEC);
    int status=0;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    CHECK(describe(alias));passed=1;
cleanup:
    if(drm>=0)close(drm);
    if(fence>=0)close(fence);
    if(alias>=0)close(alias);
    if(epoll>=0)close(epoll);
    if(event>=0)close(event);
    for(unsigned i=0;i<2;++i)if(sockets[i]>=0)close(sockets[i]);
    printf("{\"sync_file_passed\":%s,\"ring\":0,\"gpu_completion_test\":false}\n",passed?"true":"false");
    return passed?0:1;
}
