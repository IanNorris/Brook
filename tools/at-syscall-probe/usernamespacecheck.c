#define _GNU_SOURCE
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <sys/sem.h>
#include <sys/signalfd.h>
#include <sched.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <grp.h>
#include <signal.h>
static unsigned checks;
static gid_t* fixtureGroups;static int fixtureGroupCount;static gid_t fixtureGroup;
static pid_t outsider;static int outsiderGo[2],outsiderDone[2];
#define CK(x) do{++checks;if(!(x)){fprintf(stderr,"FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
struct capheader {uint32_t version;int32_t pid;};
struct capdata {uint32_t effective,permitted,inheritable;};
static void caps(struct capdata d[2]) {struct capheader h={0x20080522,0};CK(!syscall(SYS_capget,&h,d));}
static void dropcaps(void) {struct capheader h={0x20080522,0};struct capdata d[2]={{0},{0}};CK(!syscall(SYS_capset,&h,d));}
static void waitok(pid_t pid) {int status;CK(waitpid(pid,&status,0)==pid);CK(WIFEXITED(status)&&!WEXITSTATUS(status));}
static void put(const char* path,const char* content,mode_t mode) {int fd=open(path,O_CREAT|O_WRONLY|O_TRUNC,mode);CK(fd>=0);CK(write(fd,content,strlen(content))==(ssize_t)strlen(content));CK(!close(fd));}
static int mapwrite(const char* path,const char* text) {int fd=open(path,O_RDWR);if(fd<0)return -errno;ssize_t n=write(fd,text,strlen(text));int e=errno;close(fd);return n==(ssize_t)strlen(text)?0:-e;}
static void idmap(unsigned inside,unsigned groupInside,uid_t outer,gid_t group) {
 char text[96];CK(mapwrite("/proc/self/uid_map","invalid")<0); // malformed must fail
 snprintf(text,sizeof(text),"%u %u 1\n",inside,outer);CK(!mapwrite("/proc/self/uid_map",text));
 CK(mapwrite("/proc/self/uid_map",text)==-EPERM);
 CK(!mapwrite("/proc/self/setgroups","deny\n"));
 snprintf(text,sizeof(text),"%u %u 1\n",groupInside,group);CK(!mapwrite("/proc/self/gid_map",text));
 CK(getuid()==inside && geteuid()==inside && getgid()==groupInside && getegid()==groupInside);
 CK(setgroups(0,NULL)<0 && errno==EPERM);
 errno=0;CK(setuid(123456)<0 && errno==EINVAL);
}
static void sendfd(int socket,int fd) {
 char byte='F',control[CMSG_SPACE(sizeof(int))]={0};struct iovec io={&byte,1};struct msghdr message={0};
 message.msg_iov=&io;message.msg_iovlen=1;message.msg_control=control;message.msg_controllen=sizeof(control);
 struct cmsghdr* cm=CMSG_FIRSTHDR(&message);cm->cmsg_level=SOL_SOCKET;cm->cmsg_type=SCM_RIGHTS;cm->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(cm),&fd,sizeof(fd));
 CK(sendmsg(socket,&message,0)==1);
}
static int recvfd(int socket) {
 char byte=0,control[CMSG_SPACE(sizeof(int))]={0};struct iovec io={&byte,1};struct msghdr message={0};
 message.msg_iov=&io;message.msg_iovlen=1;message.msg_control=control;message.msg_controllen=sizeof(control);
 CK(recvmsg(socket,&message,0)==1&&byte=='F'&&!(message.msg_flags&MSG_CTRUNC));
 struct cmsghdr* cm=CMSG_FIRSTHDR(&message);CK(cm&&cm->cmsg_level==SOL_SOCKET&&cm->cmsg_type==SCM_RIGHTS&&cm->cmsg_len==CMSG_LEN(sizeof(int)));
 int fd=-1;memcpy(&fd,CMSG_DATA(cm),sizeof(fd));CK(fd>=0);return fd;
}
static char base[96],source[128],target[128],file[128];
static void paths(void) {snprintf(source,sizeof(source),"%s/a",base);snprintf(target,sizeof(target),"%s/t",base);snprintf(file,sizeof(file),"%s/value",source);}
static int value(const char* path) {char ch=0;int fd=open(path,O_RDONLY);CK(fd>=0);CK(read(fd,&ch,1)==1);CK(!close(fd));return ch;}
static void exec_case(char** argv,unsigned uid,unsigned gid,int expectAdmin) {
 char u[20],g[20],a[8];snprintf(u,sizeof(u),"%u",uid);snprintf(g,sizeof(g),"%u",gid);snprintf(a,sizeof(a),"%d",expectAdmin);
 char* args[]={argv[0],"--exec",u,g,a,base,NULL};execv(argv[0],args);perror("execv");_exit(91);
}
static void namespace_abi(unsigned uid,unsigned gid) {
 gid_t* groups=calloc(fixtureGroupCount+1,sizeof(gid_t));CK(groups!=NULL);CK(getgroups(fixtureGroupCount+1,groups)==fixtureGroupCount);
 for(int i=0;i<fixtureGroupCount;++i){CK(groups[i]==(fixtureGroups[i]==fixtureGroup?gid:65534));}
 free(groups);
 char path[64],report[512]={0};snprintf(path,sizeof(path),"/proc/%d/uid_map",getppid());int map=open(path,O_RDONLY);CK(map>=0);
 CK(read(map,report,sizeof(report)-1)>0);CK(!close(map));unsigned first,lower,count;
 CK(sscanf(report,"%u %u %u",&first,&lower,&count)==3&&first==0&&lower==UINT32_MAX&&count==UINT32_MAX);
 map=open("/proc/self/status",O_RDONLY);CK(map>=0);memset(report,0,sizeof(report));CK(read(map,report,sizeof(report)-1)>0);CK(!close(map));
 char* ids=strstr(report,"Uid:");unsigned real,effective,saved,fs;CK(ids&&sscanf(ids+4,"%u %u %u %u",&real,&effective,&saved,&fs)==4&&real==uid&&effective==uid&&saved==uid&&fs==uid);
 uid_t r,e,s;gid_t gr,ge,gs;CK(!getresuid(&r,&e,&s)&&r==uid&&e==uid&&s==uid);
 CK(!getresgid(&gr,&ge,&gs)&&gr==gid&&ge==gid&&gs==gid);
 int pair[2];CK(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));int enabled=1;
 CK(!setsockopt(pair[0],SOL_SOCKET,SO_PASSCRED,&enabled,sizeof(enabled)));
 char byte='C',ctl[CMSG_SPACE(sizeof(struct ucred))]={0};struct iovec io={&byte,1};struct msghdr msg={0};
 msg.msg_iov=&io;msg.msg_iovlen=1;msg.msg_control=ctl;msg.msg_controllen=sizeof(ctl);
 struct cmsghdr* cm=CMSG_FIRSTHDR(&msg);cm->cmsg_level=SOL_SOCKET;cm->cmsg_type=SCM_CREDENTIALS;cm->cmsg_len=CMSG_LEN(sizeof(struct ucred));
 struct ucred creds={getpid(),uid,gid};memcpy(CMSG_DATA(cm),&creds,sizeof(creds));CK(sendmsg(pair[1],&msg,0)==1);
 memset(ctl,0,sizeof(ctl));msg.msg_controllen=sizeof(ctl);CK(recvmsg(pair[0],&msg,0)==1);
 cm=CMSG_FIRSTHDR(&msg);CK(cm&&cm->cmsg_type==SCM_CREDENTIALS&&cm->cmsg_len==CMSG_LEN(sizeof(creds)));
 memcpy(&creds,CMSG_DATA(cm),sizeof(creds));CK(creds.pid==getpid()&&creds.uid==uid&&creds.gid==gid);CK(!close(pair[0])&&!close(pair[1]));
 sigset_t mask,previous;sigemptyset(&mask);sigaddset(&mask,SIGUSR1);CK(!sigprocmask(SIG_BLOCK,&mask,&previous));
 CK(!kill(getpid(),SIGUSR1));siginfo_t info;struct timespec timeout={0};CK(sigtimedwait(&mask,&info,&timeout)==SIGUSR1&&info.si_uid==uid);
 int fd=signalfd(-1,&mask,SFD_NONBLOCK|SFD_CLOEXEC);CK(fd>=0);CK(!syscall(SYS_tgkill,getpid(),syscall(SYS_gettid),SIGUSR1));
 struct signalfd_siginfo si;CK(read(fd,&si,sizeof(si))==sizeof(si)&&si.ssi_uid==uid&&si.ssi_pid==(unsigned)getpid());
 CK(!close(fd)&&!sigprocmask(SIG_SETMASK,&previous,NULL));
 int sem=semget(IPC_PRIVATE,1,IPC_CREAT|0600);CK(sem>=0);struct semid_ds ds;union {int value;struct semid_ds* ds;unsigned short* array;} arg;arg.ds=&ds;
 CK(!semctl(sem,0,IPC_STAT,arg));CK(ds.sem_perm.uid==uid&&ds.sem_perm.cuid==uid&&ds.sem_perm.gid==gid&&ds.sem_perm.cgid==gid);
 CK(!semctl(sem,0,IPC_SET,arg));CK(!semctl(sem,0,IPC_RMID));
}
int main(int argc,char** argv) {
 setbuf(stdout,NULL);alarm(90);
 if(argc==6&&!strcmp(argv[1],"--exec")) {
  unsigned uid=strtoul(argv[2],NULL,10),gid=strtoul(argv[3],NULL,10);int admin=atoi(argv[4]);snprintf(base,sizeof(base),"%s",argv[5]);paths();
  CK(getuid()==uid&&geteuid()==uid&&getgid()==gid&&getegid()==gid);
  CK(getauxval(AT_UID)==uid&&getauxval(AT_EUID)==uid&&getauxval(AT_GID)==gid&&getauxval(AT_EGID)==gid);
  struct capdata d[2];caps(d);CK(!!(d[0].effective&(1u<<21))==admin);
  if(admin){CK(!mount(source,target,NULL,MS_BIND,NULL));CK(!umount2(target,MNT_DETACH));}
  else CK(mount(source,target,NULL,MS_BIND,NULL)<0&&errno==EPERM);
  printf("USERNS EXEC PASS uid=%u checks=%u\n",uid,checks);return 0;
 }
 CK(argc==2 && (!strcmp(argv[1],"--disposable-vm")||!strcmp(argv[1],"--native-root-fixture")) && argv[0][0]=='/');
 snprintf(base,sizeof(base),"/tmp/userns-XXXXXX");CK(mkdtemp(base));paths();
 CK(!mkdir(source,0777)&&!mkdir(target,0777));put(file,"A",0644);
 char secret[128];snprintf(secret,sizeof(secret),"%s/secret",base);
 if(!strcmp(argv[1],"--native-root-fixture")) {
  CK(!geteuid());
  CK(!pipe(outsiderGo)&&!pipe(outsiderDone));outsider=fork();CK(outsider>=0);
  if(!outsider) {
   CK(!close(outsiderGo[1])&&!close(outsiderDone[0]));
   CK(!setgroups(0,NULL)&&!setgid(100)&&!setuid(1001));pid_t targetPid=0;
   CK(read(outsiderGo[0],&targetPid,sizeof(targetPid))==sizeof(targetPid));char path[96];snprintf(path,sizeof(path),"/proc/%d/uid_map",targetPid);
   CK(open(path,O_RDWR)<0&&errno==EACCES);CK(write(outsiderDone[1],"x",1)==1);_exit(0);
  }
  CK(!close(outsiderGo[0])&&!close(outsiderDone[1]));
  put(secret,"R",0600);CK(!chown(base,1000,100));CK(!chown(source,1000,100));CK(!chown(target,1000,100));CK(!chown(file,1000,100));
  gid_t inheritedGroup=100;CK(!setgroups(1,&inheritedGroup));CK(!setgid(100));CK(!setuid(1000));
 } else snprintf(secret,sizeof(secret),"/etc/shadow");
 uid_t outer=geteuid();gid_t outerGroup=getegid();CK(outer!=0);
 fixtureGroup=outerGroup;fixtureGroupCount=getgroups(0,NULL);CK(fixtureGroupCount>=0);
 fixtureGroups=calloc(fixtureGroupCount+1,sizeof(gid_t));CK(fixtureGroups!=NULL);CK(getgroups(fixtureGroupCount+1,fixtureGroups)==fixtureGroupCount);
 char covered[128];snprintf(covered,sizeof(covered),"%s/value",target);put(covered,"T",0644);
 int pair[2];CK(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
 for(unsigned iteration=0;iteration<2;++iteration) {
  unsigned inner=iteration?1000:0,innerGroup=iteration?100:0;
  int gate[2];CK(!pipe(gate));
  uint64_t cloneArgs[11]={0};cloneArgs[0]=CLONE_NEWUSER|CLONE_NEWNS;cloneArgs[4]=SIGCHLD;
  pid_t pid=iteration?syscall(SYS_clone3,cloneArgs,sizeof(cloneArgs)):
      syscall(SYS_clone,(unsigned long)(CLONE_NEWUSER|CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(pid>=0);
  if(!pid) {
   CK(!close(gate[1]));char go;CK(read(gate[0],&go,1)==1);CK(!close(gate[0]));
   CK(geteuid()==65534 && getegid()==65534);idmap(inner,innerGroup,outer,outerGroup);
   namespace_abi(inner,innerGroup);
   struct capdata d[2];caps(d);CK(d[0].effective&(1u<<21));
   int fd=open(secret,O_RDONLY);CK(fd<0 && (errno==EACCES||errno==EPERM));
   struct stat st;CK(!stat(file,&st)&&st.st_uid==inner&&st.st_gid==innerGroup);
   struct ucred peer;socklen_t len=sizeof(peer);CK(!getsockopt(pair[0],SOL_SOCKET,SO_PEERCRED,&peer,&len));CK(peer.uid==inner&&peer.gid==innerGroup);
   CK(!mount(source,target,NULL,MS_BIND,NULL));CK(value(covered)=='A');
   pid_t inherited=fork();CK(inherited>=0);if(!inherited){CK(value(covered)=='A');_exit(0);}waitok(inherited);
   CK(!umount2(target,MNT_DETACH));CK(value(covered)=='T');
   errno=0;CK(umount2("/proc",MNT_DETACH)<0&&errno==EINVAL);
   int mapfd=open("/proc/self",O_RDONLY|O_DIRECTORY);CK(mapfd>=0);int uidfd=openat(mapfd,"uid_map",O_RDONLY);CK(uidfd>=0);char text[96]={0};CK(read(uidfd,text,sizeof(text)-1)>0);CK(!close(uidfd)&&!close(mapfd));
   CK(prctl(PR_CAPBSET_READ,21,0,0,0)==1);CK(!prctl(PR_CAPBSET_DROP,21,0,0,0));CK(!prctl(PR_CAPBSET_READ,21,0,0,0));
   pid_t execpid=fork();CK(execpid>=0);if(!execpid)exec_case(argv,inner,innerGroup,0);waitok(execpid);
   dropcaps();CK(mount(source,target,NULL,MS_BIND,NULL)<0&&errno==EPERM);
   caps(d);CK(!d[0].effective&&!d[1].effective&&!d[0].permitted&&!d[1].permitted);
   struct capheader h={0x20080522,0};d[0].effective=d[0].permitted=1u<<21;CK(syscall(SYS_capset,&h,d)<0&&errno==EPERM);
   printf("USERNS CHILD PASS inner=%u checks=%u\n",inner,checks);_exit(0);
  }
  if(outsider && !iteration) {
   CK(write(outsiderGo[1],&pid,sizeof(pid))==sizeof(pid));char ack;CK(read(outsiderDone[0],&ack,1)==1);waitok(outsider);outsider=0;
   CK(!close(outsiderGo[1])&&!close(outsiderDone[0]));
  }
  CK(write(gate[1],"x",1)==1);CK(!close(gate[0])&&!close(gate[1]));
  waitok(pid);CK(geteuid()==outer&&getegid()==outerGroup);CK(value(covered)=='T');
 }
 // A new user namespace alone has no authority over inherited parent mounts.
 pid_t pid=syscall(SYS_clone,(unsigned long)(CLONE_NEWUSER|SIGCHLD),0L,0L,0L,0L);CK(pid>=0);
 if(!pid) {idmap(0,0,outer,outerGroup);CK(mount(source,target,NULL,MS_BIND,NULL)<0&&errno==EPERM);_exit(0);}waitok(pid);
 // setgroups authority is checked at writable open, then delegated by the
 // file; a capless owner may still read its policy.
 pid=syscall(SYS_clone,(unsigned long)(CLONE_NEWUSER|SIGCHLD),0L,0L,0L,0L);CK(pid>=0);
 if(!pid) {
  int authorized=open("/proc/self/setgroups",O_RDWR);CK(authorized>=0);dropcaps();
  int readPolicy=open("/proc/self/setgroups",O_RDONLY);CK(readPolicy>=0);CK(!close(readPolicy));
  CK(open("/proc/self/setgroups",O_WRONLY)<0&&errno==EACCES);CK(open("/proc/self/setgroups",O_RDWR)<0&&errno==EACCES);
  CK(write(authorized,"deny\n",5)==5);CK(!close(authorized));_exit(0);
 }waitok(pid);
 // An open map survives target exit; descriptor transfer preserves opening
 // authority; possession delegates that single-ID authority with the file.
 for(unsigned emptyOpener=0;emptyOpener<2;++emptyOpener) {
  pid=syscall(SYS_clone,(unsigned long)(CLONE_NEWUSER|SIGCHLD),0L,0L,0L,0L);CK(pid>=0);
  if(!pid) {if(emptyOpener)dropcaps();int fd=open("/proc/self/uid_map",O_RDWR);CK(fd>=0);sendfd(pair[1],fd);CK(!close(fd));_exit(0);}
  int fd=recvfd(pair[0]);waitok(pid);char text[96]={0};CK(read(fd,text,sizeof(text))==0);
  char mapping[96];snprintf(mapping,sizeof(mapping),"7 %u 1\n",outer);
  pid_t sibling=syscall(SYS_clone,(unsigned long)(CLONE_NEWUSER|SIGCHLD),0L,0L,0L,0L);CK(sibling>=0);
  if(!sibling){ssize_t n=write(fd,mapping,strlen(mapping));
   if(emptyOpener)CK(n<0&&errno==EPERM);
   else {CK(n==(ssize_t)strlen(mapping));CK(lseek(fd,0,SEEK_SET)==0);char report[96]={0};CK(read(fd,report,sizeof(report)-1)>0);
    unsigned in=0,out=0,count=0;CK(sscanf(report,"%u %u %u",&in,&out,&count)==3&&in==7&&out==outer&&count==1);}
   _exit(0);}waitok(sibling);
  CK(lseek(fd,0,SEEK_SET)==0);
  ssize_t result=write(fd,mapping,strlen(mapping));
  CK(result<0&&errno==EPERM);
  CK(!close(fd));
 }
 CK(!close(pair[0])&&!close(pair[1]));
 CK(!unlink(file)&&!unlink(covered));CK(!rmdir(source)&&!rmdir(target));
 // Native secret remains root-owned but parent owns its containing directory.
 if(!strcmp(argv[1],"--native-root-fixture")){CK(!unlink(secret));}
 CK(!rmdir(base));
 free(fixtureGroups);printf("USERNS PARENT PASS checks=%u\n",checks);return 0;
}
