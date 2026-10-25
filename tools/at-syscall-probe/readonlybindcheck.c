// Run only in a disposable VM. One binary exercises Linux and Brook.
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sched.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
static int brook;
static int tmpfs_only;
static unsigned long restrictions=MS_NOSUID|MS_NODEV;
#define CK(x) do {++checks;if(!(x)){fprintf(stderr,"ROBIND FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
#define RO(x) do {errno=0;CK((x)==-1&&errno==EROFS);}while(0)
static void mapid(const char* path,unsigned id){char text[64];int n=snprintf(text,sizeof(text),"0 %u 1\n",id);int f=open(path,O_WRONLY);CK(f>=0&&write(f,text,n)==n&&!close(f));}
static void waitok(pid_t child){int status;CK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));}
static void filename(char* out,size_t capacity,const char* dir,const char* leaf){CK(snprintf(out,capacity,"%s/%s",dir,leaf)>0);}
static void makevalue(const char* path){int f=open(path,O_CREAT|O_EXCL|O_RDWR,0600);CK(f>=0&&!ftruncate(f,4096)&&write(f,"A",1)==1&&!close(f));}
static int readonly(const char* path){return mount(NULL,path,NULL,MS_BIND|MS_REMOUNT|MS_RDONLY|restrictions,NULL);}
static void exercise(const char* source,const char* destination,int mapping){
 restrictions=MS_NOSUID|(mapping?0:MS_NODEV);
 CK(!mkdir(destination,0700)&&!mount(source,destination,NULL,MS_BIND,NULL));
 char value[512],newfile[512],newdir[512],moved[512],sourcevalue[512];
 filename(value,sizeof(value),destination,"value");filename(sourcevalue,sizeof(sourcevalue),source,"value");
 filename(newfile,sizeof(newfile),destination,"new");filename(newdir,sizeof(newdir),destination,"dir");filename(moved,sizeof(moved),destination,"moved");
 CK(!mkdir(newdir,0700));
 int directory=open(destination,O_RDONLY|O_DIRECTORY),metadata=open(value,O_PATH);CK(directory>=0&&metadata>=0);
 int reader=open(value,O_RDONLY);CK(reader>=0);
 if(brook){errno=0;CK(readonly(destination)==-1&&errno==EBUSY);}
 CK(!close(reader));
 if(mapping){
  int writer=open(value,O_RDWR);CK(writer>=0);char* p=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,writer,0);CK(p!=MAP_FAILED);p[0]='A';
  if(brook){errno=0;CK(readonly(destination)==-1&&errno==EBUSY);}
  CK(!close(writer)&&!munmap(p,4096));
  if(brook){int status=readonly(destination);if(status){CK(errno==EBUSY);puts("ROBIND pending writeback retained admission");sync();}else puts("ROBIND writeback retired before remount");}
 }
 CK(!readonly(destination));
 // Existence is checked before read-only admission for mkdir, as required
 // by mkdir-p callers preparing already-populated container bind parents.
 errno=0;CK(mkdir(newdir,0700)==-1&&errno==EEXIST);
 errno=0;CK(mkdir(value,0700)==-1&&errno==EEXIST);
 errno=0;CK(mkdir(destination,0700)==-1&&errno==EEXIST);
 struct statvfs space;CK(!statvfs(destination,&space)&&(space.f_flag&ST_RDONLY));
 int f=open(value,O_RDONLY|O_APPEND);CK(f>=0);char c=0;CK(read(f,&c,1)==1&&c=='A');
 RO(fchmod(f,0600));RO(fchown(f,0,0));
 struct timespec times[2]={{1,0},{2,0}};RO(futimens(f,times));CK(!close(f));
 f=open(value,O_RDONLY|O_CREAT,0600);CK(f>=0&&!close(f));
 RO(open(value,O_WRONLY));RO(open(value,O_RDWR));RO(open(value,O_RDONLY|O_TRUNC));RO(open(newfile,O_CREAT|O_RDONLY,0600));
 RO(unlink(value));RO(rmdir(newdir));RO(mkdir(moved,0700));RO(rename(value,moved));RO(symlink("value",newfile));
 RO(chmod(value,0644));RO(chown(value,0,0));RO(utimensat(AT_FDCWD,value,times,0));RO(fchmod(directory,0700));RO(futimens(directory,times));RO(openat(directory,"new",O_CREAT|O_WRONLY,0600));RO(access(value,W_OK));
 if(mapping){
  f=open(value,O_RDONLY);CK(f>=0);errno=0;CK(mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,f,0)==MAP_FAILED&&errno==EACCES);
  char* p=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE,f,0);CK(p!=MAP_FAILED);p[0]='P';CK(!munmap(p,4096)&&!close(f));
 }
 // Per-attachment protection: the independent source alias stays writable.
 f=open(sourcevalue,O_WRONLY);CK(f>=0&&write(f,"B",1)==1&&!close(f));f=open(value,O_RDONLY);CK(f>=0&&read(f,&c,1)==1&&c=='B'&&!close(f));
 if(brook){errno=0;CK(mount(NULL,destination,NULL,MS_BIND|MS_REMOUNT|restrictions,NULL)==-1&&errno==EPERM);}
 pid_t child=syscall(SYS_clone,(unsigned long)(CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(child>=0);
 if(!child){RO(open(newfile,O_CREAT|O_WRONLY,0600));_exit(0);}waitok(child);
 CK(!umount2(destination,MNT_DETACH));RO(openat(directory,"new",O_CREAT|O_WRONLY,0600));CK(!close(directory)&&!close(metadata));CK(!rmdir(destination));char sourceDir[512];filename(sourceDir,sizeof(sourceDir),source,"dir");CK(!rmdir(sourceDir));
 printf("ROBIND BACKEND PASS mapping=%d checks=%u\n",mapping,checks);
}
int main(int argc,char** argv){
 setbuf(stdout,NULL);alarm(60);brook=argc==4&&!strcmp(argv[3],"--brook");tmpfs_only=argc==4&&!strcmp(argv[3],"--tmpfs-only");CK((argc==3||brook||tmpfs_only)&&!strcmp(argv[1],"--disposable-vm")&&geteuid()!=0);
 char base[]="/tmp/robind-XXXXXX",disk[512],diskvalue[512];CK(mkdtemp(base));CK(snprintf(disk,sizeof(disk),"%s/robind-XXXXXX",argv[2])>0&&mkdtemp(disk));filename(diskvalue,sizeof(diskvalue),disk,"value");makevalue(diskvalue);
 uid_t uid=geteuid();gid_t gid=getegid();pid_t child=syscall(SYS_clone,(unsigned long)(CLONE_NEWUSER|CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(child>=0);
 if(child){waitok(child);CK(!unlink(diskvalue)&&!rmdir(disk)&&!rmdir(base));printf("ROBIND PARENT PASS checks=%u\n",checks);return 0;}
 mapid("/proc/self/uid_map",uid);int f=open("/proc/self/setgroups",O_WRONLY);CK(f>=0&&write(f,"deny\n",5)==5&&!close(f));mapid("/proc/self/gid_map",gid);
 CK(!mount(NULL,"/",NULL,MS_PRIVATE|MS_REC,NULL));
 if(brook){errno=0;CK(readonly("/")==-1&&errno==EOPNOTSUPP);}
 CK(!mount("tmpfs",base,"tmpfs",MS_NOSUID|MS_NODEV,NULL)&&!chdir(base));
 CK(!mkdir("source",0700));makevalue("source/value");exercise("source","target",0);if(!tmpfs_only)exercise(disk,"disk-target",1);
 CK(!chdir("/")&&!umount2(base,MNT_DETACH));printf("ROBIND FIXTURE PASS checks=%u brook_cases=%d\n",checks,brook);_exit(0);
}
