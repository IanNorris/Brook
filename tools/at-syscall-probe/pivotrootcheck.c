// Portable root-relocation/exec probe; requires a disposable VM.
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
#define CK(x) do {++checks;if(!(x)){fprintf(stderr,"PIVOT FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static void map(const char* p,unsigned id){char t[64];int n=snprintf(t,sizeof(t),"0 %u 1\n",id);int f=open(p,O_WRONLY);CK(f>=0);CK(write(f,t,n)==n);CK(!close(f));}
static void marker(void){int f=open("/marker",O_RDONLY);CK(f>=0);char c=0;CK(read(f,&c,1)==1&&c=='P');CK(!close(f));}
static void waitok(pid_t p){int s;CK(waitpid(p,&s,0)==p);CK(WIFEXITED(s)&&!WEXITSTATUS(s));}
static void cwd(const char* expected){char text[512];CK(getcwd(text,sizeof(text))&& !strcmp(text,expected));}
int main(int argc,char** argv){
 setbuf(stdout,NULL);alarm(60);
 if(argc==2&&!strcmp(argv[1],"--child-marker")){int f=open("/proc/self/uid_map",O_RDONLY);CK(f>=0);char b[128];CK(read(f,b,sizeof(b))>0);CK(!close(f));cwd("/");puts("PIVOT EXEC CHILD PASS");return 0;}
 CK(argc==2&&!strcmp(argv[1],"--disposable-vm"));CK(geteuid()!=0);
 char base[]="/tmp/pivot-XXXXXX";CK(mkdtemp(base));uid_t uid=geteuid();gid_t gid=getegid();
 errno=0;CK(syscall(SYS_pivot_root,base,base)==-1&&errno==EPERM);
 pid_t child=syscall(SYS_clone,(unsigned long)(CLONE_NEWUSER|CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(child>=0);
 if(child){waitok(child);CK(!rmdir(base));printf("PIVOT PARENT PASS checks=%u\n",checks);return 0;}
 alarm(60);map("/proc/self/uid_map",uid);int f=open("/proc/self/setgroups",O_WRONLY);CK(f>=0&&write(f,"deny\n",5)==5&&!close(f));map("/proc/self/gid_map",gid);
 CK(!mount(NULL,"/",NULL,MS_PRIVATE|MS_REC,NULL));
 CK(!mount("tmpfs",base,"tmpfs",MS_NODEV|MS_NOSUID,NULL));CK(!chdir(base));
 CK(!mkdir("oldroot",0700)&&!mkdir("newroot",0700));f=open("marker",O_CREAT|O_EXCL|O_WRONLY,0600);CK(f>=0&&write(f,"P",1)==1&&!close(f));
 CK(!mount("newroot","newroot",NULL,MS_BIND|MS_REC|MS_SILENT|0xc0ed0000UL,NULL));
 CK(!chdir("/"));int oldfd=open("/",O_RDONLY|O_DIRECTORY);CK(oldfd>=0);struct stat original;CK(!fstat(oldfd,&original));
 int sync[2];CK(!pipe(sync));pid_t observer=fork();CK(observer>=0);
 if(!observer){alarm(60);CK(!close(sync[1]));char c;CK(read(sync[0],&c,1)==1&&c=='G');marker();cwd("/");CK(!close(sync[0])&&!close(oldfd));_exit(0);}
 CK(!close(sync[0]));CK(!chdir(base));
 CK(!syscall(SYS_pivot_root,base,"oldroot"));cwd("/");marker();
 CK(write(sync[1],"G",1)==1&&!close(sync[1]));waitok(observer);
 struct stat moved;CK(!stat("/oldroot",&moved));CK(moved.st_dev==original.st_dev&&moved.st_ino==original.st_ino);
 // A retained old-root fd remains an intentional capability, not a remapped fd.
 CK(!fchdir(oldfd));cwd("/oldroot");CK(!chdir("/"));
 CK(!mount("/oldroot","/newroot",NULL,MS_BIND|MS_REC,NULL));
 CK(!mount(NULL,"/oldroot",NULL,MS_SLAVE|MS_REC,NULL));CK(!umount2("/oldroot",MNT_DETACH));
 int temporary=open("/",O_RDONLY|O_DIRECTORY);CK(temporary>=0);CK(!chdir("/newroot"));
 CK(!syscall(SYS_pivot_root,".","."));
 // Mount target traversal must follow the covering old root, even for dot.
 CK(!umount2(".",MNT_DETACH));CK(!chdir("/"));cwd("/");
 CK(!stat("/",&moved));CK(moved.st_dev==original.st_dev&&moved.st_ino==original.st_ino);
 CK(!close(temporary)&&!close(oldfd));
 pid_t execchild=fork();CK(execchild>=0);if(!execchild){execl("/proc/self/exe","pivotrootcheck","--child-marker",(char*)NULL);perror("exec /proc/self/exe");_exit(1);}waitok(execchild);
 printf("PIVOT FIXTURE PASS checks=%u\n",checks);_exit(0);
}
