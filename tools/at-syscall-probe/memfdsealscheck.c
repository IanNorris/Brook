// Real memfd seal/mapping lifetime acceptance; the temporary vnode fixture is removed.
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CK(x) do {++checks;if(!(x)){fprintf(stderr,"MSEALS FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
#define ERR(x,e) do {errno=0;CK((x)==-1&&errno==(e));}while(0)
static int fresh(unsigned flags){int f=syscall(SYS_memfd_create,"brook-seals",flags);CK(f>=0);return f;}
static int seals(int f){int bits=fcntl(f,F_GET_SEALS);CK(bits>=0);return bits;}
static void waitok(pid_t child){int status;CK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));}
static int reopened(int fd,int flags){char path[64];snprintf(path,sizeof(path),"/proc/self/fd/%d",fd);int copy=open(path,flags);CK(copy>=0);return copy;}
static int transferred(int fd){
 int pair[2];CK(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));char byte='S',control[CMSG_SPACE(sizeof(int))];struct iovec iov={&byte,1};struct msghdr msg={0};
 msg.msg_iov=&iov;msg.msg_iovlen=1;msg.msg_control=control;msg.msg_controllen=sizeof(control);memset(control,0,sizeof(control));
 struct cmsghdr* header=CMSG_FIRSTHDR(&msg);header->cmsg_level=SOL_SOCKET;header->cmsg_type=SCM_RIGHTS;header->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(header),&fd,sizeof(fd));CK(sendmsg(pair[0],&msg,0)==1);
 memset(control,0,sizeof(control));msg.msg_controllen=sizeof(control);CK(recvmsg(pair[1],&msg,0)==1);header=CMSG_FIRSTHDR(&msg);CK(header&&header->cmsg_level==SOL_SOCKET&&header->cmsg_type==SCM_RIGHTS);int received=-1;memcpy(&received,CMSG_DATA(header),sizeof(received));CK(received>=0&&!close(pair[0])&&!close(pair[1]));return received;
}
static void copychecks(void){
 int source=fresh(0),target=fresh(0);char pattern[8192],readback[8192];for(unsigned i=0;i<sizeof(pattern);++i)pattern[i]=(char)(i%113);
 CK(write(source,pattern,sizeof(pattern))==(ssize_t)sizeof(pattern)&&!lseek(source,0,SEEK_SET));
 CK(sendfile(target,source,NULL,1000)==1000&&lseek(source,0,SEEK_CUR)==1000);CK(pread(target,readback,1000,0)==1000&&!memcmp(readback,pattern,1000));
 off_t explicit=7;CK(sendfile(target,source,&explicit,100)==100&&explicit==107&&lseek(source,0,SEEK_CUR)==1000);CK(pread(target,readback,100,1000)==100&&!memcmp(readback,pattern+7,100));CK(!close(target));
 target=fresh(0);CK(write(target,"Y",1)==1&&!fcntl(target,F_SETFL,O_APPEND)&&!lseek(source,0,SEEK_SET));ERR(sendfile(target,source,NULL,1),EINVAL);struct stat st;CK(!fstat(target,&st)&&st.st_size==1&&!lseek(source,0,SEEK_CUR)&&!close(target));
 char name[]="/tmp/mseals-sendfile-XXXXXX";target=mkstemp(name);CK(target>=0&&write(target,"Y",1)==1&&!fcntl(target,F_SETFL,O_APPEND));ERR(sendfile(target,source,NULL,1),EINVAL);CK(!fstat(target,&st)&&st.st_size==1&&!lseek(source,0,SEEK_CUR)&&!close(target)&&!unlink(name));
 int pair[2];CK(!pipe2(pair,O_NONBLOCK));char filler[4096];memset(filler,'P',sizeof(filler));size_t filled=0;
 for(;;){ssize_t n=write(pair[1],filler,sizeof(filler));if(n<0){CK(errno==EAGAIN);break;}CK(n>0);filled+=(size_t)n;CK(filled<=1024*1024);}
 CK(filled>=4096&&read(pair[0],readback,4096)==4096);CK(sendfile(pair[1],source,NULL,8192)==4096&&lseek(source,0,SEEK_CUR)==4096);
 size_t old=filled-4096;while(old){size_t amount=old>sizeof(readback)?sizeof(readback):old;ssize_t n=read(pair[0],readback,amount);CK(n>0);for(ssize_t i=0;i<n;++i)CK(readback[i]=='P');old-=(size_t)n;}
 CK(read(pair[0],readback,4096)==4096&&!memcmp(readback,pattern,4096));CK(!close(pair[0])&&!close(pair[1])&&!close(source));puts("MSEALS SENDFILE PASS");
}
static int raceFd,start,done,mapped,mapError;
static void* mapper(void* unused){
 (void)unused;while(!__atomic_load_n(&start,__ATOMIC_ACQUIRE)){}
 void* p=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,raceFd,0);
 if(p==MAP_FAILED)mapError=errno;else {mapped=1;while(!__atomic_load_n(&done,__ATOMIC_ACQUIRE)){}if(munmap(p,4096)){perror("race unmap");exit(1);}}
 return NULL;
}
int main(int argc,char** argv){
 setbuf(stdout,NULL);alarm(60);int brook=argc==2&&!strcmp(argv[1],"--brook");CK(argc==1||brook);
 int f=fresh(0);CK(seals(f)==F_SEAL_SEAL);ERR(fcntl(f,F_ADD_SEALS,F_SEAL_SHRINK),EPERM);CK(!close(f));
 f=fresh(MFD_ALLOW_SEALING);CK(!syscall(SYS_fcntl,f,F_ADD_SEALS,0x100000002ULL)&&seals(f)==F_SEAL_SHRINK);CK(!close(f));
 f=fresh(MFD_ALLOW_SEALING|MFD_CLOEXEC);CK(!seals(f)&&(fcntl(f,F_GETFD)&FD_CLOEXEC));CK(write(f,"ABCDE",5)==5);CK(!fallocate(f,FALLOC_FL_KEEP_SIZE,8192,4096));
 ERR(fcntl(f,F_ADD_SEALS,0x80000000u),EINVAL);CK(!seals(f));int ro=reopened(f,O_RDONLY);ERR(fcntl(ro,F_ADD_SEALS,0),EPERM);ERR(fcntl(ro,F_ADD_SEALS,0x80000000u),EPERM);CK(!close(ro));
 CK(!fcntl(f,F_ADD_SEALS,F_SEAL_GROW));CK(pwrite(f,"Z",1,1)==1);ERR(pwrite(f,"Z",1,5),EPERM);ERR(ftruncate(f,6),EPERM);
 ERR(fallocate(f,FALLOC_FL_KEEP_SIZE,8192,4096),EPERM);CK(!fallocate(f,FALLOC_FL_KEEP_SIZE,0,1));struct stat st;CK(!fstat(f,&st)&&st.st_size==5);ERR(fallocate(f,0,8192,4096),EPERM);
 CK(!ftruncate(f,2));CK(!fcntl(f,F_ADD_SEALS,F_SEAL_SHRINK));ERR(ftruncate(f,1),EPERM);CK(!ftruncate(f,2));CK(!close(f));
 f=fresh(MFD_ALLOW_SEALING);CK(write(f,"ABCDE",5)==5&&!ftruncate(f,2)&&!ftruncate(f,5));char bytes[5]={0};CK(pread(f,bytes,5,0)==5&&!memcmp(bytes,"AB\0\0\0",5));CK(!close(f));
 f=fresh(MFD_ALLOW_SEALING);CK(!ftruncate(f,4096));char* p=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,f,0);CK(p!=MAP_FAILED);p[0]='M';
 ERR(fcntl(f,F_ADD_SEALS,15),EBUSY);CK(!seals(f));
 if(brook){ERR(ftruncate(f,1),EBUSY);CK(!fstat(f,&st)&&st.st_size==4096&&p[0]=='M');}
 int ready[2],finish[2];CK(!pipe(ready)&&!pipe(finish));pid_t child=fork();CK(child>=0);
 if(!child){close(ready[0]);close(finish[1]);char byte='R';if(write(ready[1],&byte,1)!=1||read(finish[0],&byte,1)!=1||p[0]!='M'||munmap(p,4096))_exit(1);_exit(0);}
 CK(!close(ready[1])&&!close(finish[0]));char byte;CK(read(ready[0],&byte,1)==1&&!munmap(p,4096));ERR(fcntl(f,F_ADD_SEALS,F_SEAL_WRITE),EBUSY);
 CK(write(finish[1],"F",1)==1&&!close(ready[0])&&!close(finish[1]));waitok(child);
 // Failed publication must retire the mapping admission lease.
 void* occupied=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CK(occupied!=MAP_FAILED);
 ERR(mmap(occupied,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED_NOREPLACE,f,0)==MAP_FAILED?-1:0,EEXIST);CK(!munmap(occupied,4096));
 CK(!fcntl(f,F_ADD_SEALS,F_SEAL_WRITE));p=mmap(NULL,4096,PROT_READ,MAP_SHARED,f,0);CK(p!=MAP_FAILED&&p[0]=='M');ERR(mprotect(p,4096,PROT_READ|PROT_WRITE),EACCES);CK(!munmap(p,4096));
 ERR(mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,f,0)==MAP_FAILED?-1:0,EPERM);
 ERR(write(f,"X",1),EPERM);ERR(pwrite(f,"X",1,0),EPERM);CK(!ftruncate(f,8192)&&!ftruncate(f,4096));
 int duplicate=dup(f);CK(duplicate>=0&&seals(duplicate)==F_SEAL_WRITE);CK(!fcntl(duplicate,F_ADD_SEALS,7)&&seals(f)==15);
 int received=transferred(f);CK(seals(received)==15);ERR(write(received,"X",1),EPERM);ERR(ftruncate(received,1),EPERM);ERR(ftruncate(received,8192),EPERM);ERR(fcntl(received,F_ADD_SEALS,0),EPERM);
 int input=fresh(0);CK(write(input,"X",1)==1&&!lseek(input,0,SEEK_SET));ERR(sendfile(f,input,NULL,1),EPERM);CK(lseek(input,0,SEEK_CUR)==0);CK(!close(input)&&!close(received)&&!close(duplicate)&&!close(f));
 // Brook initially rejects WRITE while even a read-only mapping is alive.
 f=fresh(MFD_ALLOW_SEALING);CK(!ftruncate(f,4096));ro=reopened(f,O_RDONLY);p=mmap(NULL,4096,PROT_READ,MAP_SHARED,ro,0);CK(p!=MAP_FAILED);
 if(brook)ERR(fcntl(f,F_ADD_SEALS,F_SEAL_WRITE),EBUSY);else CK(!fcntl(f,F_ADD_SEALS,F_SEAL_WRITE));
 CK(!munmap(p,4096)&&!fcntl(f,F_ADD_SEALS,F_SEAL_WRITE)&&!close(ro)&&!close(f));
 for(unsigned iteration=0;iteration<32;++iteration){
  raceFd=fresh(MFD_ALLOW_SEALING);CK(!ftruncate(raceFd,4096));start=done=mapped=mapError=0;pthread_t thread;CK(!pthread_create(&thread,NULL,mapper,NULL));
  __atomic_store_n(&start,1,__ATOMIC_RELEASE);errno=0;int result=fcntl(raceFd,F_ADD_SEALS,F_SEAL_WRITE),error=errno;__atomic_store_n(&done,1,__ATOMIC_RELEASE);CK(!pthread_join(thread,NULL));
  CK((!result&&!mapped&&mapError==EPERM)||(result==-1&&error==EBUSY&&mapped&&!mapError));CK(!fcntl(raceFd,F_ADD_SEALS,F_SEAL_WRITE)&&!close(raceFd));
 }
 copychecks();
 printf("MSEALS PASS checks=%u brook_cases=%d\n",checks,brook);return 0;
}
