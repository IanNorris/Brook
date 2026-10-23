#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
static unsigned checks;
#define CK(x) do{++checks;if(!(x)){fprintf(stderr,"SOCKETBIND FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static void map(const char* path,unsigned id){char text[64];int n=snprintf(text,sizeof(text),"0 %u 1\n",id);int f=open(path,O_WRONLY);CK(f>=0);CK(write(f,text,n)==n);CK(close(f)==0);}
static struct sockaddr_un address(const char* path){struct sockaddr_un a={.sun_family=AF_UNIX};CK(strlen(path)<sizeof(a.sun_path));strcpy(a.sun_path,path);return a;}
static int listener(const char* path){int f=socket(AF_UNIX,SOCK_STREAM,0);CK(f>=0);struct sockaddr_un a=address(path);CK(bind(f,(void*)&a,sizeof(a))==0);CK(listen(f,8)==0);return f;}
static void exchange(const char* path,int server,char tag){int f=socket(AF_UNIX,SOCK_STREAM,0);CK(f>=0);struct sockaddr_un a=address(path);CK(connect(f,(void*)&a,sizeof(a))==0);struct pollfd p={.fd=server,.events=POLLIN};CK(poll(&p,1,2000)==1&&(p.revents&POLLIN));int accepted=accept(server,NULL,NULL);CK(accepted>=0);CK(write(f,&tag,1)==1);char c=0;CK(read(accepted,&c,1)==1&&c==tag);c++;CK(write(accepted,&c,1)==1);CK(read(f,&c,1)==1&&c==tag+1);CK(close(accepted)==0&&close(f)==0);}
static void denied(const char* path){pid_t child=fork();CK(child>=0);if(!child){struct{uint32_t version;int32_t pid;}header={0x20080522,0};struct{uint32_t effective,permitted,inheritable;}data[2]={{0}};CK(syscall(SYS_capset,&header,data)==0);int f=socket(AF_UNIX,SOCK_STREAM,0);CK(f>=0);struct sockaddr_un a=address(path);errno=0;CK(connect(f,(void*)&a,sizeof(a))==-1&&errno==EACCES);CK(close(f)==0);_exit(0);}int status;CK(waitpid(child,&status,0)==child);CK(WIFEXITED(status)&&WEXITSTATUS(status)==0);}
static void identity(const char* path,const struct stat* original){struct stat st;CK(stat(path,&st)==0&&S_ISSOCK(st.st_mode)&&st.st_dev==original->st_dev&&st.st_ino==original->st_ino);int f=open(path,O_PATH|O_CLOEXEC);CK(f>=0);char proc[64],expected[512],actual[512];CK(getcwd(expected,sizeof(expected)));size_t n=strlen(expected);CK(n+strlen(path)+2<sizeof(expected));expected[n++]='/';strcpy(expected+n,path);snprintf(proc,sizeof(proc),"/proc/self/fd/%d",f);ssize_t len=readlink(proc,actual,sizeof(actual)-1);CK(len>0);actual[len]=0;CK(!strcmp(actual,expected));CK(close(f)==0);}
int main(int argc,char** argv){
 setbuf(stdout,NULL);int brook=argc==2&&!strcmp(argv[1],"--brook");CK(argc==1||brook);CK(geteuid()!=0);uid_t uid=geteuid();gid_t gid=getegid();pid_t child=syscall(SYS_clone,(unsigned long)(CLONE_NEWUSER|CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(child>=0);
 if(child){int status;CK(waitpid(child,&status,0)==child);CK(WIFEXITED(status)&&WEXITSTATUS(status)==0);printf("SOCKETBIND PARENT PASS checks=%u\n",checks);return 0;}
 alarm(60);map("/proc/self/uid_map",uid);int f=open("/proc/self/setgroups",O_WRONLY);CK(f>=0);CK(write(f,"deny\n",5)==5);CK(close(f)==0);map("/proc/self/gid_map",gid);CK(mount(NULL,"/",NULL,MS_PRIVATE|MS_REC,NULL)==0);
 char base[]="/tmp/socketbind-XXXXXX";CK(mkdtemp(base));CK(chdir(base)==0);CK(mkdir("mnt",0700)==0&&mkdir("copy",0700)==0);CK(mount("tmpfs","mnt","tmpfs",6,"mode=0755")==0);f=open("mnt/target",O_CREAT|O_EXCL|O_RDWR,0600);CK(f>=0);CK(write(f,"B",1)==1);CK(close(f)==0);
 int original=listener("source");struct stat src,st;CK(stat("source",&src)==0);CK(mount("source","mnt/target",NULL,MS_BIND,NULL)==0);identity("mnt/target",&src);exchange("mnt/target",original,'A');
 errno=0;CK(unlink("mnt/target")==-1&&errno==EBUSY);CK(rename("source","renamed")==0);int replacement=listener("source");CK(stat("source",&st)==0&&st.st_ino!=src.st_ino);exchange("mnt/target",original,'C');CK(chmod("renamed",0000)==0);denied("mnt/target");CK(chmod("renamed",0777)==0);
 CK(mount("source","mnt/target",NULL,MS_BIND,NULL)==0);identity("mnt/target",&st);exchange("mnt/target",replacement,'D');CK(umount2("mnt/target",MNT_DETACH)==0);identity("mnt/target",&src);
 CK(mount(NULL,"mnt/target",NULL,MS_BIND|MS_REMOUNT|MS_RDONLY|MS_NOSUID|MS_NODEV,NULL)==0);CK(access("mnt/target",W_OK)==0);exchange("mnt/target",original,'E');CK(chmod("renamed",0000)==0);denied("mnt/target");CK(chmod("renamed",0777)==0);
 CK(mount("mnt","copy",NULL,MS_BIND|MS_REC,NULL)==0);identity("copy/target",&src);exchange("copy/target",original,'F');
 child=syscall(SYS_clone,(unsigned long)(CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(child>=0);if(!child){CK(umount2("copy/target",MNT_DETACH)==0);CK(stat("copy/target",&st)==0&&S_ISREG(st.st_mode));_exit(0);}int status;CK(waitpid(child,&status,0)==child);CK(WIFEXITED(status)&&WEXITSTATUS(status)==0);identity("copy/target",&src);
 int held=open("copy/target",O_PATH);CK(held>=0);CK(unlink("renamed")==0);exchange("copy/target",original,'G');
 int live=socket(AF_UNIX,SOCK_STREAM,0);CK(live>=0);struct sockaddr_un liveAddress=address("copy/target");CK(connect(live,(void*)&liveAddress,sizeof(liveAddress))==0);int peer=accept(original,NULL,NULL);CK(peer>=0);CK(close(original)==0);char byte='Z';CK(write(live,&byte,1)==1);byte=0;CK(read(peer,&byte,1)==1&&byte=='Z');CK(write(peer,&byte,1)==1);CK(read(live,&byte,1)==1&&byte=='Z');CK(close(live)==0&&close(peer)==0);f=socket(AF_UNIX,SOCK_STREAM,0);CK(f>=0);struct sockaddr_un a=address("copy/target");errno=0;CK(connect(f,(void*)&a,sizeof(a))==-1&&errno==ECONNREFUSED);CK(close(f)==0);
 CK(umount2("mnt/target",MNT_DETACH)==0);CK(stat("mnt/target",&st)==0&&S_ISREG(st.st_mode));CK(umount2("copy",MNT_DETACH)==0);CK(fstat(held,&st)==0&&S_ISSOCK(st.st_mode)&&st.st_ino==src.st_ino&&st.st_nlink==0);CK(close(held)==0);CK(close(replacement)==0);CK(unlink("source")==0);CK(umount2("mnt",MNT_DETACH)==0);CK(rmdir("mnt")==0&&rmdir("copy")==0);CK(chdir("/")==0&&rmdir(base)==0);printf("SOCKETBIND FIXTURE PASS checks=%u brook_cases=%d\n",checks,brook);return 0;
}
