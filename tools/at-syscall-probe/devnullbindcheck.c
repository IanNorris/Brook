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
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
static unsigned checks;
#define CK(x) do{++checks;if(!(x)){fprintf(stderr,"DEVNULLBIND FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static void map(const char* path,unsigned id){char text[64];int n=snprintf(text,sizeof(text),"0 %u 1\n",id);int f=open(path,O_WRONLY);CK(f>=0);CK(write(f,text,n)==n);CK(close(f)==0);}
static void waitok(pid_t child){int status;CK(waitpid(child,&status,0)==child);CK(WIFEXITED(status)&&WEXITSTATUS(status)==0);}
static int nullfd(const char* path){int f=open(path,O_RDWR|O_CLOEXEC);CK(f>=0);CK((fcntl(f,F_GETFD)&FD_CLOEXEC)!=0);CK((fcntl(f,F_GETFL)&O_ACCMODE)==O_RDWR);char c='N';CK(read(f,&c,1)==0);CK(write(f,&c,1)==1);return f;}
int main(int argc,char** argv){
 setbuf(stdout,NULL);int brook=argc==2&&!strcmp(argv[1],"--brook");int linuxRoot=argc==2&&!strcmp(argv[1],"--linux-root");CK(argc==1||brook||linuxRoot);CK(linuxRoot?geteuid()==0:geteuid()!=0);uid_t uid=geteuid();gid_t gid=getegid();char base[]="/tmp/devnullbind-XXXXXX";CK(mkdtemp(base));char saved[128],fake[128],root[128],fakeNull[160],shm[128];snprintf(saved,sizeof(saved),"%s/saved",base);snprintf(fake,sizeof(fake),"%s/fake",base);snprintf(root,sizeof(root),"%s/root",base);snprintf(fakeNull,sizeof(fakeNull),"%s/null",fake);snprintf(shm,sizeof(shm),"/dev/shm/brook-null-%s",strrchr(base,'/')+1);CK(mkdir(saved,0700)==0&&mkdir(fake,0700)==0&&mkdir(root,0700)==0);int f=open(fakeNull,O_CREAT|O_EXCL|O_RDWR,0600);CK(f>=0);CK(write(f,"B",1)==1);CK(close(f)==0);f=open(shm,O_CREAT|O_EXCL|O_RDWR,0600);CK(f>=0);CK(write(f,"S",1)==1);CK(close(f)==0);
 pid_t child=syscall(SYS_clone,(unsigned long)((linuxRoot?0:CLONE_NEWUSER)|CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(child>=0);if(child){waitok(child);CK(unlink(shm)==0);CK(unlink(fakeNull)==0&&rmdir(saved)==0&&rmdir(fake)==0&&rmdir(root)==0&&rmdir(base)==0);printf("DEVNULLBIND PARENT PASS checks=%u\n",checks);return 0;}
 alarm(60);if(!linuxRoot){map("/proc/self/uid_map",uid);f=open("/proc/self/setgroups",O_WRONLY);CK(f>=0);CK(write(f,"deny\n",5)==5);CK(close(f)==0);map("/proc/self/gid_map",gid);}CK(mount(NULL,"/",NULL,MS_PRIVATE|MS_REC,NULL)==0);CK(mount("/dev",saved,NULL,MS_BIND|(brook?0:MS_REC),NULL)==0);CK(mount(saved,"/dev",NULL,MS_BIND|(brook?0:MS_REC),NULL)==0);struct statvfs fs;CK(statvfs("/dev",&fs)==0);unsigned long flags=(fs.f_flag&15)|MS_NOSUID;CK(mount(NULL,"/dev",NULL,MS_BIND|MS_REMOUNT|flags,NULL)==0);f=nullfd("/dev/null");CK(chdir("/dev")==0);int relative=nullfd("null");CK(close(relative)==0);CK(chdir("/")==0);if(brook){errno=0;CK(open(shm,O_RDONLY)==-1&&errno==ENOENT);}
 char alias[160];snprintf(alias,sizeof(alias),"%s/null",base);CK(symlink("/dev/null",alias)==0);CK(chdir(base)==0);errno=0;CK(open("null",O_RDWR|O_NOFOLLOW)==-1&&errno==ELOOP);errno=0;CK(open("null",O_RDWR|O_CREAT|O_EXCL,0600)==-1&&errno==EEXIST);relative=nullfd("null");CK(close(relative)==0);CK(unlink("null")==0);CK(chdir("/")==0);
 child=syscall(SYS_clone,(unsigned long)(CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(child>=0);if(!child){int n=nullfd("/dev/null");CK(close(n)==0);CK(mount(fake,"/dev",NULL,MS_BIND,NULL)==0);n=open("/dev/null",O_RDONLY);CK(n>=0);char c=0;CK(read(n,&c,1)==1&&c=='B');CK(close(n)==0);_exit(0);}waitok(child);int again=nullfd("/dev/null");CK(close(again)==0);
 CK(mount(NULL,"/dev",NULL,MS_BIND|MS_REMOUNT|flags|MS_NODEV,NULL)==0);errno=0;CK(open("/dev/null",O_RDWR)==-1&&errno==EACCES);char c='H';CK(write(f,&c,1)==1&&read(f,&c,1)==0);CK(close(f)==0);
 CK(mount("tmpfs",root,"tmpfs",6,"mode=0755")==0);char dev[160],old[160];snprintf(dev,sizeof(dev),"%s/dev",root);snprintf(old,sizeof(old),"%s/old",root);CK(mkdir(dev,0755)==0&&mkdir(old,0755)==0);CK(mount(saved,dev,NULL,MS_BIND|(brook?0:MS_REC),NULL)==0);CK(mount(NULL,dev,NULL,MS_BIND|MS_REMOUNT|flags,NULL)==0);CK(chdir(root)==0);CK(syscall(SYS_pivot_root,".","old")==0);CK(chdir("/")==0);CK(umount2("/old",MNT_DETACH)==0);f=nullfd("/dev/null");CK(close(f)==0);if(brook){errno=0;CK(open(shm,O_RDONLY)==-1&&errno==ENOENT);}printf("DEVNULLBIND FIXTURE PASS checks=%u brook_cases=%d\n",checks,brook);return 0;
}
