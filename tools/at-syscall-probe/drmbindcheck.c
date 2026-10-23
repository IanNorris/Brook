#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned checks;
#define CK(x) do { ++checks; if (!(x)) { fprintf(stderr,"DRMBIND FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1); } } while(0)
static const char *render="/dev/dri/renderD128";
static const char *vendor="/sys/dev/char/226:128/device/vendor";
static void version(int fd) { unsigned char bytes[64]={0};CK(ioctl(fd,0xc0406400UL,bytes)==0); }
static void map(const char* path,unsigned id) {
    char text[64];int n=snprintf(text,sizeof(text),"0 %u 1\n",id);
    int fd=open(path,O_WRONLY);CK(fd>=0);CK(write(fd,text,n)==n);CK(!close(fd));
}
static void waitok(pid_t child) { int status;CK(waitpid(child,&status,0)==child);CK(WIFEXITED(status)&&WEXITSTATUS(status)==0); }
static int probe(void) {
    CK(!access(render,F_OK));
    CK(!syscall(SYS_faccessat2,AT_FDCWD,render,F_OK,0));
    struct stat st;CK(!stat(render,&st)&&S_ISCHR(st.st_mode));
    CK(major(st.st_rdev)==226&&minor(st.st_rdev)==128);
    struct statx sx={0};CK(!syscall(SYS_statx,AT_FDCWD,render,0,STATX_BASIC_STATS,&sx));
    CK(S_ISCHR(sx.stx_mode)&&sx.stx_rdev_major==226&&sx.stx_rdev_minor==128);
    int fd=open(render,O_RDWR|O_CLOEXEC);CK(fd>=0);version(fd);
    memset(&sx,0,sizeof(sx));CK(!syscall(SYS_statx,fd,"",AT_EMPTY_PATH,STATX_BASIC_STATS,&sx));
    CK(sx.stx_rdev_major==226&&sx.stx_rdev_minor==128);
    int v=open(vendor,O_RDONLY);CK(v>=0);char data[64]={0};CK(read(v,data,sizeof(data)-1)>0);CK(!strncmp(data,"0x",2));CK(!close(v));
    char link[256];ssize_t count=readlink("/sys/dev/char/226:128/device/subsystem",link,sizeof(link)-1);
    CK(count>0);link[count]=0;CK(strstr(link,"/pci")!=NULL);
    return fd;
}
static void mkdirs(const char* path) {
    char text[512];CK(strlen(path)<sizeof(text));strcpy(text,path);
    for(char* p=text+1;*p;++p)if(*p=='/') { *p=0;CK(!mkdir(text,0700)||errno==EEXIST);*p='/'; }
    CK(!mkdir(text,0700)||errno==EEXIST);
}
int main(int argc,char** argv) {
    setbuf(stdout,NULL);CK(argc==2);
    int native=!strcmp(argv[1],"--native"),linuxRoot=!strcmp(argv[1],"--linux-root");
    CK(native||linuxRoot||!strcmp(argv[1],"--brook"));
    int initial=probe();CK(!close(initial));
    if(native) {printf("DRMBIND_NATIVE_PASS checks=%u\n",checks);return 0;}
    CK(linuxRoot?geteuid()==0:geteuid()!=0);
    uid_t uid=geteuid();gid_t gid=getegid();char base[]="/tmp/drmbind-XXXXXX";CK(mkdtemp(base)!=NULL);
    char savedDev[256],savedSys[256],savedDri[256],fakeDev[256],fakeDri[256],fakeRender[256],root[256];
    snprintf(savedDev,sizeof(savedDev),"%s/dev",base);snprintf(savedSys,sizeof(savedSys),"%s/sys",base);
    snprintf(fakeDev,sizeof(fakeDev),"%s/fake",base);snprintf(fakeDri,sizeof(fakeDri),"%s/fake/dri",base);
    snprintf(fakeRender,sizeof(fakeRender),"%s/fake/dri/renderD128",base);snprintf(root,sizeof(root),"%s/root",base);
    snprintf(savedDri,sizeof(savedDri),"%s/dri",base);
    CK(!mkdir(savedDev,0700)&&!mkdir(savedSys,0700)&&!mkdir(savedDri,0700)&&!mkdir(root,0700));mkdirs(fakeDri);
    int f=open(fakeRender,O_CREAT|O_EXCL|O_RDWR,0600);CK(f>=0);CK(write(f,"FAKE",4)==4);CK(!close(f));
    pid_t child=syscall(SYS_clone,(unsigned long)((linuxRoot?0:CLONE_NEWUSER)|CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(child>=0);
    if(child) {
        waitok(child);CK(!unlink(fakeRender));CK(!rmdir(fakeDri)&&!rmdir(fakeDev));
        CK(!rmdir(savedDev)&&!rmdir(savedSys)&&!rmdir(savedDri)&&!rmdir(root)&&!rmdir(base));
        printf("DRMBIND_PARENT_PASS checks=%u\n",checks);return 0;
    }
    alarm(90);
    if(!linuxRoot) {
        map("/proc/self/uid_map",uid);f=open("/proc/self/setgroups",O_WRONLY);CK(f>=0);CK(write(f,"deny\n",5)==5);CK(!close(f));map("/proc/self/gid_map",gid);
    }
    CK(!mount(NULL,"/",NULL,MS_PRIVATE|MS_REC,NULL));
    CK(!mount("/dev",savedDev,NULL,MS_BIND|MS_REC,NULL));CK(!mount("/sys",savedSys,NULL,MS_BIND|MS_REC,NULL));
    CK(!mount(savedDev,"/dev",NULL,MS_BIND|MS_REC,NULL));CK(!mount(savedSys,"/sys",NULL,MS_BIND|MS_REC,NULL));
    CK(!mount(NULL,"/dev",NULL,MS_BIND|MS_REMOUNT|MS_NOSUID,NULL));
    f=probe();
    child=syscall(SYS_clone,(unsigned long)(CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(child>=0);
    if(!child) {
        CK(!mount(fakeDev,"/dev",NULL,MS_BIND,NULL));struct stat st;CK(!stat(render,&st)&&S_ISREG(st.st_mode));
        int fd=open(render,O_RDONLY);CK(fd>=0);char text[4];CK(read(fd,text,4)==4&&!memcmp(text,"FAKE",4));CK(!close(fd));
        errno=0;CK(open("/dev/dsp",O_RDONLY)==-1&&errno==ENOENT);_exit(0);
    }
    waitok(child);version(f);
    child=syscall(SYS_clone,(unsigned long)(CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(child>=0);
    if(!child) {
        CK(!mount("/dev/dri",savedDri,NULL,MS_BIND,NULL));CK(!mount(savedDri,"/dev/dri",NULL,MS_BIND,NULL));
        int fd=probe();CK(!mount(NULL,"/dev/dri",NULL,MS_BIND|MS_REMOUNT|MS_NOSUID|MS_NODEV,NULL));
        errno=0;CK(open(render,O_RDWR)==-1&&errno==EACCES);version(fd);CK(!close(fd));_exit(0);
    }
    waitok(child);
    CK(!mount(NULL,"/dev",NULL,MS_BIND|MS_REMOUNT|MS_NOSUID|MS_NODEV,NULL));
    CK(!access(render,F_OK));errno=0;CK(open(render,O_RDWR)==-1&&errno==EACCES);version(f);CK(!close(f));
    CK(!mount("tmpfs",root,"tmpfs",MS_NODEV|MS_NOSUID,"mode=0755"));
    char newDev[300],newSys[300],old[300];snprintf(newDev,sizeof(newDev),"%s/dev",root);snprintf(newSys,sizeof(newSys),"%s/sys",root);snprintf(old,sizeof(old),"%s/old",root);
    CK(!mkdir(newDev,0755)&&!mkdir(newSys,0755)&&!mkdir(old,0755));
    CK(!mount(savedDev,newDev,NULL,MS_BIND|MS_REC,NULL));CK(!mount(savedSys,newSys,NULL,MS_BIND|MS_REC,NULL));
    CK(!chdir(root));CK(!syscall(SYS_pivot_root,".","old"));CK(!chdir("/"));CK(!umount2("/old",MNT_DETACH));
    f=probe();CK(!close(f));printf("DRMBIND_FIXTURE_PASS checks=%u\n",checks);return 0;
}
