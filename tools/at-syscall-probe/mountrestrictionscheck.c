// Disposable Linux/Brook differential for adding bind mount restrictions.
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sched.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CK(x) do {++checks;if(!(x)){fprintf(stderr,"MRESTRICT FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static void map(const char* p,unsigned id){char t[64];int n=snprintf(t,sizeof(t),"0 %u 1\n",id);int f=open(p,O_WRONLY);CK(f>=0);CK(write(f,t,n)==n);CK(!close(f));}
static void waitok(pid_t p){int status;CK(waitpid(p,&status,0)==p);CK(WIFEXITED(status)&&!WEXITSTATUS(status));}
static char deviceMount[512];
static unsigned long deviceflags(void){
 FILE* f=fopen("/proc/self/mountinfo","r");CK(f);char line[8192],root[2048],point[2048],options[512];unsigned long flags=0;int found=0;size_t longest=0;
 while(fgets(line,sizeof(line),f)) {
  CK(sscanf(line,"%*u %*u %*s %2047s %2047s %511s",root,point,options)==3);
  size_t length=strlen(point);if(length<longest || strncmp("/dev/null",point,length) || (strcmp(point,"/") && "/dev/null"[length] && "/dev/null"[length]!='/'))continue;
  longest=length;CK(length<sizeof(deviceMount));strcpy(deviceMount,point);found=1;flags=0;char* state;for(char* token=strtok_r(options,",",&state);token;token=strtok_r(NULL,",",&state)) {
   if(!strcmp(token,"ro"))flags|=MS_RDONLY;
   if(!strcmp(token,"nosuid"))flags|=MS_NOSUID;
   if(!strcmp(token,"nodev"))flags|=MS_NODEV;
   if(!strcmp(token,"noexec"))flags|=MS_NOEXEC;
   if(!strcmp(token,"relatime"))flags|=MS_RELATIME;
   if(!strcmp(token,"noatime"))flags|=MS_NOATIME;
   if(!strcmp(token,"nodiratime"))flags|=MS_NODIRATIME;
  }
 }
 CK(found&&!ferror(f)&&!fclose(f));return flags;
}
static void transfer(int fd){
 int pair[2];CK(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
 char byte='F',control[CMSG_SPACE(sizeof(int))];struct iovec vec={&byte,1};struct msghdr m={0};m.msg_iov=&vec;m.msg_iovlen=1;m.msg_control=control;m.msg_controllen=sizeof(control);memset(control,0,sizeof(control));
 struct cmsghdr* c=CMSG_FIRSTHDR(&m);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(c),&fd,sizeof(fd));CK(sendmsg(pair[0],&m,0)==1);
 memset(control,0,sizeof(control));m.msg_controllen=sizeof(control);CK(recvmsg(pair[1],&m,0)==1);c=CMSG_FIRSTHDR(&m);CK(c&&c->cmsg_level==SOL_SOCKET&&c->cmsg_type==SCM_RIGHTS&&c->cmsg_len==CMSG_LEN(sizeof(int)));
 int received=-1;memcpy(&received,CMSG_DATA(c),sizeof(received));CK(received>=0&&write(received,"X",1)==1);CK(!close(received)&&!close(pair[0])&&!close(pair[1]));
}
int main(int argc,char** argv){
 setbuf(stdout,NULL);alarm(60);int brook=argc==3&&!strcmp(argv[2],"--brook");CK((argc==2||brook)&&!strcmp(argv[1],"--disposable-vm"));CK(geteuid()!=0);
 int existing=open("/dev/null",O_RDWR);CK(existing>=0);uid_t uid=geteuid();gid_t gid=getegid();
 errno=0;CK(mount(NULL,"/",NULL,MS_BIND|MS_REMOUNT|MS_NOSUID|MS_NODEV,NULL)==-1&&errno==EPERM);
 pid_t child=syscall(SYS_clone,(unsigned long)(CLONE_NEWUSER|CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(child>=0);
 if(child){waitok(child);int f=open("/dev/null",O_RDWR);CK(f>=0&&!close(f)&&!close(existing));printf("MRESTRICT PARENT PASS checks=%u\n",checks);return 0;}
 map("/proc/self/uid_map",uid);int f=open("/proc/self/setgroups",O_WRONLY);CK(f>=0&&write(f,"deny\n",5)==5&&!close(f));map("/proc/self/gid_map",gid);
 CK(!mount(NULL,"/",NULL,MS_PRIVATE|MS_REC,NULL));unsigned long previous=deviceflags();
 struct stat beforeStat;int hadStat=stat("/dev/null",&beforeStat);int statError=errno;
 if(brook){for(unsigned long extra=1;extra<=8;extra*=8){errno=0;CK(mount(NULL,deviceMount,NULL,MS_BIND|MS_REMOUNT|MS_NOSUID|MS_NODEV|extra,NULL)==-1&&errno==EINVAL);CK(deviceflags()==previous);}errno=0;CK(mount(NULL,deviceMount,NULL,MS_BIND|MS_REMOUNT,NULL)==-1&&errno==EINVAL);}
 CK(!mount(NULL,deviceMount,NULL,MS_BIND|MS_REMOUNT|MS_SILENT|previous|MS_NOSUID|MS_NODEV,NULL));
 CK((deviceflags()&(MS_NOSUID|MS_NODEV))==(MS_NOSUID|MS_NODEV));
 errno=0;CK(open("/dev/null",O_RDWR)==-1&&errno==EACCES);
 CK(write(existing,"X",1)==1);transfer(existing);struct stat st;CK(!fstat(existing,&st));CK((st.st_mode&S_IFMT)==S_IFCHR);
 errno=0;int afterStat=stat("/dev/null",&st);CK(afterStat==hadStat);if(hadStat)CK(errno==statError);else CK(st.st_mode==beforeStat.st_mode);
 char shm[128];snprintf(shm,sizeof(shm),"/dev/shm/brook-mrestrict-%ld",(long)getpid());f=open(shm,O_CREAT|O_EXCL|O_RDWR,0600);CK(f>=0);CK(write(f,"S",1)==1&&!close(f)&&!unlink(shm));
 pid_t copy=syscall(SYS_clone,(unsigned long)(CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(copy>=0);
 if(!copy){errno=0;CK(open("/dev/null",O_RDWR)==-1&&errno==EACCES);CK(write(existing,"X",1)==1);puts("MRESTRICT COPY PASS");_exit(0);}waitok(copy);
 CK(!close(existing));printf("MRESTRICT FIXTURE PASS checks=%u brook_cases=%d\n",checks,brook);_exit(0);
}
