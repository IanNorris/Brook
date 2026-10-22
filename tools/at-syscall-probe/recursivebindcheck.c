// Same executable on Linux and Brook; private disposable VM fixture only.
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sched.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CK(x) do {++checks;if(!(x)){fprintf(stderr,"RBIND FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static void map(const char* path,unsigned id){char t[64];int n=snprintf(t,sizeof(t),"0 %u 1\n",id);int f=open(path,O_WRONLY);CK(f>=0);CK(write(f,t,n)==n);CK(!close(f));}
static void dir(const char* p){CK(!mkdir(p,0700));}
static void value(const char* p,char c){int f=open(p,O_CREAT|O_EXCL|O_WRONLY,0600);CK(f>=0);CK(write(f,&c,1)==1);CK(!close(f));}
static void readat(int fd,const char* p,char c){int f=openat(fd,p,O_RDONLY);CK(f>=0);char b=0;CK(read(f,&b,1)==1&&b==c);CK(!close(f));}
static void missing(int fd,const char* p){errno=0;CK(openat(fd,p,O_RDONLY)==-1&&errno==ENOENT);}
static void memory(const char* p){CK(!mount("tmpfs",p,"tmpfs",MS_NODEV|MS_NOSUID,NULL));}
int main(int argc,char** argv){
 setbuf(stdout,NULL);CK(argc==2&&!strcmp(argv[1],"--disposable-vm"));CK(geteuid()!=0);alarm(60);
 char base[]="/tmp/rbind-XXXXXX";CK(mkdtemp(base));uid_t uid=geteuid();gid_t gid=getegid();
 errno=0;CK(mount("/",base,NULL,MS_BIND|MS_REC,NULL)==-1&&errno==EPERM);
 pid_t child=syscall(SYS_clone,(unsigned long)(CLONE_NEWUSER|CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(child>=0);
 if(child){int s;CK(waitpid(child,&s,0)==child);CK(WIFEXITED(s)&&!WEXITSTATUS(s));CK(!rmdir(base));printf("RBIND PARENT PASS checks=%u\n",checks);return 0;}
 alarm(60);map("/proc/self/uid_map",uid);int f=open("/proc/self/setgroups",O_WRONLY);CK(f>=0&&write(f,"deny\n",5)==5&&!close(f));map("/proc/self/gid_map",gid);
 CK(!mount(NULL,"/",NULL,MS_PRIVATE|MS_REC,NULL));memory(base);CK(!chdir(base));
 dir("src");dir("dst");dir("plain");dir("other");dir("src/sub");dir("src/snapshot");value("src/value",'A');value("dst/value",'T');
 memory("src/sub");value("src/sub/value",'B');memory("src/sub");value("src/sub/value",'C');dir("src/sub/deep");memory("src/sub/deep");value("src/sub/deep/value",'D');
 memory("other");value("other/value",'O');
 int covered=open("dst",O_RDONLY|O_DIRECTORY),source=open("src",O_RDONLY|O_DIRECTORY);CK(covered>=0&&source>=0);
 CK(!mount("src","plain",NULL,MS_BIND,NULL));missing(AT_FDCWD,"plain/sub/value");
 CK(!mount("src","dst",NULL,MS_BIND|MS_REC|MS_SILENT|0xc0ed0000UL,NULL));
 readat(AT_FDCWD,"dst/value",'A');readat(AT_FDCWD,"dst/sub/value",'C');readat(AT_FDCWD,"dst/sub/deep/value",'D');readat(covered,"value",'T');
 struct stat s,t;CK(!stat("src/sub/deep",&s)&&!stat("dst/sub/deep",&t));CK(s.st_dev==t.st_dev&&s.st_ino==t.st_ino);
 CK(!rename("src","renamed"));readat(AT_FDCWD,"dst/sub/deep/value",'D');CK(!rename("renamed","src"));
 int held=open("dst/sub/deep",O_RDONLY|O_DIRECTORY);CK(held>=0);CK(!umount2("src/sub",MNT_DETACH));
 readat(AT_FDCWD,"src/sub/value",'B');readat(AT_FDCWD,"dst/sub/value",'C');
 CK(!umount2("dst",MNT_DETACH));readat(held,"value",'D');readat(AT_FDCWD,"dst/value",'T');CK(!close(held));
 // Finite recursive snapshot even when destination is inside source.
 CK(!mount("src","src/snapshot",NULL,MS_BIND|MS_REC,NULL));readat(AT_FDCWD,"src/snapshot/sub/value",'B');missing(AT_FDCWD,"src/snapshot/snapshot/value");CK(!umount2("src/snapshot",MNT_DETACH));
 CK(!mount("src","src",NULL,MS_BIND|MS_REC|MS_SILENT,NULL));readat(AT_FDCWD,"src/sub/value",'B');CK(!umount2("src",MNT_DETACH));readat(source,"sub/value",'B');
 CK(!umount2("plain",MNT_DETACH));CK(!umount2("src/sub",MNT_DETACH));CK(!umount2("other",MNT_DETACH));
 CK(!close(source)&&!close(covered));CK(!chdir("/"));CK(!umount2(base,MNT_DETACH));printf("RBIND FIXTURE PASS checks=%u\n",checks);_exit(0);
}
