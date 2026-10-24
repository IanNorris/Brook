// Retained directory proc-FD capabilities; run only in a disposable VM.
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sched.h>
#include <pthread.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CK(x) do {++checks;if(!(x)){fprintf(stderr,"PDIRFD FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static void map(const char* p,unsigned id){char t[64];int n=snprintf(t,sizeof(t),"0 %u 1\n",id);int f=open(p,O_WRONLY);CK(f>=0);CK(write(f,t,n)==n);CK(!close(f));}
static void waitok(pid_t p){int status;CK(waitpid(p,&status,0)==p);CK(WIFEXITED(status)&&!WEXITSTATUS(status));}
static void value(const char* path,char byte){int f=open(path,O_CREAT|O_EXCL|O_WRONLY,0600);CK(f>=0);CK(write(f,&byte,1)==1&&!close(f));}
static char readvalue(int fd){int f=openat(fd,"value",O_RDONLY);CK(f>=0);char c=0;CK(read(f,&c,1)==1&&!close(f));return c;}
static void linkname(int fd,char* name,size_t size){snprintf(name,size,"/proc/self/fd/%d",fd);}
static void reopen(int fd,char expected){
 char name[64];linkname(fd,name,sizeof(name));DIR* d=opendir(name);CK(d);int seen=0;struct dirent* ent;
 while((ent=readdir(d)))if(!strcmp(ent->d_name,"value"))++seen;
 CK(seen==1);CK(readvalue(dirfd(d))==expected);CK(!closedir(d));
 char suffix[80];snprintf(suffix,sizeof(suffix),"%s/value",name);int f=open(suffix,O_RDONLY);CK(f>=0);char byte=0;CK(read(f,&byte,1)==1&&byte==expected&&!close(f));
}
static int stopped;static unsigned observations;static char raceName[64];
static void* reader(void* ignored){
 (void)ignored;
 while(!__atomic_load_n(&stopped,__ATOMIC_ACQUIRE)) {
  int fd=open(raceName,O_RDONLY|O_DIRECTORY);
  if(fd<0){if(errno!=ENOENT){perror("racing directory follow");exit(1);}continue;}
  int f=openat(fd,"value",O_RDONLY);if(f<0){perror("racing openat");exit(1);}char c=0;
  if(read(f,&c,1)!=1 || (c!='A'&&c!='C') || close(f) || close(fd)){fputs("PDIRFD bad captured directory\n",stderr);exit(1);}
  __atomic_add_fetch(&observations,1,__ATOMIC_RELEASE);
 }
 return NULL;
}
int main(int argc,char** argv){
 setbuf(stdout,NULL);alarm(60);int brook=argc==3&&!strcmp(argv[2],"--brook");CK((argc==2||brook)&&!strcmp(argv[1],"--disposable-vm"));CK(geteuid()!=0);
 char base[]="/tmp/pdir-XXXXXX";CK(mkdtemp(base));uid_t uid=geteuid();gid_t gid=getegid();
 pid_t child=syscall(SYS_clone,(unsigned long)(CLONE_NEWUSER|CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(child>=0);
 if(child){waitok(child);CK(!rmdir(base));printf("PDIRFD PARENT PASS checks=%u\n",checks);return 0;}
 map("/proc/self/uid_map",uid);int f=open("/proc/self/setgroups",O_WRONLY);CK(f>=0&&write(f,"deny\n",5)==5&&!close(f));map("/proc/self/gid_map",gid);
 CK(!mount(NULL,"/",NULL,MS_PRIVATE|MS_REC,NULL));CK(!mount("tmpfs",base,"tmpfs",MS_NOSUID|MS_NODEV,NULL));CK(!chdir(base));
 CK(!mkdir("src",0700));value("src/value",'A');int original=open("src",O_RDONLY|O_DIRECTORY),metadata=open("src",O_PATH|O_DIRECTORY);CK(original>=0&&metadata>=0);reopen(original,'A');reopen(metadata,'A');
 CK(!rename("src","renamed")&&!mkdir("src",0700));value("src/value",'C');reopen(original,'A');reopen(metadata,'A');
 char name[80];linkname(original,name,sizeof(name));errno=0;CK(open(name,O_RDONLY|O_NOFOLLOW)==-1&&errno==ELOOP);
 int link=open(name,O_PATH|O_NOFOLLOW);CK(link>=0);struct stat st;CK(!fstat(link,&st)&&S_ISLNK(st.st_mode));CK(!close(link));
 int other=open("src",O_PATH|O_DIRECTORY);CK(other>=0);int slot=fcntl(original,F_DUPFD,200);CK(slot>=0);linkname(slot,raceName,sizeof(raceName));pthread_t thread;CK(!pthread_create(&thread,NULL,reader,NULL));while(!__atomic_load_n(&observations,__ATOMIC_ACQUIRE))sched_yield();
 for(unsigned i=0;i<300;++i){CK(dup2(original,slot)==slot&&!close(slot));CK(dup2(other,slot)==slot&&!close(slot));}
 __atomic_store_n(&stopped,1,__ATOMIC_RELEASE);CK(!pthread_join(thread,NULL));CK(!close(other));printf("PDIRFD RACE PASS observations=%u\n",observations);
 CK(!mount("tmpfs","renamed","tmpfs",MS_NOSUID|MS_NODEV,NULL));value("renamed/value",'B');reopen(original,'A');
 int upper=open("renamed",O_RDONLY|O_DIRECTORY);CK(upper>=0);reopen(upper,'B');CK(!umount2("renamed",MNT_DETACH));reopen(upper,'B');CK(!close(upper));
 // Other-process inspection intentionally remains narrower in Brook.
 int ready[2],finish[2];CK(!pipe(ready)&&!pipe(finish));pid_t observer=fork();CK(observer>=0);
 if(!observer){close(ready[0]);close(finish[1]);char c='R';if(write(ready[1],&c,1)!=1||read(finish[0],&c,1)!=1)_exit(1);_exit(0);}
 CK(!close(ready[1])&&!close(finish[0]));char byte=0;CK(read(ready[0],&byte,1)==1);snprintf(name,sizeof(name),"/proc/%ld/fd/%d",(long)observer,original);errno=0;f=open(name,O_RDONLY|O_DIRECTORY);
 if(brook)CK(f==-1&&errno==EACCES);else CK(f>=0&&readvalue(f)=='A'&&!close(f));
 CK(write(finish[1],"F",1)==1&&!close(finish[1])&&!close(ready[0]));waitok(observer);
 f=open("src/value",O_RDONLY);CK(f>=0);linkname(f,name,sizeof(name));errno=0;int file=open(name,O_RDONLY);CK(file>=0&&!close(file));
 CK(!lstat(name,&st)&&S_ISLNK(st.st_mode));char reported[256];CK(readlink(name,reported,sizeof(reported))>0);
 link=open(name,O_PATH|O_NOFOLLOW);CK(link>=0&&!fstat(link,&st)&&S_ISLNK(st.st_mode)&&!close(link));
 errno=0;int sr=stat(name,&st);CK(!sr&&S_ISREG(st.st_mode));
 errno=0;int ar=access(name,R_OK);CK(!ar);CK(!close(f));
 int memory=syscall(SYS_memfd_create,"procdirfd-regression",0);CK(memory>=0);CK(write(memory,"M",1)==1);linkname(memory,name,sizeof(name));int proxy=open(name,O_RDWR);CK(proxy>=0);byte=0;CK(read(proxy,&byte,1)==1&&byte=='M');CK(!close(proxy));errno=0;CK(open(name,O_RDONLY|O_DIRECTORY)==-1&&errno==ENOTDIR);errno=0;CK(open(name,O_RDONLY|O_NOFOLLOW)==-1&&errno==ELOOP);CK(!close(memory));
 CK(!close(original)&&!close(metadata));CK(!chdir("/")&&!umount2(base,MNT_DETACH));printf("PDIRFD FIXTURE PASS checks=%u brook_cases=%d\n",checks,brook);_exit(0);
}
