#define _GNU_SOURCE
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sched.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* Run only in a disposable VM. This discovery probe creates real mounts.
 * Observations explicitly marked DISCOVERY are not conformance assertions. */
static int count;
static void ck(int ok,const char *msg) {
    if(!ok) { fprintf(stderr,"FAIL %s errno=%d\n",msg,errno);exit(1); }
    ++count;
}
static void joinpath(char *out,const char *base,const char *tail) {
    ck(snprintf(out,PATH_MAX,"%s/%s",base,tail)<PATH_MAX,"fixture path length");
}
static void put(const char *name,char c) {
    int fd=open(name,O_WRONLY|O_CREAT|O_EXCL,0600);
    ck(fd>=0 && write(fd,&c,1)==1,"write fixture");close(fd);
}
static int value(int dir,const char *name) {
    int fd=openat(dir,name,O_RDONLY);if(fd<0)return -errno;
    char c=0;int n=read(fd,&c,1);close(fd);return n==1?(unsigned char)c:-EIO;
}
static void sendbyte(int fd) { ck(write(fd,"x",1)==1,"pipe send"); }
static void recvbyte(int fd) { char c;ck(read(fd,&c,1)==1,"pipe receive"); }
static int same(int a,int b) { struct stat x,y;return !fstat(a,&x)&&!fstat(b,&y)&&x.st_dev==y.st_dev&&x.st_ino==y.st_ino; }
int main(int argc,char **argv) {
    ck((argc==2 || (argc==3 && !strcmp(argv[2],"--linux-discovery"))) && !strcmp(argv[1],"--disposable-vm"),"explicit disposable VM fixture");
    ck(geteuid()==0,"privileged fixture");
    setbuf(stdout,NULL);
    char base[]="/tmp/brook-mountns-XXXXXX";ck(mkdtemp(base)!=NULL,"fixture directory");
    char a[PATH_MAX],b[PATH_MAX],target[PATH_MAX],name[PATH_MAX];
    joinpath(a,base,"a");joinpath(b,base,"b");joinpath(target,base,"target");
    ck(!mkdir(a,0700)&&!mkdir(b,0700)&&!mkdir(target,0700),"fixture directories");
    joinpath(name,a,"value");put(name,'A');joinpath(name,b,"value");put(name,'B');joinpath(name,target,"value");put(name,'T');
    joinpath(name,a,"sub");ck(!mkdir(name,0700),"nested mountpoint");
    int parentfd=open(base,O_RDONLY|O_DIRECTORY),oldfd=open(target,O_RDONLY|O_DIRECTORY);
    ck(parentfd>=0&&oldfd>=0,"pre-clone directory capabilities");
    int ready[2],go[2];ck(!pipe(ready)&&!pipe(go),"synchronization pipes");
    pid_t child=(pid_t)syscall(SYS_clone,(unsigned long)(CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);
    ck(child>=0,"clone NEWNS");
    if(!child) {
        close(ready[0]);close(go[1]);
        ck(!fchdir(oldfd),"child cwd before overmount");
        ck(!mount(a,target,NULL,MS_BIND,NULL),"child directory bind");
        ck(value(AT_FDCWD,"value")=='T',"old cwd retains covered directory");
        ck(value(oldfd,"value")=='T',"old dirfd retains covered directory");
        joinpath(name,target,"value");ck(value(AT_FDCWD,name)=='A',"child absolute lookup enters private bind");
        int bound=open(target,O_RDONLY|O_DIRECTORY),up=openat(bound,"..",O_RDONLY|O_DIRECTORY);
        ck(bound>=0&&up>=0&&same(up,parentfd),"bind root dotdot reaches covered parent");close(up);
        sendbyte(ready[1]);recvbyte(go[0]);
        ck(value(AT_FDCWD,name)=='A',"parent overmount does not change child absolute lookup");
        int foreign=value(parentfd,"target/value");
        printf("DISCOVERY foreign-parent-dirfd-after-parent-bind=%d (%c)\n",foreign,foreign>0?foreign:'?');
#ifdef MOUNTNS_ACCEPTANCE
        ck(foreign=='B',"foreign parent dirfd follows original namespace's bind");
#endif
        ck(value(oldfd,"value")=='T',"old target dirfd remains exact after both binds");
        joinpath(name,target,"sub");ck(!mount(b,name,NULL,MS_BIND,NULL),"nested child bind");
        ck(value(bound,"sub/value")=='B',"nested mount visible through held bind root");
        int nested=openat(bound,"sub",O_RDONLY|O_DIRECTORY);ck(nested>=0,"hold nested bind root");
        ck(!umount2(target,MNT_DETACH),"child lazy detach");
        joinpath(name,target,"value");
        ck(value(AT_FDCWD,name)=='T',"fresh child absolute lookup returns covered tree");
        ck(value(bound,"value")=='A',"detached root dirfd retains bound backing");
        int detached_nested=value(bound,"sub/value");
        printf("DISCOVERY detached-root-dirfd-nested-mount-value=%d\n",detached_nested);
#ifdef MOUNTNS_ACCEPTANCE
        ck(detached_nested==-ENOENT,"detached root no longer traverses former nested mount");
#endif
        ck(value(nested,"value")=='B',"exact nested bind handle survives recursive detach");
        close(nested);
        up=openat(bound,"..",O_RDONLY|O_DIRECTORY);ck(up>=0,"detached root dotdot opens");
        printf("DISCOVERY detached-dotdot-is-root=%d detached-dotdot-is-former-parent=%d\n",same(up,bound),same(up,parentfd));
#ifdef MOUNTNS_ACCEPTANCE
        ck(same(up,bound)&&!same(up,parentfd),"detached root dotdot identity clamps");
#endif
        close(up);close(bound);close(oldfd);close(parentfd);
        printf("MOUNTNS CHILD PASS checks=%d\n",count);_exit(0);
    }
    close(ready[1]);close(go[0]);recvbyte(ready[0]);
    joinpath(name,target,"value");ck(value(AT_FDCWD,name)=='T',"child bind invisible to parent");
    ck(!mount(b,target,NULL,MS_BIND,NULL),"parent independent bind");
    ck(value(AT_FDCWD,name)=='B',"parent sees own bind");sendbyte(go[1]);
    int status;ck(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status),"child completed");
    ck(value(AT_FDCWD,name)=='B',"child detach leaves parent bind");
    ck(!umount2(target,MNT_DETACH),"parent detach");ck(value(AT_FDCWD,name)=='T',"parent underlying tree restored");
    if(argc==3) {
        /* Linux-only ownership discovery. Brook's initial checkpoint does not
         * implement unshare; this is not part of its conformance subset. */
        int ready2[2],go2[2];ck(!pipe(ready2)&&!pipe(go2),"owner-lifetime pipes");
        pid_t owner=(pid_t)syscall(SYS_clone,(unsigned long)(CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);
        ck(owner>=0,"owner-lifetime namespace clone");
        if(!owner) {
            ck(!mount(a,target,NULL,MS_BIND,NULL),"owner-lifetime bind");
            int original=open(base,O_RDONLY|O_DIRECTORY);ck(original>=0,"hold original namespace dirfd");
            pid_t follower=(pid_t)syscall(SYS_clone,(unsigned long)(CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);
            ck(follower>=0,"follower namespace clone");
            if(!follower) {
                sendbyte(ready2[1]);recvbyte(go2[0]);
                printf("DISCOVERY foreign-dirfd-after-last-namespace-process-left=%d\n",value(original,"target/value"));
                joinpath(name,target,"value");
                ck(value(AT_FDCWD,name)=='A',"follower absolute lookup retains its copied mount");
                close(original);_exit(0);
            }
            recvbyte(ready2[0]);close(original);
            ck(!unshare(CLONE_NEWNS),"last process leaves original namespace (Linux discovery only)");
            sendbyte(go2[1]);int child_status;
            ck(waitpid(follower,&child_status,0)==follower&&WIFEXITED(child_status)&&!WEXITSTATUS(child_status),"follower completion");
            ck(!umount2(target,MNT_DETACH),"owner copied namespace cleanup");_exit(0);
        }
        int owner_status;
        ck(waitpid(owner,&owner_status,0)==owner&&WIFEXITED(owner_status)&&!WEXITSTATUS(owner_status),"owner-lifetime completion");
        close(ready2[0]);close(ready2[1]);close(go2[0]);close(go2[1]);
        ck(value(AT_FDCWD,name)=='T',"owner-lifetime test did not mutate parent topology");
    }
    close(oldfd);close(parentfd);close(ready[0]);close(go[1]);
    joinpath(name,a,"value");ck(!unlink(name),"remove a fixture");joinpath(name,b,"value");ck(!unlink(name),"remove b fixture");joinpath(name,target,"value");ck(!unlink(name),"remove target fixture");
    joinpath(name,a,"sub");ck(!rmdir(name),"remove nested mountpoint");
    ck(!rmdir(a)&&!rmdir(b)&&!rmdir(target)&&!rmdir(base),"fixture cleanup");
    printf("MOUNTNS PARENT PASS checks=%d; discovery observations require review\n",count);
    return 0;
}
