#define _GNU_SOURCE
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sched.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <dirent.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/mman.h>
static unsigned checks;
static int outsiderSocket=-1;static pid_t outsider;
#define CK(x) do { ++checks;if(!(x)){fprintf(stderr,"PROCNS FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);} } while(0)
static void put(const char* p,const char* s) { int f=open(p,O_WRONLY|O_CREAT|O_TRUNC,0600);CK(f>=0);CK(write(f,s,strlen(s))==(ssize_t)strlen(s));CK(!close(f)); }
static void deny_groups(void) {int f=open("/proc/self/setgroups",O_WRONLY);CK(f>=0);CK(write(f,"deny\n",5)==5);CK(!close(f));}
static void map(const char* p,unsigned in,unsigned out) {char t[64];snprintf(t,sizeof(t),"%u %u 1\n",in,out);int f=open(p,O_WRONLY);CK(f>=0);CK(write(f,t,strlen(t))==(ssize_t)strlen(t));CK(!close(f));}
static void sendfd(int s,int f) {char c='F',data[CMSG_SPACE(sizeof(int))]={0};struct iovec io={&c,1};struct msghdr m={0};m.msg_iov=&io;m.msg_iovlen=1;m.msg_control=data;m.msg_controllen=sizeof(data);struct cmsghdr* h=CMSG_FIRSTHDR(&m);h->cmsg_level=SOL_SOCKET;h->cmsg_type=SCM_RIGHTS;h->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(h),&f,sizeof(f));CK(sendmsg(s,&m,0)==1);}
static int recvfd(int s) {char c=0,data[CMSG_SPACE(sizeof(int))]={0};struct iovec io={&c,1};struct msghdr m={0};m.msg_iov=&io;m.msg_iovlen=1;m.msg_control=data;m.msg_controllen=sizeof(data);CK(recvmsg(s,&m,0)==1&&c=='F'&&!(m.msg_flags&MSG_CTRUNC));struct cmsghdr* h=CMSG_FIRSTHDR(&m);CK(h&&h->cmsg_level==SOL_SOCKET&&h->cmsg_type==SCM_RIGHTS);int f=-1;memcpy(&f,CMSG_DATA(h),sizeof(f));CK(f>=0);return f;}
static void waitok(pid_t p) {int status;CK(waitpid(p,&status,0)==p);CK(WIFEXITED(status)&&!WEXITSTATUS(status));}
static void error_read(int f,int e) {char c;errno=0;CK(read(f,&c,1)==-1&&errno==e);}
static void pathfd(int f) {
 CK((fcntl(f,F_GETFL)&O_PATH)!=0);error_read(f,EBADF);
 errno=0;CK(lseek(f,0,SEEK_SET)==-1&&errno==EBADF);
 errno=0;CK(write(f,"x",1)==-1&&errno==EBADF);
 errno=0;CK(ioctl(f,0xb703,0)==-1&&errno==EBADF);
 errno=0;CK(fcntl(f,F_SETFL,O_NONBLOCK)==-1&&errno==EBADF);
 errno=0;CK(fsync(f)==-1&&errno==EBADF);errno=0;CK(fdatasync(f)==-1&&errno==EBADF);
 errno=0;CK(syscall(SYS_fchmod,f,0600)==-1&&errno==EBADF);errno=0;CK(syscall(SYS_fchown,f,getuid(),getgid())==-1&&errno==EBADF);
 errno=0;CK(ftruncate(f,0)==-1&&errno==EBADF);
 char buffer[512];errno=0;CK(pread(f,buffer,1,0)==-1&&errno==EBADF);errno=0;CK(pwrite(f,"X",1,0)==-1&&errno==EBADF);
 errno=0;CK(syscall(SYS_getdents64,f,buffer,sizeof(buffer))==-1&&errno==EBADF);
 errno=0;CK(mmap(NULL,4096,PROT_READ,MAP_PRIVATE,f,0)==MAP_FAILED&&errno==EBADF);
 int ep=epoll_create1(0);CK(ep>=0);struct epoll_event event={.events=EPOLLIN};errno=0;CK(epoll_ctl(ep,EPOLL_CTL_ADD,f,&event)==-1&&errno==EBADF);CK(!close(ep));
 struct pollfd p={f,POLLIN,0};CK(poll(&p,1,0)==1&&(p.revents&POLLNVAL));
 int copy=fcntl(f,F_DUPFD_CLOEXEC,0);CK(copy>=0);CK(fcntl(copy,F_GETFD)&FD_CLOEXEC);CK(!close(copy));
}
static void own(void) {
 struct stat st,linkst;CK(!stat("/proc/self/ns/mnt",&st));CK(S_ISREG(st.st_mode));
 CK(!lstat("/proc/self/ns/mnt",&linkst)&&S_ISLNK(linkst.st_mode));
 char text[80]={0};ssize_t n=readlink("/proc/self/ns/mnt",text,sizeof(text)-1);CK(n>0);unsigned long long id=0;CK(sscanf(text,"mnt:[%llu]",&id)==1&&id==st.st_ino);
 int fd=open("/proc/self/ns/mnt",O_RDONLY);CK(fd>=0);struct stat s;CK(!fstat(fd,&s)&&s.st_ino==st.st_ino&&s.st_dev==st.st_dev);error_read(fd,EINVAL);
 errno=0;CK(lseek(fd,0,SEEK_SET)==-1&&errno==ESPIPE);CK(ioctl(fd,0xb703,0)==CLONE_NEWNS);
 errno=0;CK(ioctl(fd,0x123456,0)==-1&&errno==ENOTTY);CK(!close(fd));
 errno=0;CK(open("/proc/self/ns/mnt",O_WRONLY)==-1&&errno==EPERM);
 errno=0;CK(open("/proc/self/ns/mnt",O_RDWR)==-1&&errno==EPERM);
 fd=open("/proc/self/ns/mnt",O_PATH);CK(fd>=0);pathfd(fd);CK(!fstat(fd,&s)&&s.st_ino==st.st_ino&&S_ISREG(s.st_mode));CK(!close(fd));
 fd=open("/proc/self/ns/mnt",O_PATH|O_NOFOLLOW);CK(fd>=0);pathfd(fd);CK(!fstat(fd,&s)&&S_ISLNK(s.st_mode));CK(!close(fd));
 fd=open("/proc/self/ns",O_PATH|O_DIRECTORY);CK(fd>=0);CK(!fstatat(fd,"mnt",&s,0)&&s.st_ino==st.st_ino);CK(!close(fd));
 fd=open("/proc/self/ns",O_RDONLY|O_DIRECTORY);CK(fd>=0);DIR* d=fdopendir(fd);CK(d);int found=0;struct dirent* e;while((e=readdir(d)))if(!strcmp(e->d_name,"mnt"))++found;CK(found==1);CK(!closedir(d));
}
static int value(int dir) {int f=openat(dir,"sub/value",O_RDONLY);if(f<0)return -errno;char c=0;ssize_t n=read(f,&c,1);CK(n==1);CK(!close(f));return c;}
int main(int argc,char** argv) {
 CK(argc==2&&(!strcmp(argv[1],"--disposable-vm")||!strcmp(argv[1],"--root-fixture")));
 {
  int root=!strcmp(argv[1],"--root-fixture");uid_t outer=geteuid();gid_t group=getegid();
  if(root)CK(getuid()==0);
  int pair[2];CK(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
  outsider=root?fork():syscall(SYS_clone,(unsigned long)(CLONE_NEWUSER|SIGCHLD),0L,0L,0L,0L);CK(outsider>=0);
  if(!outsider) {
   CK(!close(pair[0]));
   if(root) {CK(!setgid(100)&&!setuid(1001));}
   else {map("/proc/self/uid_map",0,outer);deny_groups();map("/proc/self/gid_map",0,group);}
   pid_t targetPid;CK(read(pair[1],&targetPid,sizeof(targetPid))==sizeof(targetPid));
   char path[80],link[80];snprintf(path,sizeof(path),"/proc/%d/ns/mnt",targetPid);
   errno=0;CK(readlink(path,link,sizeof(link))==-1&&errno==EACCES);
   errno=0;CK(open(path,O_RDONLY)==-1&&errno==EACCES);
   int received=recvfd(pair[1]);struct stat ns;CK(!fstat(received,&ns)&&S_ISREG(ns.st_mode));CK(ioctl(received,0xb703,0)==CLONE_NEWNS);CK(!close(received));
   CK(write(pair[1],"Y",1)==1);CK(!close(pair[1]));_exit(0);
  }
  CK(!close(pair[1]));outsiderSocket=pair[0];if(root) {CK(!setgid(100)&&!setuid(1000));}
 }
 CK(getuid()!=0);own();
 char base[]="/tmp/procns-XXXXXX";CK(mkdtemp(base));char a[128],t[128],sub[128],av[160],tv[160];
 snprintf(a,sizeof(a),"%s/a",base);snprintf(t,sizeof(t),"%s/t",base);snprintf(sub,sizeof(sub),"%s/t/sub",base);snprintf(av,sizeof(av),"%s/value",a);snprintf(tv,sizeof(tv),"%s/value",sub);
 CK(!mkdir(a,0700)&&!mkdir(t,0700)&&!mkdir(sub,0700));put(av,"B");put(tv,"T");
 int plain=open(av,O_PATH|O_RDWR|O_TRUNC|O_CREAT,0600);CK(plain>=0);pathfd(plain);CK(!close(plain));
 int plainRead=open(av,O_RDONLY);CK(plainRead>=0);char unchanged;CK(read(plainRead,&unchanged,1)==1&&unchanged=='B');CK(!close(plainRead));
 uid_t uid=geteuid();gid_t gid=getegid();struct stat self;CK(!stat("/proc/self/ns/mnt",&self));
 for(int mode=0;mode<5;++mode) {
  int sv[2];CK(!socketpair(AF_UNIX,SOCK_STREAM,0,sv));
  pid_t child=syscall(SYS_clone,(unsigned long)(CLONE_NEWUSER|CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(child>=0);
  if(!child) {
   CK(!close(sv[0]));map("/proc/self/uid_map",0,uid);deny_groups();map("/proc/self/gid_map",0,gid);
   CK(!mount(a,sub,NULL,MS_BIND,NULL));int dir=open(t,O_RDONLY|O_DIRECTORY);CK(dir>=0);sendfd(sv[1],dir);CK(!close(dir));char c;CK(read(sv[1],&c,1)==1);CK(!close(sv[1]));_exit(0);
  }
  CK(!close(sv[1]));int foreign=recvfd(sv[0]);CK(value(foreign)=='B');char p[80];snprintf(p,sizeof(p),"/proc/%d/ns",child);
  int directory=open(p,O_PATH|O_DIRECTORY);CK(directory>=0);struct stat ns;CK(!fstatat(directory,"mnt",&ns,0)&&ns.st_ino!=self.st_ino);
  int link=-1,handle=-1;
  if(mode==1) {link=openat(directory,"mnt",O_PATH|O_NOFOLLOW);CK(link>=0);}
  int keeperPipe[2]={-1,-1};pid_t keeper=-1;
  if(mode>=2) {
   handle=openat(directory,"mnt",mode==3?O_PATH:O_RDONLY);CK(handle>=0);
   if(outsiderSocket>=0) {
    CK(write(outsiderSocket,&child,sizeof(child))==sizeof(child));sendfd(outsiderSocket,handle);char answer;CK(read(outsiderSocket,&answer,1)==1&&answer=='Y');CK(!close(outsiderSocket));outsiderSocket=-1;waitok(outsider);
   }
   int copy=dup(handle);CK(copy>=0);CK(!close(handle));handle=copy;
   int rights[2];CK(!socketpair(AF_UNIX,SOCK_STREAM,0,rights));sendfd(rights[0],handle);CK(!close(handle));handle=recvfd(rights[1]);CK(!close(rights[0])&&!close(rights[1]));
  }
  if(mode==4) {
   CK(!pipe(keeperPipe));keeper=fork();CK(keeper>=0);
   if(!keeper) {CK(!close(keeperPipe[1]));char c;CK(read(keeperPipe[0],&c,1)==1);struct stat kept;CK(!fstat(handle,&kept)&&S_ISREG(kept.st_mode));CK(!close(handle)&&!close(keeperPipe[0]));_exit(0);}
   CK(!close(keeperPipe[0])&&!close(handle));handle=-1;
  }
  CK(write(sv[0],"X",1)==1);CK(!close(sv[0]));waitok(child);
  errno=0;CK(fstatat(directory,"mnt",&ns,0)==-1&&errno==ENOENT);
  CK(value(foreign)==(mode>=2?'B':'T'));
  if(mode>=2) {
   if(mode==4) {CK(write(keeperPipe[1],"X",1)==1);CK(!close(keeperPipe[1]));waitok(keeper);}
   else {CK(!fstat(handle,&ns)&&S_ISREG(ns.st_mode));CK(!close(handle));}
   for(int retry=0;retry<100&&value(foreign)=='B';++retry) {usleep(10000);}
   CK(value(foreign)=='T');}
  if(link>=0) {CK(!fstat(link,&ns)&&S_ISLNK(ns.st_mode));CK(!close(link));}
  CK(!fstat(directory,&ns)&&S_ISDIR(ns.st_mode));
  CK(!close(directory)&&!close(foreign));
 }
 CK(!unlink(tv)&&!unlink(av));CK(!rmdir(sub)&&!rmdir(t)&&!rmdir(a)&&!rmdir(base));
 puts("PROCNS PASS: identity, link/follow, O_PATH, ordinary handles, transfer/dup, active mount lifetime and final-close retirement");
 printf("PROCNS parent cumulative checks=%u\n",checks);return 0;
}
