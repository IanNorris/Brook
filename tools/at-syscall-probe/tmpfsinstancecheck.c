// Portable tmpfs instance contract; --brook adds bounded backend checks.
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sched.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CK(x) do { ++checks;if(!(x)){fprintf(stderr,"TMPFS INSTANCE FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);} } while(0)
static void map(const char* p,unsigned id) {char t[64];int n=snprintf(t,sizeof(t),"0 %u 1\n",id);int f=open(p,O_WRONLY);CK(f>=0);CK(write(f,t,n)==n);CK(!close(f));}
static void sendfd(int s,int fd){char c='F',buf[CMSG_SPACE(sizeof(int))]={0};struct iovec io={&c,1};struct msghdr m={0};m.msg_iov=&io;m.msg_iovlen=1;m.msg_control=buf;m.msg_controllen=sizeof(buf);struct cmsghdr* h=CMSG_FIRSTHDR(&m);h->cmsg_level=SOL_SOCKET;h->cmsg_type=SCM_RIGHTS;h->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(h),&fd,sizeof(fd));CK(sendmsg(s,&m,0)==1);}
static int recvfd(int s){char c,buf[CMSG_SPACE(sizeof(int))]={0};struct iovec io={&c,1};struct msghdr m={0};m.msg_iov=&io;m.msg_iovlen=1;m.msg_control=buf;m.msg_controllen=sizeof(buf);CK(recvmsg(s,&m,0)==1&&c=='F'&&!(m.msg_flags&MSG_CTRUNC));struct cmsghdr* h=CMSG_FIRSTHDR(&m);CK(h&&h->cmsg_level==SOL_SOCKET&&h->cmsg_type==SCM_RIGHTS);int fd;memcpy(&fd,CMSG_DATA(h),sizeof(fd));CK(fd>=0);return fd;}
int main(int argc,char** argv) {
 setbuf(stdout,NULL);CK((argc==2||(argc==3&&!strcmp(argv[2],"--brook")))&&!strcmp(argv[1],"--disposable-vm"));int brook=argc==3;CK(geteuid()!=0);
 alarm(60);uid_t uid=geteuid();gid_t gid=getegid();int sv[2]={-1,-1};if(brook)CK(!socketpair(AF_UNIX,SOCK_STREAM,0,sv));
 pid_t child=syscall(SYS_clone,(unsigned long)(CLONE_NEWUSER|CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(child>=0);
 if(child) {
 if(brook){CK(!close(sv[1]));int foreign=recvfd(sv[0]),directory=recvfd(sv[0]);CK(!fchown(foreign,2000,200));CK(!fchmod(foreign,0000));CK(!fchown(directory,3000,300));CK(write(sv[0],"G",1)==1);char c;CK(read(sv[0],&c,1)==1&&c=='R');CK(!fchown(foreign,uid,gid));CK(!fchown(directory,uid,gid));CK(!fchmod(foreign,0600));CK(!close(foreign)&&!close(directory));CK(write(sv[0],"G",1)==1);CK(!close(sv[0]));}
 int s;CK(waitpid(child,&s,0)==child);CK(WIFEXITED(s)&&!WEXITSTATUS(s));printf("TMPFS INSTANCE PARENT PASS checks=%u\n",checks);return 0;}
 alarm(60);if(brook)CK(!close(sv[0]));
 map("/proc/self/uid_map",uid);int f=open("/proc/self/setgroups",O_WRONLY);CK(f>=0);CK(write(f,"deny\n",5)==5);CK(!close(f));map("/proc/self/gid_map",gid);
 CK(!mount(NULL,"/",NULL,MS_PRIVATE|MS_REC,NULL));
 char base[]="/tmp/tmpfs-instance-XXXXXX";CK(mkdtemp(base));char a[256],b[256],file[280];
 snprintf(a,sizeof(a),"%s/a",base);snprintf(b,sizeof(b),"%s/b",base);CK(!mkdir(a,0700)&&!mkdir(b,0700));
 int covered=open(a,O_RDONLY|O_DIRECTORY);CK(covered>=0);
 CK(!mount("tmpfs",a,"tmpfs",MS_NODEV|MS_NOSUID,NULL));CK(!mount("tmpfs",b,"tmpfs",MS_NODEV|MS_NOSUID,NULL));
 struct stat sa,sb;CK(!stat(a,&sa)&&!stat(b,&sb));CK(sa.st_dev!=sb.st_dev);CK((sa.st_mode&07777)==01777&&sa.st_uid==0&&sa.st_gid==0);
 struct statfs fs;CK(!statfs(a,&fs)&&fs.f_type==0x01021994);struct statvfs v;CK(!statvfs(a,&v));CK((v.f_flag&ST_NOSUID)!=0);
 // NODEV is a Linux mount flag even where libc omits its statvfs macro.
 CK((v.f_flag&4)!=0);
 int da=open(a,O_RDONLY|O_DIRECTORY),db=open(b,O_RDONLY|O_DIRECTORY);CK(da>=0&&db>=0);umask(0022);
 f=openat(da,"value",O_CREAT|O_EXCL|O_RDWR,0666);CK(f>=0);CK(write(f,"one",3)==3);CK(!fstat(f,&sa)&&(sa.st_mode&0777)==0644);
 errno=0;CK(openat(da,"value",O_CREAT|O_EXCL|O_RDWR,0666)==-1&&errno==EEXIST);
 errno=0;CK(openat(db,"value",O_RDONLY)==-1&&errno==ENOENT);errno=0;CK(openat(covered,"value",O_RDONLY)==-1&&errno==ENOENT);
 if(brook) {
  int unmapped=openat(da,"unmapped",O_CREAT|O_EXCL|O_RDWR,0600);CK(unmapped>=0);sendfd(sv[1],unmapped);sendfd(sv[1],da);char ack;CK(read(sv[1],&ack,1)==1&&ack=='G');
  struct stat deniedBefore,deniedAfter;CK(!fstat(unmapped,&deniedBefore));
  errno=0;CK(fchmod(unmapped,0777)==-1&&errno==EPERM);errno=0;CK(fchown(unmapped,0,0)==-1&&errno==EPERM);errno=0;CK(unlinkat(da,"unmapped",0)==-1&&errno==EPERM);
  CK(!fstat(unmapped,&deniedAfter)&&deniedAfter.st_uid==deniedBefore.st_uid&&deniedAfter.st_gid==deniedBefore.st_gid&&deniedAfter.st_mode==deniedBefore.st_mode&&deniedAfter.st_nlink==deniedBefore.st_nlink);
  CK(write(sv[1],"R",1)==1);CK(read(sv[1],&ack,1)==1&&ack=='G');CK(!close(sv[1]));CK(!unlinkat(da,"unmapped",0)&&!close(unmapped));
  errno=0;CK(chown(base,0,0)==-1&&errno==EPERM);
  CK(!fchown(f,0,0));CK(!fstat(f,&sa)&&sa.st_uid==0&&sa.st_gid==0);
  errno=0;CK(fchown(f,12345,(gid_t)-1)==-1&&errno==EINVAL);
  CK(!fchownat(da,"value",0,0,0));
  for(int shared=0;shared<2;++shared){
   char* view=mmap(NULL,4096,PROT_READ|PROT_WRITE,shared?MAP_SHARED:MAP_PRIVATE,f,0);CK(view!=MAP_FAILED);CK(!memcmp(view,"one",3));
   view[0]='X';char first=0;CK(pread(f,&first,1,0)==1&&first==(shared?'X':'o'));
   if(shared){CK(!msync(view,4096,MS_SYNC));view[0]='o';CK(!msync(view,4096,MS_SYNC));}
   errno=0;CK(ftruncate(f,0)==-1&&errno==EBUSY);CK(!munmap(view,4096));
  }
  errno=0;CK(mount("tmpfs",b,"tmpfs",MS_NODEV|MS_NOSUID,"size=1M")==-1&&errno==EINVAL);
  struct stat preserved;CK(!stat(b,&preserved)&&preserved.st_dev==sb.st_dev);
  errno=0;CK(mount("tmpfs",b,"tmpfs",MS_NODEV|MS_NOSUID|MS_NOEXEC,NULL)==-1&&errno==EINVAL);
  CK(!mount("tmpfs","/dev","tmpfs",MS_NODEV|MS_NOSUID,NULL));errno=0;CK(open("/dev/null",O_WRONLY)==-1&&errno==ENOENT);CK(!umount2("/dev",MNT_DETACH));
 }
 CK(!mkdirat(da,"dir",0755));CK(!symlinkat("value",da,"link"));char text[8]={0};CK(readlinkat(da,"link",text,sizeof(text))==5&&!memcmp(text,"value",5));
 CK(!renameat(da,"value",da,"renamed"));CK(!unlinkat(da,"renamed",0));CK(!fstat(f,&sa)&&sa.st_nlink==0);CK(pread(f,text,3,0)==3&&!memcmp(text,"one",3));
 if(brook){
  CK(!ftruncate(f,0));static char fill[65536];size_t total=0;ssize_t n;
  while((n=write(f,fill,sizeof(fill)))>0){total+=(size_t)n;CK(total<16*1024*1024);}
  CK(n==-1&&errno==ENOSPC&&total>15*1024*1024);
  struct stat exhausted;CK(!fstat(f,&exhausted)&&exhausted.st_size==(off_t)(total+3));off_t stopped=lseek(f,0,SEEK_CUR);CK(stopped==exhausted.st_size);errno=0;CK(write(f,fill,1)==-1&&errno==ENOSPC);CK(lseek(f,0,SEEK_CUR)==stopped);struct stat unchanged;CK(!fstat(f,&unchanged)&&unchanged.st_size==exhausted.st_size);
  struct statvfs held,reclaimed;CK(!fstatvfs(da,&held));CK(!close(f));sync();CK(!fstatvfs(da,&reclaimed));CK(reclaimed.f_bfree>held.f_bfree&&reclaimed.f_ffree==held.f_ffree+1);
  f=openat(da,"resize",O_CREAT|O_EXCL|O_RDWR,0600);CK(f>=0);CK(!unlinkat(da,"resize",0));
 }
 CK(!ftruncate(f,1ULL<<33));CK(pread(f,text,1,(1ULL<<33)-1)==1&&text[0]==0);CK(pwrite(f,"Z",1,(1ULL<<33)-1)==1);CK(!ftruncate(f,3));CK(!close(f));
 CK(!unlinkat(da,"link",0)&&!unlinkat(da,"dir",AT_REMOVEDIR));
 CK(!umount2(a,MNT_DETACH));CK(!fstat(da,&sa)&&sa.st_dev!=sb.st_dev);CK(!close(da));CK(!close(covered));
 CK(!umount2(b,MNT_DETACH));CK(!close(db));CK(!rmdir(a)&&!rmdir(b)&&!rmdir(base));
 snprintf(file,sizeof(file),"%u",checks);printf("TMPFS INSTANCE FIXTURE PASS checks=%s\n",file);_exit(0);
}
