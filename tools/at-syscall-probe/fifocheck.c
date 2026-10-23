#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
static unsigned checks;
#define CK(x) do {++checks;if(!(x)){fprintf(stderr,"FIFO FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static void map(const char* p,unsigned id){char b[64];int n=snprintf(b,sizeof(b),"0 %u 1\n",id);int f=open(p,O_WRONLY);CK(f>=0&&write(f,b,n)==n&&close(f)==0);}
static void waitok(pid_t p){int s;CK(waitpid(p,&s,0)==p);CK(WIFEXITED(s)&&!WEXITSTATUS(s));}
static void pair(int w,int r,const char* text){char b[128];size_t n=strlen(text);CK(write(w,text,n)==(ssize_t)n);CK(read(r,b,n)==(ssize_t)n);CK(!memcmp(b,text,n));}
static void caught(int s){(void)s;}
static void passfd(int sock,int fd){char b='F';struct iovec i={&b,1};char control[CMSG_SPACE(sizeof(int))];memset(control,0,sizeof(control));struct msghdr m={0};m.msg_iov=&i;m.msg_iovlen=1;m.msg_control=control;m.msg_controllen=sizeof(control);struct cmsghdr* c=CMSG_FIRSTHDR(&m);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(c),&fd,sizeof(fd));CK(sendmsg(sock,&m,0)==1);}
static int recvfd(int sock){char b;struct iovec i={&b,1};char control[CMSG_SPACE(sizeof(int))];struct msghdr m={0};m.msg_iov=&i;m.msg_iovlen=1;m.msg_control=control;m.msg_controllen=sizeof(control);CK(recvmsg(sock,&m,0)==1);struct cmsghdr* c=CMSG_FIRSTHDR(&m);CK(c&&c->cmsg_level==SOL_SOCKET&&c->cmsg_type==SCM_RIGHTS);int f;memcpy(&f,CMSG_DATA(c),sizeof(f));return f;}
int main(int argc,char** argv){
 setbuf(stdout,NULL);int brook=argc==2&&!strcmp(argv[1],"--brook"),brookdisk=argc==2&&!strcmp(argv[1],"--brook-ext2"),linuxdisk=argc==2&&!strcmp(argv[1],"--linux-disk"),disk=brookdisk||linuxdisk;CK(argc==1||brook||disk);CK(geteuid()!=0);uid_t uid=geteuid();gid_t gid=getegid();char base[128];snprintf(base,sizeof(base),"%s/fifo-XXXXXX",brookdisk?"/data/home":"/tmp");CK(mkdtemp(base));
 pid_t child=syscall(SYS_clone,(unsigned long)(CLONE_NEWUSER|CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(child>=0);if(child){waitok(child);CK(rmdir(base)==0);printf("FIFO PARENT PASS checks=%u\n",checks);return 0;}
 alarm(90);map("/proc/self/uid_map",uid);int f=open("/proc/self/setgroups",O_WRONLY);CK(f>=0&&write(f,"deny\n",5)==5&&close(f)==0);map("/proc/self/gid_map",gid);CK(mount(NULL,"/",NULL,MS_PRIVATE|MS_REC,NULL)==0);if(!disk)CK(mount("tmpfs",base,"tmpfs",6,"mode=0700")==0);CK(chdir(base)==0);
 umask(0027);CK(mkfifo("p",0666)==0);struct stat st;CK(stat("p",&st)==0&&S_ISFIFO(st.st_mode)&&(st.st_mode&0777)==0640);errno=0;CK(mkfifo("p",0600)==-1&&errno==EEXIST);int dir=open(".",O_PATH|O_DIRECTORY);CK(dir>=0);CK(mkfifoat(dir,"at",0600)==0&&unlinkat(dir,"at",0)==0);CK(close(dir)==0);
 errno=0;CK(open("p",O_WRONLY|O_NONBLOCK)==-1&&errno==ENXIO);int r=open("p",O_RDONLY|O_NONBLOCK|O_CLOEXEC);CK(r>=0&&(fcntl(r,F_GETFD)&FD_CLOEXEC));char b[128];CK(read(r,b,1)==0);struct pollfd p={r,POLLIN,0};CK(poll(&p,1,0)==0);
 int w=open("p",O_WRONLY|O_NONBLOCK);CK(w>=0);errno=0;CK(read(w,b,1)==-1&&errno==EBADF);errno=0;CK(write(r,"x",1)==-1&&errno==EBADF);p.revents=0;CK(poll(&p,1,0)==0);CK(write(w,"hello",5)==5);int available=0;CK(ioctl(r,FIONREAD,&available)==0&&available==5);p.revents=0;CK(poll(&p,1,0)==1&&(p.revents&POLLIN));CK(read(r,b,5)==5&&!memcmp(b,"hello",5));errno=0;CK(read(r,b,1)==-1&&errno==EAGAIN);CK(close(w)==0);p.revents=0;CK(poll(&p,1,0)==1&&(p.revents&POLLHUP));CK(read(r,b,1)==0&&close(r)==0);
 f=open("p",O_RDWR|O_NONBLOCK|O_CLOEXEC);CK(f>=0);CK((fcntl(f,F_GETFL)&O_ACCMODE)==O_RDWR);pair(f,f,"roundtrip");errno=0;CK(lseek(f,0,SEEK_SET)==-1&&errno==ESPIPE);errno=0;CK(pread(f,b,1,0)==-1&&errno==ESPIPE);errno=0;CK(pwrite(f,"x",1,0)==-1&&errno==ESPIPE);errno=0;CK(mmap(NULL,4096,PROT_READ,MAP_PRIVATE,f,0)==MAP_FAILED&&errno==ENODEV);
 int ep=epoll_create1(EPOLL_CLOEXEC);CK(ep>=0);struct epoll_event ev={.events=EPOLLIN,.data.fd=f};CK(epoll_ctl(ep,EPOLL_CTL_ADD,f,&ev)==0);CK(epoll_wait(ep,&ev,1,0)==0);child=fork();CK(child>=0);if(!child){usleep(50000);CK(write(f,"e",1)==1);_exit(0);}CK(epoll_wait(ep,&ev,1,1000)==1&&(ev.events&EPOLLIN));CK(read(f,b,1)==1&&b[0]=='e');waitok(child);CK(close(ep)==0);
 int d=dup(f);CK(d>=0&&close(f)==0);pair(d,d,"dup survives");child=fork();CK(child>=0);if(!child){CK(write(d,"fork",4)==4);_exit(0);}waitok(child);CK(read(d,b,4)==4&&!memcmp(b,"fork",4));
 CK(rename("p","renamed")==0);int alias=open("renamed",O_RDWR|O_NONBLOCK);CK(alias>=0);CK(fstat(d,&st)==0);struct stat as;CK(fstat(alias,&as)==0&&st.st_ino==as.st_ino);pair(alias,d,"same inode");CK(unlink("renamed")==0);CK(fstat(d,&st)==0&&!st.st_nlink);CK(mkfifo("renamed",0600)==0);int fresh=open("renamed",O_RDWR|O_NONBLOCK);CK(fresh>=0);CK(fstat(fresh,&as)==0&&st.st_ino!=as.st_ino);CK(write(d,"old",3)==3);errno=0;CK(read(fresh,b,1)==-1&&errno==EAGAIN);CK(read(alias,b,3)==3&&!memcmp(b,"old",3));CK(close(d)==0&&close(alias)==0);
 int sv[2];CK(socketpair(AF_UNIX,SOCK_STREAM,0,sv)==0);passfd(sv[0],fresh);CK(close(fresh)==0);fresh=recvfd(sv[1]);CK(fresh>=0);pair(fresh,fresh,"rights hold");CK(close(sv[0])==0&&close(sv[1])==0&&close(fresh)==0);
 // Directory/file binds and namespace copies preserve the same FIFO identity.
 int target=open("alias",O_CREAT|O_EXCL|O_RDWR,0600);CK(target>=0&&close(target)==0);CK(mount("renamed","alias",NULL,MS_BIND,NULL)==0);f=open("renamed",O_RDWR|O_NONBLOCK);alias=open("alias",O_RDWR|O_NONBLOCK);CK(f>=0&&alias>=0);pair(alias,f,"bind alias");child=syscall(SYS_clone,(unsigned long)(CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(child>=0);if(!child){int c=open("alias",O_WRONLY|O_NONBLOCK);CK(c>=0&&write(c,"copy",4)==4&&close(c)==0);_exit(0);}waitok(child);CK(read(f,b,4)==4&&!memcmp(b,"copy",4));CK(close(alias)==0&&close(f)==0&&umount2("alias",MNT_DETACH)==0&&unlink("alias")==0);
 // Small writes are atomic and a full nonblocking FIFO cannot admit a prefix.
 f=open("renamed",O_RDWR|O_NONBLOCK);CK(f>=0);char page[4096];memset(page,'P',sizeof(page));for(;;){ssize_t n=write(f,page,sizeof(page));if(n<0){CK(errno==EAGAIN);break;}CK(n==sizeof(page));}while(write(f,"P",1)>0){}CK(errno==EAGAIN);errno=0;CK(write(f,page,sizeof(page))==-1&&errno==EAGAIN);CK(read(f,b,1)==1);errno=0;CK(write(f,page,sizeof(page))==-1&&errno==EAGAIN);while(read(f,page,sizeof(page))>0){}CK(errno==EAGAIN&&close(f)==0);
 CK(mkfifo("blocking",0600)==0);child=fork();CK(child>=0);if(!child){int c=open("blocking",O_WRONLY);CK(c>=0&&write(c,"B",1)==1&&close(c)==0);_exit(0);}r=open("blocking",O_RDONLY);CK(r>=0&&read(r,b,1)==1&&b[0]=='B'&&close(r)==0);waitok(child);
 int ready[2];CK(pipe(ready)==0);child=fork();CK(child>=0);if(!child){close(ready[0]);struct sigaction sa={0};sa.sa_handler=caught;sigemptyset(&sa.sa_mask);CK(sigaction(SIGUSR1,&sa,NULL)==0);CK(write(ready[1],"R",1)==1);errno=0;CK(open("blocking",O_RDONLY)==-1&&errno==EINTR);_exit(0);}CK(close(ready[1])==0&&read(ready[0],b,1)==1&&close(ready[0])==0);usleep(50000);CK(kill(child,SIGUSR1)==0);waitok(child);
 // Interrupt one of two blocked writers, reap it, then wake the survivor.
 f=open("renamed",O_RDWR|O_NONBLOCK);CK(f>=0);while(write(f,page,sizeof(page))>0){}CK(errno==EAGAIN);while(write(f,"P",1)>0){}CK(errno==EAGAIN);
 int wake[2];CK(pipe(wake)==0);pid_t writers[2];for(int i=0;i<2;++i){writers[i]=fork();CK(writers[i]>=0);if(!writers[i]){close(wake[0]);struct sigaction sa={0};sa.sa_handler=caught;sigemptyset(&sa.sa_mask);CK(sigaction(SIGUSR1,&sa,NULL)==0);int out=open("renamed",O_WRONLY);CK(out>=0&&write(wake[1],"R",1)==1);errno=0;ssize_t n=write(out,"W",1);CK(i==0?(n==-1&&errno==EINTR):n==1);CK(close(out)==0);_exit(0);}}
 CK(close(wake[1])==0);for(int i=0;i<2;++i)CK(read(wake[0],b,1)==1);CK(close(wake[0])==0);usleep(100000);CK(kill(writers[0],SIGUSR1)==0);waitok(writers[0]);usleep(50000);CK(read(f,page,sizeof(page))==(ssize_t)sizeof(page));waitok(writers[1]);while(read(f,page,sizeof(page))>0){}CK(errno==EAGAIN&&close(f)==0);
 // Last close discards buffered bytes and a new open obtains a fresh stream.
 f=open("renamed",O_RDWR|O_NONBLOCK);CK(f>=0&&write(f,"discard",7)==7&&close(f)==0);f=open("renamed",O_RDWR|O_NONBLOCK);CK(f>=0);errno=0;CK(read(f,b,1)==-1&&errno==EAGAIN);CK(close(f)==0);
 struct statvfs space;CK(statvfs(base,&space)==0);unsigned long attrs=space.f_flag&15;CK(mount(base,base,NULL,MS_BIND,NULL)==0);CK(mount(NULL,base,NULL,MS_BIND|MS_REMOUNT|MS_RDONLY|MS_NOSUID|attrs,NULL)==0);char abs[160];snprintf(abs,sizeof(abs),"%s/renamed",base);f=open(abs,O_RDWR|O_NONBLOCK);CK(f>=0);pair(f,f,"readonly mount stream");CK(close(f)==0);snprintf(abs,sizeof(abs),"%s/denied",base);errno=0;CK(mkfifo(abs,0600)==-1&&errno==EROFS);CK(umount2(base,MNT_DETACH)==0);
 CK(unlink("renamed")==0&&unlink("blocking")==0);printf("FIFO FIXTURE PASS checks=%u brook_cases=%d disk=%d\n",checks,brook||brookdisk,disk);return 0;
}
