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
#define CK(x) do{++checks;if(!(x)){fprintf(stderr,"PATHSTATFS FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static void map(const char* path,unsigned id){char text[64];int n=snprintf(text,sizeof(text),"0 %u 1\n",id);int f=open(path,O_WRONLY);CK(f>=0);CK(write(f,text,n)==n);CK(close(f)==0);}
static void same(const struct statfs* a,const struct statfs* b){CK(a->f_type==b->f_type&&a->f_bsize==b->f_bsize&&a->f_frsize==b->f_frsize&&a->f_namelen==b->f_namelen&&!memcmp(&a->f_fsid,&b->f_fsid,sizeof(a->f_fsid)));}
int main(int argc,char** argv){
 setbuf(stdout,NULL);int brook=argc==2&&!strcmp(argv[1],"--brook");CK(argc==1||brook);CK(geteuid()!=0);uid_t uid=geteuid();gid_t gid=getegid();pid_t child=syscall(SYS_clone,(unsigned long)(CLONE_NEWUSER|CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(child>=0);if(child){int status;CK(waitpid(child,&status,0)==child);CK(WIFEXITED(status)&&WEXITSTATUS(status)==0);printf("PATHSTATFS PARENT PASS checks=%u\n",checks);return 0;}
 alarm(60);map("/proc/self/uid_map",uid);int f=open("/proc/self/setgroups",O_WRONLY);CK(f>=0);CK(write(f,"deny\n",5)==5);CK(close(f)==0);map("/proc/self/gid_map",gid);CK(mount(NULL,"/",NULL,MS_PRIVATE|MS_REC,NULL)==0);
 char base[]="/tmp/pathstatfs-XXXXXX";CK(mkdtemp(base));CK(chdir(base)==0);CK(mkdir("dir",0700)==0);f=open("file",O_CREAT|O_EXCL|O_RDWR,0600);CK(f>=0);CK(close(f)==0);CK(symlink("file","link")==0);
 struct statfs expected,actual;CK(statfs(".",&expected)==0);const char* paths[]={"dir","file","link"};int kept=-1;
 for(unsigned i=0;i<3;++i){f=open(paths[i],O_PATH|O_NOFOLLOW|O_CLOEXEC);CK(f>=0);CK(fstatfs(f,&actual)==0);same(&expected,&actual);if(i==1)kept=f;else CK(close(f)==0);}
 CK(rename("file","moved")==0);CK(unlink("moved")==0);CK(fstatfs(kept,&actual)==0);same(&expected,&actual);CK(close(kept)==0);errno=0;CK(fstatfs(kept,&actual)==-1&&errno==EBADF);
 int covered=open("dir",O_PATH|O_DIRECTORY);CK(covered>=0);CK(mount("tmpfs","dir","tmpfs",6,"mode=0755")==0);CK(statfs("dir",&actual)==0&&actual.f_type==0x01021994);CK(fstatfs(covered,&actual)==0);same(&expected,&actual);
 int memory=open("dir",O_PATH|O_DIRECTORY);CK(memory>=0);CK(fstatfs(memory,&actual)==0&&actual.f_type==0x01021994&&(actual.f_flags&6)==6);CK(mount(NULL,"dir",NULL,MS_BIND|MS_REMOUNT|MS_RDONLY|MS_NOSUID|MS_NODEV,NULL)==0);CK(fstatfs(memory,&actual)==0&&(actual.f_flags&7)==7);CK(umount2("dir",MNT_DETACH)==0);CK(fstatfs(memory,&actual)==0&&actual.f_type==0x01021994&&(actual.f_flags&7)==7);CK(close(memory)==0);CK(fstatfs(covered,&actual)==0);same(&expected,&actual);CK(close(covered)==0);
 errno=0;CK(fstatfs(-1,&actual)==-1&&errno==EBADF);f=open("dir",O_PATH);CK(f>=0);errno=0;CK(syscall(SYS_fstatfs,f,(void*)1)==-1&&errno==EFAULT);CK(close(f)==0);CK(unlink("link")==0&&rmdir("dir")==0);CK(chdir("/")==0&&rmdir(base)==0);printf("PATHSTATFS FIXTURE PASS checks=%u brook_cases=%d\n",checks,brook);return 0;
}
