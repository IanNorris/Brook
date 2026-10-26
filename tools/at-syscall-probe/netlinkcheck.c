#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
static void dump(int fd,unsigned seq){
 struct {struct nlmsghdr h;struct rtgenmsg g;} r={{NLMSG_LENGTH(sizeof(struct rtgenmsg)),RTM_GETLINK,NLM_F_REQUEST|NLM_F_DUMP,seq,0},{AF_UNSPEC}};
 struct sockaddr_nl kernel={.nl_family=AF_NETLINK};
 assert(sendto(fd,&r,r.h.nlmsg_len,0,(void*)&kernel,sizeof(kernel))==(ssize_t)r.h.nlmsg_len);
}
static unsigned receive_dump(int fd,unsigned seq,unsigned port){
 unsigned links=0;int done=0;unsigned char buffer[8192];
 while(!done){struct sockaddr_nl src={0};struct iovec vector={buffer,sizeof(buffer)};struct msghdr m={.msg_name=&src,.msg_namelen=sizeof(src),.msg_iov=&vector,.msg_iovlen=1};
  ssize_t n=recvmsg(fd,&m,0);assert(n>0 && !(m.msg_flags&MSG_TRUNC) && src.nl_family==AF_NETLINK && src.nl_pid==0);
  unsigned remaining=n;for(struct nlmsghdr* h=(void*)buffer;NLMSG_OK(h,remaining);h=NLMSG_NEXT(h,remaining)){
   assert(h->nlmsg_seq==seq && h->nlmsg_pid==port);
   if(h->nlmsg_type==NLMSG_DONE){done=1;break;}assert(h->nlmsg_type==RTM_NEWLINK);++links;
  }
 }assert(links);return links;
}
struct Sender {int fd;unsigned seq;};
static void* sender(void* opaque){struct Sender* s=opaque;usleep(30000);dump(s->fd,s->seq);return NULL;}
int main(void){
 alarm(30);setvbuf(stdout,NULL,_IONBF,0);puts("NETLINK_BEGIN");
 struct if_nameindex* names=if_nameindex();assert(names);unsigned interfaces=0;
 int inet=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);assert(inet>=0);
 for(struct if_nameindex* n=names;n->if_index;++n){
  assert(n->if_name && if_nametoindex(n->if_name)==n->if_index);char name[IF_NAMESIZE];assert(if_indextoname(n->if_index,name) && !strcmp(name,n->if_name));
  struct ifreq r={0};strncpy(r.ifr_name,n->if_name,IF_NAMESIZE-1);assert(!ioctl(inet,SIOCGIFINDEX,&r) && r.ifr_ifindex==(int)n->if_index);
  assert(!ioctl(inet,SIOCGIFHWADDR,&r));assert(!ioctl(inet,SIOCGIFFLAGS,&r));assert(!ioctl(inet,SIOCGIFMTU,&r) && r.ifr_mtu>0);++interfaces;
 }
 puts("NETLINK_INTERFACE_IOCTLS_PASS");assert(interfaces);if_freenameindex(names);close(inet);
 struct ifaddrs* addresses=NULL;assert(!getifaddrs(&addresses));unsigned ipv4=0;
 for(struct ifaddrs* a=addresses;a;a=a->ifa_next)if(a->ifa_addr && a->ifa_addr->sa_family==AF_INET){assert(a->ifa_netmask && a->ifa_name);++ipv4;}
 puts("NETLINK_IFADDRS_PASS");assert(ipv4);freeifaddrs(addresses);
 int fd=socket(AF_NETLINK,SOCK_RAW|SOCK_CLOEXEC|SOCK_NONBLOCK,NETLINK_ROUTE);assert(fd>=0);
 struct sockaddr_nl name={.nl_family=AF_NETLINK};assert(!bind(fd,(void*)&name,sizeof(name)));socklen_t len=sizeof(name);assert(!getsockname(fd,(void*)&name,&len) && len==sizeof(name) && name.nl_pid);
 int alias=dup(fd);assert(alias>=0);assert(fcntl(alias,F_GETFL)&O_NONBLOCK);assert(!fcntl(alias,F_SETFL,fcntl(alias,F_GETFL)&~O_NONBLOCK));assert(!(fcntl(fd,F_GETFL)&O_NONBLOCK));
 int pair[2];assert(!socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,pair));
 char payload='x';char control[CMSG_SPACE(sizeof(int))]={0};struct iovec rightVector={&payload,1};struct msghdr rights={.msg_iov=&rightVector,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof(control)};
 struct cmsghdr* header=CMSG_FIRSTHDR(&rights);header->cmsg_level=SOL_SOCKET;header->cmsg_type=SCM_RIGHTS;header->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(header),&fd,sizeof(fd));assert(sendmsg(pair[0],&rights,0)==1);
 memset(control,0,sizeof(control));rights.msg_controllen=sizeof(control);assert(recvmsg(pair[1],&rights,MSG_CMSG_CLOEXEC)==1);header=CMSG_FIRSTHDR(&rights);assert(header && header->cmsg_level==SOL_SOCKET && header->cmsg_type==SCM_RIGHTS);
 int passed=-1;memcpy(&passed,CMSG_DATA(header),sizeof(passed));assert(passed>=0 && (fcntl(passed,F_GETFD)&FD_CLOEXEC));assert(!fcntl(passed,F_SETFL,fcntl(passed,F_GETFL)|O_NONBLOCK));assert(fcntl(fd,F_GETFL)&O_NONBLOCK);assert(!fcntl(passed,F_SETFL,fcntl(passed,F_GETFL)&~O_NONBLOCK));close(passed);close(pair[0]);close(pair[1]);
 puts("NETLINK_RIGHTS_ALIAS_PASS");int second=socket(AF_NETLINK,SOCK_DGRAM|SOCK_CLOEXEC,NETLINK_ROUTE);assert(second>=0);assert(bind(second,(void*)&name,sizeof(name))==-1 && errno==EADDRINUSE);struct sockaddr_nl automatic={.nl_family=AF_NETLINK};assert(!bind(second,(void*)&automatic,sizeof(automatic)));len=sizeof(automatic);assert(!getsockname(second,(void*)&automatic,&len) && automatic.nl_pid && automatic.nl_pid!=name.nl_pid);close(second);
 struct stat a,b;assert(!fstat(fd,&a) && !fstat(alias,&b) && S_ISSOCK(a.st_mode) && a.st_ino==b.st_ino);
 int type=-1;len=sizeof(type);assert(!getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&len) && type==SOCK_RAW);
 dump(fd,71);struct pollfd p={fd,POLLIN,0};assert(poll(&p,1,1000)==1 && (p.revents&POLLIN));
 unsigned char tiny[7];ssize_t bytes=recv(fd,tiny,sizeof(tiny),MSG_PEEK|MSG_TRUNC);assert(bytes>(ssize_t)sizeof(tiny));
 assert(receive_dump(alias,71,name.nl_pid)>=1);
 assert(recv(fd,tiny,sizeof(tiny),MSG_DONTWAIT)==-1 && errno==EAGAIN);
 // One truncated receive discards its whole datagram; preserve all framing
 // in the second dump rather than assuming Linux packs DONE with link data.
 dump(fd,72);assert(recv(fd,tiny,sizeof(tiny),MSG_TRUNC)>(ssize_t)sizeof(tiny));
 while(recv(fd,tiny,sizeof(tiny),MSG_DONTWAIT)>=0){}assert(errno==EAGAIN);
 int ep=epoll_create1(EPOLL_CLOEXEC);assert(ep>=0);struct epoll_event event={.events=EPOLLIN,.data.u64=987};assert(!epoll_ctl(ep,EPOLL_CTL_ADD,fd,&event));
 struct Sender s={alias,73};pthread_t thread;assert(!pthread_create(&thread,NULL,sender,&s));assert(epoll_wait(ep,&event,1,2000)==1 && event.data.u64==987 && (event.events&EPOLLIN));assert(!pthread_join(thread,NULL));receive_dump(fd,73,name.nl_pid);close(ep);
 pid_t child=fork();assert(child>=0);if(!child){close(fd);dump(alias,74);close(alias);_exit(0);}int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));receive_dump(fd,74,name.nl_pid);
 close(fd);dump(alias,75);receive_dump(alias,75,name.nl_pid);close(alias);
 printf("NETLINK_PASS interfaces=%u ipv4=%u\n",interfaces,ipv4);return 0;
}
