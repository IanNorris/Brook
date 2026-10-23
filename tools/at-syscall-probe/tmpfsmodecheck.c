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
#define CK(x) do{++checks;if(!(x)){fprintf(stderr,"TMPFSMODE FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static void map(const char* path,unsigned id){char text[64];int n=snprintf(text,sizeof(text),"0 %u 1\n",id);int f=open(path,O_WRONLY);CK(f>=0);CK(write(f,text,n)==n);CK(close(f)==0);}
int main(int argc,char** argv){
 setbuf(stdout,NULL);int brook=argc==2&&!strcmp(argv[1],"--brook");CK(argc==1||brook);CK(geteuid()!=0);uid_t uid=geteuid();gid_t gid=getegid();
 pid_t child=syscall(SYS_clone,(unsigned long)(CLONE_NEWUSER|CLONE_NEWNS|SIGCHLD),0L,0L,0L,0L);CK(child>=0);if(child){int status;CK(waitpid(child,&status,0)==child);CK(WIFEXITED(status)&&WEXITSTATUS(status)==0);printf("TMPFSMODE PARENT PASS checks=%u\n",checks);return 0;}
 alarm(60);map("/proc/self/uid_map",uid);int f=open("/proc/self/setgroups",O_WRONLY);CK(f>=0);CK(write(f,"deny\n",5)==5);CK(close(f)==0);map("/proc/self/gid_map",gid);CK(mount(NULL,"/",NULL,MS_PRIVATE|MS_REC,NULL)==0);
 char base[]="/tmp/tmpfsmode-XXXXXX";CK(mkdtemp(base));struct stat before,st;CK(stat(base,&before)==0);
 const char* options[]={"mode=0755","mode=700","mode=0","mode=01777","mode=07777","mode=0700,mode=0755",NULL,""};unsigned modes[]={0755,0700,0,01777,07777,0755,01777,01777};
 for(unsigned i=0;i<sizeof(modes)/sizeof(modes[0]);++i){
  CK(mount("tmpfs",base,"tmpfs",MS_NOSUID|MS_NODEV,options[i])==0);CK(stat(base,&st)==0&&(st.st_mode&07777)==modes[i]&&st.st_uid==0&&st.st_gid==0&&st.st_dev!=before.st_dev);struct statfs fs;CK(statfs(base,&fs)==0&&fs.f_type==0x01021994);
  CK(umount2(base,MNT_DETACH)==0);CK(stat(base,&st)==0&&st.st_dev==before.st_dev&&st.st_ino==before.st_ino);
 }
 if(brook){const char* invalid[]={"mode=","mode=8","mode=-1","mode=10000","mode=755,","mode=755,,mode=700","size=1M","uid=0","mode=755,size=1M","mode=755x"};
  for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i){errno=0;CK(mount("tmpfs",base,"tmpfs",MS_NOSUID|MS_NODEV,invalid[i])==-1&&errno==EINVAL);CK(stat(base,&st)==0&&st.st_dev==before.st_dev&&st.st_ino==before.st_ino);}}
 CK(rmdir(base)==0);printf("TMPFSMODE FIXTURE PASS checks=%u brook_cases=%d\n",checks,brook);return 0;
}
