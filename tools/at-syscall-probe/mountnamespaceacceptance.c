#define main mount_namespace_discovery_main
#define MOUNTNS_ACCEPTANCE 1
#include "mountnamespacecheck.c"
#undef main

static pid_t private_child(void) {
    pid_t pid=(pid_t)syscall(SYS_clone,(unsigned long)(CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);
    ck(pid>=0,"private namespace child");return pid;
}
static pid_t private_child3(void) {
    unsigned long long args[11]={0};args[0]=CLONE_NEWNS;args[4]=SIGCHLD;
    pid_t pid=(pid_t)syscall(SYS_clone3,args,sizeof(args));ck(pid>=0,"clone3 private namespace child");return pid;
}
static void wait_ok(pid_t pid,const char* label) {
    int status=0;ck(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&!WEXITSTATUS(status),label);
}
static int listed_mount(const char* target) {
    int fd=open("/proc/mounts",O_RDONLY);ck(fd>=0,"open namespace mounts");
    char text[32768],needle[PATH_MAX+4];size_t used=0;ssize_t n=0;
    while(used<sizeof(text)-1 && (n=read(fd,text+used,sizeof(text)-1-used))>0)used+=(size_t)n;
    ck(used<sizeof(text)-1&&n==0,"complete mount report reaches successful EOF");text[used]=0;ck(!close(fd),"close mount report");
    ck(snprintf(needle,sizeof(needle)," %s ",target)<(int)sizeof(needle),"mount report needle");
    return strstr(text,needle)!=NULL;
}
static void parallel_mutation(const char* source,const char* target,int expected) {
    char name[PATH_MAX];joinpath(name,target,"value");
    for(unsigned i=0;i<12;++i) {
        ck(!mount(source,target,NULL,MS_BIND,NULL),"parallel bind");
        ck(value(AT_FDCWD,name)==expected,"parallel namespace sees own source");
        ck(!umount2(target,MNT_DETACH),"parallel detach");
        ck(value(AT_FDCWD,name)=='T',"parallel covered tree restored");
    }
}
static void negative_privilege(void) {
    ck(geteuid()!=0,"unprivileged fixture identity");errno=0;
    long denied=syscall(SYS_clone,(unsigned long)(CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);
    if(!denied)_exit(92);
    ck(denied<0&&errno==EPERM,"unprivileged namespace denied");
    errno=0;ck(mount("/tmp","/tmp",NULL,MS_BIND,NULL)<0&&errno==EPERM,"unprivileged bind denied");
    errno=0;ck(umount2("/tmp",MNT_DETACH)<0&&errno==EPERM,"unprivileged detach denied");
}
int main(int argc,char** argv) {
    setbuf(stdout,NULL);
    if(argc==2&&!strcmp(argv[1],"--unprivileged")) {negative_privilege();printf("MOUNTNS UNPRIVILEGED PASS checks=%d\n",count);return 0;}
    if(argc==4 && !strcmp(argv[1],"--exec-preserve")) {
        ck(geteuid()==0,"exec credentials unchanged");
        char name[PATH_MAX];joinpath(name,argv[2],"value");
        ck(value(AT_FDCWD,name)=='A',"exec preserves private bind");
        ck(value(AT_FDCWD,"value")=='T',"exec preserves exact covered cwd");
        ck(value(atoi(argv[3]),"value")=='T',"exec preserves foreign directory fd");
        ck(listed_mount(argv[2]),"exec proc report uses private namespace");
        printf("MOUNTNS EXEC PASS checks=%d\n",count);return 0;
    }
    int linux_userns=argc==3&&!strcmp(argv[2],"--linux-userns-fixture");
    ck((argc==2||linux_userns)&&!strcmp(argv[1],"--disposable-vm")&&argv[0][0]=='/',"absolute executable and disposable fixture");
    ck(geteuid()==0,"root-only acceptance fixture");
    char* base_args[]={argv[0],"--disposable-vm",NULL};
    ck(!mount_namespace_discovery_main(2,base_args),"Linux-derived base probe");
    char base[]="/tmp/brook-mount-accept-XXXXXX";ck(mkdtemp(base)!=NULL,"acceptance fixture");
    char a[PATH_MAX],b[PATH_MAX],target[PATH_MAX],name[PATH_MAX],other[PATH_MAX];
    joinpath(a,base,"a");joinpath(b,base,"b");joinpath(target,base,"target");
    ck(!mkdir(a,0700)&&!mkdir(b,0700)&&!mkdir(target,0700),"acceptance directories");
    joinpath(name,a,"value");put(name,'A');joinpath(name,b,"value");put(name,'B');joinpath(name,target,"value");put(name,'T');
    joinpath(name,a,"version");put(name,'V');joinpath(name,a,"sub");ck(!mkdir(name,0700),"acceptance nested directory");
    joinpath(name,a,"escape");ck(!symlink("../b/value",name),"relative escape fixture");
    joinpath(name,a,"absolute");joinpath(other,b,"value");ck(!symlink(other,name),"absolute link fixture");
    int original=open(target,O_RDONLY|O_DIRECTORY);ck(original>=0,"original target fd");
    int ready[2],go[2];ck(!pipe(ready)&&!pipe(go),"acceptance rendezvous");
    pid_t child=private_child();
    if(!child) {
        close(ready[0]);close(go[1]);ck(!fchdir(original),"covered cwd before bind");
        ck(!mount(a,target,NULL,MS_BIND,NULL),"private acceptance bind");
        ck(listed_mount(target),"private proc report contains bind");
        char descriptor[32];snprintf(descriptor,sizeof(descriptor),"%d",original);
        pid_t exec_child=fork();ck(exec_child>=0,"fork inherits namespace");
        if(!exec_child) {char* args[]={argv[0],"--exec-preserve",target,descriptor,NULL};execv(argv[0],args);_exit(91);}
        wait_ok(exec_child,"fork/exec namespace child complete");
        joinpath(name,target,"escape");ck(value(AT_FDCWD,name)=='B',"attached bind dotdot uses covered parent");
        int held=open(target,O_RDONLY|O_DIRECTORY);ck(held>=0,"held bound root");
        sendbyte(ready[1]);recvbyte(go[0]);
        ck(!umount2(target,MNT_DETACH),"acceptance detach");
        ck(!listed_mount(target),"private proc report drops detached bind");
        ck(value(held,"../value")=='A',"detached root dotdot clamps");
        ck(value(held,"sub/../../value")=='A',"nested detached root clamp");
        ck(value(held,"escape")==-ENOENT,"relative symlink cannot reach physical backend parent");
        ck(value(held,"absolute")=='B',"absolute symlink restarts at process root");close(held);
        // /proc/version is also supplied by Brook's syscall broker. Covering
        // /proc must make the replacement file win in every lookup path.
        ck(!mount(a,"/proc",NULL,MS_BIND,NULL),"cover broker-owned proc directory");
        ck(value(AT_FDCWD,"/proc/version")=='V',"bind masks synthetic version broker");
        ck(!umount2("/proc",MNT_DETACH),"restore private proc directory");
        printf("MOUNTNS PRIVATE PASS checks=%d\n",count);_exit(0);
    }
    close(ready[1]);close(go[0]);recvbyte(ready[0]);
    ck(!listed_mount(target),"parent proc report excludes child's bind");
    pid_t sibling=fork();ck(sibling>=0,"ordinary sibling fork");
    if(!sibling) {
        joinpath(name,target,"value");ck(value(AT_FDCWD,name)=='T',"sibling topology unchanged");
        ck(!listed_mount(target),"sibling report unchanged");_exit(0);
    }
    wait_ok(sibling,"sibling complete");sendbyte(go[1]);wait_ok(child,"private acceptance complete");
    close(ready[0]);close(go[1]);

    ck(!pipe(ready)&&!pipe(go),"parallel rendezvous");pid_t workers[2];
    for(unsigned i=0;i<2;++i) {
        workers[i]=private_child();
        if(!workers[i]) {
            close(ready[0]);close(go[1]);sendbyte(ready[1]);recvbyte(go[0]);
            parallel_mutation(i?b:a,target,i?'B':'A');_exit(0);
        }
    }
    close(ready[1]);close(go[0]);recvbyte(ready[0]);recvbyte(ready[0]);sendbyte(go[1]);sendbyte(go[1]);
    parallel_mutation(b,target,'B');wait_ok(workers[0],"parallel A complete");wait_ok(workers[1],"parallel B complete");
    close(ready[0]);close(go[1]);
    for(unsigned i=0;i<8;++i) {
        child=(i&1)?private_child3():private_child();
        if(!child) {ck(!mount(a,target,NULL,MS_BIND,NULL),"exit with mounted private tree");_exit(0);}
        wait_ok(child,"repeated private namespace exit");
        joinpath(name,target,"value");ck(value(AT_FDCWD,name)=='T',"exit cannot alter parent topology");
    }

    // Last-member publication ends at logical exit, while a zombie owner and
    // its follower's foreign descriptors still retain filesystem storage.
    int done[2];ck(!pipe(ready)&&!pipe(go)&&!pipe(done),"last-member rendezvous");
    pid_t owner=private_child();
    if(!owner) {
        close(go[1]);close(done[0]);ck(!mount(a,target,NULL,MS_BIND,NULL),"last-member owner bind");
        int foreign=open(base,O_RDONLY|O_DIRECTORY),held=open(target,O_RDONLY|O_DIRECTORY);
        ck(foreign>=0&&held>=0,"owner exact capabilities");
        pid_t follower=private_child();
        if(!follower) {
            close(ready[0]);sendbyte(ready[1]);recvbyte(go[0]);
            ck(value(foreign,"target/value")=='T',"foreign namespace publication ends with last member");
            joinpath(name,target,"value");ck(value(AT_FDCWD,name)=='A',"copied namespace retains own bind");
            int up=openat(held,"..",O_RDONLY|O_DIRECTORY);ck(up>=0&&same(up,held),"retired root capability clamps dotdot");close(up);
            ck(value(held,"value")=='A',"retired attachment retains backing");
            sendbyte(done[1]);_exit(0);
        }
        close(ready[1]);recvbyte(ready[0]);_exit(0);
    }
    close(ready[0]);close(ready[1]);close(go[0]);close(done[1]);
    siginfo_t death;memset(&death,0,sizeof(death));
    ck(!waitid(P_PID,owner,&death,WEXITED|WNOWAIT)&&death.si_pid==owner&&death.si_code==CLD_EXITED&&!death.si_status,"owner logically exited but remains zombie");
    sendbyte(go[1]);recvbyte(done[0]);char eof;ck(read(done[0],&eof,1)==0,"follower closed completion channel");
    wait_ok(owner,"reap namespace owner");close(go[1]);close(done[0]);

    if(!linux_userns) {
        child=fork();ck(child>=0,"unprivileged negative fixture");
        if(!child) {ck(!setuid(1000),"drop maintenance credentials");negative_privilege();_exit(0);}
        wait_ok(child,"negative privilege fixture complete");
    } else {
        printf("FIXTURE NOTE single-UID Linux mapping: run --unprivileged separately outside this user namespace\n");
    }
    errno=0;long invalid=syscall(SYS_clone,(unsigned long)(CLONE_NEWNS|CLONE_FS|SIGCHLD),0L,0L,0L,0L);
    if(!invalid)_exit(93);
    ck(invalid<0&&errno==EINVAL,"NEWNS shared FS rejected before birth");
    unsigned long long extended[12]={0};extended[0]=CLONE_NEWNS;extended[4]=SIGCHLD;extended[11]=1;
    errno=0;invalid=syscall(SYS_clone3,extended,sizeof(extended));
    if(!invalid)_exit(94);
    ck(invalid<0&&errno==E2BIG,"nonzero clone3 extension rejected");
    ck(!close(original),"close original capability");
    const char* leaves[]={"value","version","escape","absolute"};
    for(unsigned i=0;i<sizeof(leaves)/sizeof(leaves[0]);++i) {joinpath(name,a,leaves[i]);ck(!unlink(name),"remove source fixture");}
    joinpath(name,a,"sub");ck(!rmdir(name),"remove source subdirectory");joinpath(name,b,"value");ck(!unlink(name),"remove B fixture");joinpath(name,target,"value");ck(!unlink(name),"remove T fixture");
    ck(!rmdir(a)&&!rmdir(b)&&!rmdir(target)&&!rmdir(base),"acceptance cleanup");
    printf("MOUNTNS ACCEPTANCE PASS checks=%d fork/exec/sibling/concurrent/exit/foreign-fd/root-clamp/broker privilege=%s\n",count,linux_userns?"separate-fixture-required":"passed");return 0;
}
