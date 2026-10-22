// Linux reference contract for the forthcoming tmpfs backend. This probe is not
// claimed to pass on Brook until tmpfs is implemented; mount-instance plumbing
// is exercised separately by the actual retained-VFS hosted fixture.
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
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
int main(int argc,char** argv) {
 setbuf(stdout,NULL);CK(argc==2&&!strcmp(argv[1],"--disposable-vm"));CK(geteuid()!=0);
 uid_t uid=geteuid();gid_t gid=getegid();
 pid_t child=syscall(SYS_clone,(unsigned long)(CLONE_NEWUSER|CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(child>=0);
 if(child) {int s;CK(waitpid(child,&s,0)==child);CK(WIFEXITED(s)&&!WEXITSTATUS(s));printf("TMPFS INSTANCE PARENT PASS checks=%u\n",checks);return 0;}
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
 CK(!mkdirat(da,"dir",0755));CK(!symlinkat("value",da,"link"));char text[8]={0};CK(readlinkat(da,"link",text,sizeof(text))==5&&!memcmp(text,"value",5));
 CK(!renameat(da,"value",da,"renamed"));CK(!unlinkat(da,"renamed",0));CK(!fstat(f,&sa)&&sa.st_nlink==0);CK(pread(f,text,3,0)==3&&!memcmp(text,"one",3));
 CK(!ftruncate(f,1ULL<<33));CK(pread(f,text,1,(1ULL<<33)-1)==1&&text[0]==0);CK(pwrite(f,"Z",1,(1ULL<<33)-1)==1);CK(!ftruncate(f,3));CK(!close(f));
 CK(!unlinkat(da,"link",0)&&!unlinkat(da,"dir",AT_REMOVEDIR));
 CK(!umount2(a,MNT_DETACH));CK(!fstat(da,&sa)&&sa.st_dev!=sb.st_dev);CK(!close(da));CK(!close(covered));
 CK(!umount2(b,MNT_DETACH));CK(!close(db));CK(!rmdir(a)&&!rmdir(b)&&!rmdir(base));
 snprintf(file,sizeof(file),"%u",checks);printf("TMPFS INSTANCE FIXTURE PASS checks=%s\n",file);_exit(0);
}
