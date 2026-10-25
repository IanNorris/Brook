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
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
static unsigned checks;
#define CK(x) do { ++checks;if(!(x)){fprintf(stderr,"FILEBIND FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);} }while(0)
static void map(const char *p,unsigned id){char text[64];int n=snprintf(text,sizeof(text),"0 %u 1\n",id);int f=open(p,O_WRONLY);CK(f>=0);CK(write(f,text,n)==n);CK(close(f)==0);}
static void value(const char *p,char c){int f=open(p,O_CREAT|O_EXCL|O_WRONLY,0600);CK(f>=0);CK(write(f,&c,1)==1);CK(close(f)==0);}
static char readfd(int f){char c=0;CK(lseek(f,0,SEEK_SET)==0);CK(read(f,&c,1)==1);return c;}
static char readvalue(const char *p){int f=open(p,O_RDONLY);CK(f>=0);char c=readfd(f);CK(close(f)==0);return c;}
static void fdpath(const char *path,int flags){char expected[512],link[64],actual[512];CK(getcwd(expected,sizeof(expected)));size_t n=strlen(expected);CK(n+strlen(path)+2<sizeof(expected));expected[n++]='/';strcpy(expected+n,path);int f=open(path,flags);CK(f>=0);snprintf(link,sizeof(link),"/proc/self/fd/%d",f);ssize_t len=readlink(link,actual,sizeof(actual)-1);CK(len>0);actual[len]=0;CK(!strcmp(actual,expected));CK(close(f)==0);}
static void waitok(pid_t p){int status=0;CK(waitpid(p,&status,0)==p);CK(WIFEXITED(status)&&WEXITSTATUS(status)==0);}
static void pivot_source(const char *base){
 char pivot[128],target[160];snprintf(pivot,sizeof(pivot),"%s/pivot",base);CK(!mkdir(pivot,0700));
 pid_t child=syscall(SYS_clone,(unsigned long)(CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(child>=0);
 if(child){waitok(child);CK(!rmdir(pivot));return;}
 int machinefd=open("/etc/machine-id",O_PATH|O_CLOEXEC);CK(machinefd>=0);char machine=readvalue("/etc/machine-id");
 CK(!mount("tmpfs",pivot,"tmpfs",MS_NOSUID|MS_NODEV,NULL));
 snprintf(target,sizeof(target),"%s/oldroot",pivot);CK(!mkdir(target,0700));
 snprintf(target,sizeof(target),"%s/proc",pivot);CK(!mkdir(target,0700));CK(!mount("/proc",target,NULL,MS_BIND|MS_REC,NULL));
 CK(!chdir(pivot));CK(!syscall(SYS_pivot_root,".","oldroot"));CK(!chdir("/"));
 int root=open("/oldroot",O_PATH|O_DIRECTORY|O_CLOEXEC);CK(root>=0);
 char relative[128],expected[160];snprintf(relative,sizeof(relative),"%s/source",base+1);snprintf(expected,sizeof(expected),"/oldroot%s/source",base);
 for(unsigned i=0;i<2;++i){
  int f=openat(root,relative,(i?O_RDONLY:O_PATH)|O_CLOEXEC);CK(f>=0);
  char name[64],actual[256];snprintf(name,sizeof(name),"/proc/self/fd/%d",f);
  ssize_t n=readlink(name,actual,sizeof(actual)-1);CK(n>0);actual[n]=0;CK(!strcmp(actual,expected));CK(!close(f));
 }
 value("/target",'Z');int sourcefd=openat(root,relative,O_PATH|O_CLOEXEC),targetfd=open("/target",O_PATH|O_CLOEXEC);CK(sourcefd>=0&&targetfd>=0);
 char sourcepath[64],targetpath[64];snprintf(sourcepath,sizeof(sourcepath),"/proc/self/fd/%d",sourcefd);snprintf(targetpath,sizeof(targetpath),"/proc/self/fd/%d",targetfd);
 CK(!mount(sourcepath,targetpath,NULL,MS_BIND,NULL));CK(readvalue("/target")=='N');
 CK(!umount2("/target",MNT_DETACH));CK(readvalue("/target")=='Z');CK(!close(sourcefd)&&!close(targetfd));
 // Brook's real machine-id lives in FAT initrd, not Ext2/tmpfs.
 sourcefd=machinefd;targetfd=open("/target",O_PATH|O_CLOEXEC);CK(targetfd>=0);
 snprintf(sourcepath,sizeof(sourcepath),"/proc/self/fd/%d",sourcefd);snprintf(targetpath,sizeof(targetpath),"/proc/self/fd/%d",targetfd);
 CK(!mount(sourcepath,targetpath,NULL,MS_BIND,NULL));CK(readvalue("/target")==machine);
 CK(!umount2("/target",MNT_DETACH));CK(readvalue("/target")=='Z');CK(!close(sourcefd)&&!close(targetfd));
 CK(!close(root));_exit(0);
}
int main(int argc,char **argv){
 setbuf(stdout,NULL);int disk=argc==2&&(!strcmp(argv[1],"--ext2")||!strcmp(argv[1],"--brook-ext2"));int brook=argc==2&&(!strcmp(argv[1],"--brook")||!strcmp(argv[1],"--brook-ext2"));CK(argc==1||brook||disk);CK(geteuid()!=0);uid_t uid=geteuid();gid_t gid=getegid();
 pid_t child=syscall(SYS_clone,(unsigned long)(CLONE_NEWUSER|CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(child>=0);
 if(child){waitok(child);printf("FILEBIND PARENT PASS checks=%u\n",checks);return 0;}
 alarm(60);map("/proc/self/uid_map",uid);int f=open("/proc/self/setgroups",O_WRONLY);CK(f>=0);CK(write(f,"deny\n",5)==5);CK(close(f)==0);map("/proc/self/gid_map",gid);
 CK(mount(NULL,"/",NULL,MS_PRIVATE|MS_REC,NULL)==0);char base[64];strcpy(base,brook&&disk?"/data/home/filebind-XXXXXX":"/tmp/filebind-XXXXXX");CK(mkdtemp(base));if(!disk)CK(mount("tmpfs",base,"tmpfs",MS_NOSUID|MS_NODEV,NULL)==0);struct statfs fs;CK(statfs(base,&fs)==0&&fs.f_type==(disk?0xef53:0x01021994));CK(chdir(base)==0);
 CK(mkdir("tree",0700)==0&&mkdir("copy",0700)==0);value("source",'A');value("other",'C');value("tree/target",'B');
 // Bubblewrap's safe_openat fallback reads back the O_PATH descriptor name
 // before mounting. Cover an ordinary unbound file as well as bind targets.
 fdpath("source",O_RDONLY);fdpath("source",O_PATH|O_CLOEXEC);
 // Bubblewrap mounts the exact opened objects via proc magic links.
 int sourcefd=open("source",O_PATH|O_CLOEXEC),targetfd=open("tree/target",O_PATH|O_CLOEXEC);CK(sourcefd>=0&&targetfd>=0);
 char sourcepath[64],targetpath[64];snprintf(sourcepath,sizeof(sourcepath),"/proc/self/fd/%d",sourcefd);snprintf(targetpath,sizeof(targetpath),"/proc/self/fd/%d",targetfd);
 CK(!rename("source","fd-renamed")&&!rename("tree/target","tree/fd-target"));
 CK(!mount("other","fd-renamed",NULL,MS_BIND,NULL));CK(readvalue("fd-renamed")=='C');
 CK(mount(sourcepath,targetpath,NULL,MS_BIND,NULL)==0);CK(readvalue("tree/fd-target")=='A');
 CK(!umount2("fd-renamed",MNT_DETACH));
 CK(!rename("fd-renamed","source")&&!rename("tree","fd-tree"));
 int otherfd=open("other",O_PATH|O_CLOEXEC);CK(otherfd>=0);char otherpath[64];snprintf(otherpath,sizeof(otherpath),"/proc/self/fd/%d",otherfd);
 CK(mount(otherpath,targetpath,NULL,MS_BIND,NULL)==0);CK(readvalue("fd-tree/fd-target")=='C');
 errno=0;CK(unlink("fd-tree/fd-target")==-1&&errno==EBUSY);
 CK(umount2("fd-tree/fd-target",MNT_DETACH)==0);CK(readvalue("fd-tree/fd-target")=='A');
 CK(umount2("fd-tree/fd-target",MNT_DETACH)==0);CK(readvalue("fd-tree/fd-target")=='B');
 CK(!rename("fd-tree","tree")&&!rename("tree/fd-target","tree/target"));CK(!close(sourcefd)&&!close(targetfd)&&!close(otherfd));
 errno=0;int linked=link("tree/target","tree/alias");
 if(brook)CK(linked==-1&&errno==EPERM);
 else {CK(linked==0);CK(mount("source","tree/target",NULL,MS_BIND,NULL)==0);CK(readvalue("tree/target")=='A'&&readvalue("tree/alias")=='B');CK(umount2("tree/target",MNT_DETACH)==0);CK(unlink("tree/alias")==0);CK(link("source","source-alias")==0);}
 errno=0;CK(mount("source","tree",NULL,MS_BIND,NULL)==-1&&errno==ENOTDIR);errno=0;CK(mount("tree","tree/target",NULL,MS_BIND,NULL)==-1&&errno==ENOTDIR);CK(readvalue("tree/target")=='B');
 int old=open("tree/target",O_RDONLY);CK(old>=0);CK(mount("source","tree/target",NULL,MS_BIND,NULL)==0);CK(readvalue("tree/target")=='A'&&readfd(old)=='B');fdpath("tree/target",O_RDONLY);fdpath("tree/target",O_PATH|O_CLOEXEC);
 errno=0;CK(unlink("tree/target")==-1&&errno==EBUSY);errno=0;CK(rename("other","tree/target")==-1&&errno==EBUSY);
 CK(rename("source","renamed")==0);value("source",'N');CK(readvalue("tree/target")=='A');if(!brook)CK(unlink("source-alias")==0);
 CK(mount("other","tree/target",NULL,MS_BIND|MS_REC,NULL)==0);CK(readvalue("tree/target")=='C');
 CK(rename("tree","tree-moved")==0);CK(readvalue("tree-moved/target")=='C');fdpath("tree-moved/target",O_PATH|O_CLOEXEC);CK(rename("tree-moved","tree")==0);
 CK(mount("tree","copy",NULL,MS_BIND|MS_REC,NULL)==0);CK(readvalue("copy/target")=='C');fdpath("copy/target",O_PATH|O_CLOEXEC);CK(umount2("copy/target",MNT_DETACH)==0);CK(readvalue("copy/target")=='A');CK(umount2("copy/target",MNT_DETACH)==0);CK(readvalue("copy/target")=='B');CK(umount2("copy",MNT_DETACH)==0);
 CK(umount2("tree/target",MNT_DETACH)==0);CK(readvalue("tree/target")=='A');
 sync();CK(mount(NULL,"tree/target",NULL,MS_BIND|MS_REMOUNT|MS_RDONLY|MS_NOSUID|MS_NODEV,NULL)==0);errno=0;CK(open("tree/target",O_WRONLY)==-1&&errno==EROFS);
 CK(mount("tree","copy",NULL,MS_BIND|MS_REC,NULL)==0);CK(readvalue("copy/target")=='A');errno=0;CK(open("copy/target",O_WRONLY)==-1&&errno==EROFS);
 child=syscall(SYS_clone,(unsigned long)(CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(child>=0);if(!child){CK(umount2("copy/target",MNT_DETACH)==0);CK(readvalue("copy/target")=='B');CK(readvalue("tree/target")=='A');_exit(0);}waitok(child);CK(readvalue("copy/target")=='A');
 CK(unlink("renamed")==0);CK(readvalue("tree/target")=='A'&&readvalue("copy/target")=='A');int held=open("copy/target",O_RDONLY);CK(held>=0);CK(umount2("tree/target",MNT_DETACH)==0);CK(readvalue("tree/target")=='B'&&readvalue("copy/target")=='A');CK(umount2("copy/target",MNT_DETACH)==0);CK(readvalue("copy/target")=='B'&&readfd(held)=='A');CK(umount2("copy",MNT_DETACH)==0);CK(readfd(held)=='A'&&readfd(old)=='B');CK(close(held)==0&&close(old)==0);
 pivot_source(base);
 if(disk){CK(unlink("source")==0&&unlink("other")==0&&unlink("tree/target")==0);CK(rmdir("tree")==0&&rmdir("copy")==0);}CK(chdir("/")==0);if(!disk)CK(umount2(base,MNT_DETACH)==0);CK(rmdir(base)==0);printf("FILEBIND FIXTURE PASS checks=%u brook_cases=%d\n",checks,brook);return 0;
}
