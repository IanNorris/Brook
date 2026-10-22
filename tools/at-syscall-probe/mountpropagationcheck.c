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
#define CK(x) do { ++checks;if(!(x)){fprintf(stderr,"PROPAGATION FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);} } while(0)
static void map(const char* p,unsigned id) {char t[64];int n=snprintf(t,sizeof(t),"0 %u 1\n",id);int f=open(p,O_WRONLY);CK(f>=0);CK(write(f,t,n)==n);CK(!close(f));}
static void waitok(pid_t p) {int s;CK(waitpid(p,&s,0)==p);CK(WIFEXITED(s)&&!WEXITSTATUS(s));}
static void byte(int fd,int send) {char c='x';CK((send?write(fd,&c,1):read(fd,&c,1))==1);}
static void put(const char* p,char c) {int f=open(p,O_CREAT|O_WRONLY,0600);CK(f>=0);CK(write(f,&c,1)==1);CK(!close(f));}
static char value(const char* dir) {char p[256],c=0;CK(snprintf(p,sizeof(p),"%s/value",dir)<(int)sizeof(p));int f=open(p,O_RDONLY);CK(f>=0);CK(read(f,&c,1)==1);CK(!close(f));return c;}
static void conversions(const char* target) {
 unsigned long types[]={MS_PRIVATE,MS_SLAVE};
 for(unsigned i=0;i<2;++i)for(unsigned rec=0;rec<2;++rec)for(unsigned silent=0;silent<2;++silent) {
  unsigned long flags=types[i]|(rec?MS_REC:0)|(silent?MS_SILENT:0);
  CK(!mount(NULL,target,NULL,flags,NULL));
  CK(!mount("/does-not-exist",target,NULL,flags,NULL));
 }
}
int main(int argc,char** argv) {
 setbuf(stdout,NULL);CK(argc==2&&!strcmp(argv[1],"--disposable-vm"));CK(geteuid()!=0);
 errno=0;CK(mount(NULL,"/",NULL,MS_SLAVE|MS_REC|MS_SILENT,NULL)==-1&&errno==EPERM);
 uid_t uid=geteuid();gid_t gid=getegid();
 pid_t child=syscall(SYS_clone,(unsigned long)(CLONE_NEWUSER|CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(child>=0);
 if(child) {waitok(child);printf("PROPAGATION PARENT PASS checks=%u\n",checks);return 0;}
 map("/proc/self/uid_map",uid);int f=open("/proc/self/setgroups",O_WRONLY);CK(f>=0);CK(write(f,"deny\n",5)==5);CK(!close(f));map("/proc/self/gid_map",gid);
 // Linux may inherit slave relationships from a shared host tree. Establish the
 // private starting invariant that Brook already guarantees, then test it.
 CK(!mount(NULL,"/",NULL,MS_PRIVATE|MS_REC,NULL));conversions("/");
 char base[]="/tmp/propagation-XXXXXX";CK(mkdtemp(base));
 char a[256],b[256],target[256],sub[256],p[280];
 CK(snprintf(a,sizeof(a),"%s/a",base)<(int)sizeof(a));CK(snprintf(b,sizeof(b),"%s/b",base)<(int)sizeof(b));
 CK(snprintf(target,sizeof(target),"%s/t",base)<(int)sizeof(target));CK(snprintf(sub,sizeof(sub),"%s/a/sub",base)<(int)sizeof(sub));
 CK(!mkdir(a,0700)&&!mkdir(b,0700)&&!mkdir(target,0700)&&!mkdir(sub,0700));
 snprintf(p,sizeof(p),"%s/value",a);put(p,'A');snprintf(p,sizeof(p),"%s/value",b);put(p,'B');snprintf(p,sizeof(p),"%s/value",target);put(p,'T');
 errno=0;CK(mount(NULL,target,NULL,MS_PRIVATE,NULL)==-1&&errno==EINVAL);
 CK(!mount(a,target,NULL,MS_BIND,NULL));conversions(target);
 char nested[280];snprintf(nested,sizeof(nested),"%s/sub",target);CK(!mount(b,nested,NULL,MS_BIND,NULL));conversions(nested);conversions(target);CK(value(nested)=='B');
 unsigned long invalid[]={MS_PRIVATE|MS_SLAVE,MS_PRIVATE|MS_SHARED,MS_PRIVATE|MS_UNBINDABLE,MS_PRIVATE|MS_RDONLY,MS_SLAVE|MS_NOSUID};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {errno=0;CK(mount(NULL,target,NULL,invalid[i],NULL)==-1&&errno==EINVAL);}
 errno=0;CK(mount(NULL,"/missing-propagation-path",NULL,MS_PRIVATE,NULL)==-1&&errno==ENOENT);
 CK(value(target)=='A'&&value(nested)=='B');
 // Less privileged namespace inherits locked attachments. Converting them
 // must not unlock detach, and fresh mounts must remain private both ways.
 int ready[2],go[2];CK(!pipe(ready)&&!pipe(go));
 pid_t sibling=syscall(SYS_clone,(unsigned long)(CLONE_NEWUSER|CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(sibling>=0);
 if(!sibling) {
  CK(!close(ready[0])&&!close(go[1]));conversions(target);conversions(nested);
  errno=0;CK(umount2(target,MNT_DETACH)==-1&&errno==EINVAL);
  CK(!mount(b,target,NULL,MS_BIND,NULL));CK(value(target)=='B');byte(ready[1],1);byte(go[0],0);
  CK(value(target)=='B');CK(!umount2(target,MNT_DETACH));CK(value(target)=='A');
  CK(!close(ready[1])&&!close(go[0]));printf("PROPAGATION LOCKED CHILD PASS checks=%u\n",checks);_exit(0);
 }
 CK(!close(ready[1])&&!close(go[0]));byte(ready[0],0);CK(value(target)=='A');
 CK(!mount(b,target,NULL,MS_BIND,NULL));byte(go[1],1);waitok(sibling);CK(value(target)=='B');
 CK(!close(ready[0])&&!close(go[1]));CK(!umount2(target,MNT_DETACH));CK(value(target)=='A');
 CK(!umount2(nested,MNT_DETACH));CK(!umount2(target,MNT_DETACH));CK(value(target)=='T');
 CK(!rmdir(sub));snprintf(p,sizeof(p),"%s/value",a);CK(!unlink(p));snprintf(p,sizeof(p),"%s/value",b);CK(!unlink(p));snprintf(p,sizeof(p),"%s/value",target);CK(!unlink(p));
 CK(!rmdir(a)&&!rmdir(b)&&!rmdir(target)&&!rmdir(base));
 printf("PROPAGATION PRIVATE FIXTURE PASS checks=%u\n",checks);_exit(0);
}
