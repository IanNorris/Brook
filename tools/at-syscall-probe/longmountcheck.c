#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
static unsigned checks;
static int brookCases;
#define CK(x) do{++checks;if(!(x)){fprintf(stderr,"LONGMOUNT FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static void map(const char* path,unsigned id){char text[64];int n=snprintf(text,sizeof(text),"0 %u 1\n",id);int f=open(path,O_WRONLY);CK(f>=0);CK(write(f,text,n)==n);CK(close(f)==0);}
static void readvalue(const char* path){int f=open(path,O_RDONLY);CK(f>=0);char c=0;CK(read(f,&c,1)==1&&c=='A');CK(close(f)==0);}
static void reported(const char* path){char full[512],proc[64],actual[512];CK(getcwd(full,sizeof(full)));size_t n=strlen(full);CK(n+strlen(path)+2<sizeof(full));full[n++]='/';strcpy(full+n,path);int f=open(path,O_PATH|O_DIRECTORY);if(f<0)fprintf(stderr,"LONGMOUNT open path %s errno=%d\n",path,errno);CK(f>=0);snprintf(proc,sizeof(proc),"/proc/self/fd/%d",f);ssize_t len=readlink(proc,actual,sizeof(actual)-1);CK(len>0);actual[len]=0;CK(!strcmp(actual,full));CK(close(f)==0);for(unsigned i=0;i<2;++i){const char* name=i?(brookCases?"/proc/mounts":"/proc/self/mounts"):"/proc/self/mountinfo";f=open(name,O_RDONLY);if(f<0)fprintf(stderr,"LONGMOUNT open proc %s errno=%d\n",name,errno);CK(f>=0);char* text=calloc(1,262144);CK(text);size_t used=0;ssize_t count;while((count=read(f,text+used,262143-used))>0){used+=(size_t)count;CK(used<262143);}CK(count==0);CK(strstr(text,full));free(text);CK(close(f)==0);}}
int main(int argc,char** argv){
 setbuf(stdout,NULL);int brook=argc==2&&!strcmp(argv[1],"--brook");CK(argc==1||brook);brookCases=brook;CK(geteuid()!=0);uid_t uid=geteuid();gid_t gid=getegid();pid_t child=syscall(SYS_clone,(unsigned long)(CLONE_NEWUSER|CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(child>=0);if(child){int status;CK(waitpid(child,&status,0)==child);CK(WIFEXITED(status)&&WEXITSTATUS(status)==0);printf("LONGMOUNT PARENT PASS checks=%u\n",checks);return 0;}
 alarm(60);map("/proc/self/uid_map",uid);int f=open("/proc/self/setgroups",O_WRONLY);CK(f>=0);CK(write(f,"deny\n",5)==5);CK(close(f)==0);map("/proc/self/gid_map",gid);CK(mount(NULL,"/",NULL,MS_PRIVATE|MS_REC,NULL)==0);char base[]="/tmp/longmount-XXXXXX";CK(mkdtemp(base));CK(mount("tmpfs",base,"tmpfs",6,"mode=0755")==0);CK(chdir(base)==0);
 char leaf[201],parent[181],target[512],value[512],copied[512];memset(leaf,'x',200);leaf[200]=0;memset(parent,'n',180);parent[180]=0;CK(mkdir("source",0700)==0&&mkdir("parent",0700)==0&&mkdir("copy",0700)==0);snprintf(target,sizeof(target),"parent/%s",leaf);CK(mkdir(target,0700)==0);f=open("source/value",O_CREAT|O_EXCL|O_RDWR,0600);CK(f>=0);CK(write(f,"A",1)==1);CK(close(f)==0);CK(mount("source",target,NULL,MS_BIND,NULL)==0);reported(target);snprintf(value,sizeof(value),"parent/%s/value",leaf);readvalue(value);
 CK(rename("parent",parent)==0);snprintf(target,sizeof(target),"%s/%s",parent,leaf);reported(target);snprintf(value,sizeof(value),"%s/%s/value",parent,leaf);readvalue(value);CK(mount(parent,"copy",NULL,MS_BIND|MS_REC,NULL)==0);snprintf(copied,sizeof(copied),"copy/%s",leaf);reported(copied);snprintf(value,sizeof(value),"copy/%s/value",leaf);readvalue(value);
 child=syscall(SYS_clone,(unsigned long)(CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(child>=0);if(!child){reported(target);CK(umount2(target,MNT_DETACH)==0);char childValue[512];snprintf(childValue,sizeof(childValue),"%s/%s/value",parent,leaf);errno=0;CK(open(childValue,O_RDONLY)==-1&&errno==ENOENT);_exit(0);}int status;CK(waitpid(child,&status,0)==child);CK(WIFEXITED(status)&&WEXITSTATUS(status)==0);reported(target);
 CK(umount2("copy",MNT_DETACH)==0);CK(umount2(target,MNT_DETACH)==0);CK(chdir("/")==0);CK(umount2(base,MNT_DETACH)==0);CK(rmdir(base)==0);printf("LONGMOUNT FIXTURE PASS checks=%u brook_cases=%d\n",checks,brook);return 0;
}
