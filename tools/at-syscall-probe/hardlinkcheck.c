#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
static unsigned checks;
#define CK(x) do {++checks;if(!(x)){fprintf(stderr,"HARDLINK FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static struct stat metadata(const char* p,int nofollow){struct stat s;CK((nofollow?lstat(p,&s):stat(p,&s))==0);return s;}
static void same(const char* a,const char* b,unsigned links){struct stat x=metadata(a,0),y=metadata(b,0);CK(x.st_dev==y.st_dev&&x.st_ino==y.st_ino&&x.st_nlink==links&&y.st_nlink==links);}
int main(int argc,char** argv){
 const char* parent=argc>1?argv[1]:"/tmp";int require_empty=argc>2&&!strcmp(argv[2],"--require-empty");
 char base[512];CK(snprintf(base,sizeof(base),"%s/brook-hardlink-XXXXXX",parent)>0);CK(mkdtemp(base));CK(!chdir(base));
 int d=open(".",O_PATH|O_DIRECTORY|O_CLOEXEC);CK(d>=0);int f=open("a",O_CREAT|O_EXCL|O_RDWR,0600);CK(f>=0);CK(write(f,"old",3)==3);
 int path=open("a",O_PATH|O_CLOEXEC);CK(path>=0);
 CK(syscall(SYS_link,"a","b")==0);same("a","b",2);
 CK(syscall(SYS_linkat,d,"a",d,"c",0)==0);same("a","c",3);
 int w=open("c",O_WRONLY);CK(w>=0);CK(write(w,"new",3)==3);CK(!close(w));char text[4]={0};CK(lseek(f,0,SEEK_SET)==0&&read(f,text,3)==3&&!strcmp(text,"new"));
 CK(!rename("a","moved"));CK(!unlink("b"));same("moved","c",2);
 errno=0;CK(link("c","moved")==-1&&errno==EEXIST);errno=0;CK(link("missing","bad")==-1&&errno==ENOENT);
 errno=0;CK(link(".","bad-dir")==-1&&errno==EPERM);errno=0;CK(syscall(SYS_linkat,d,"c",d,"bad",0x80000000u)==-1&&errno==EINVAL);
 errno=0;CK(syscall(SYS_linkat,d,"",d,"bad",0)==-1&&errno==ENOENT);
 CK(!symlink("c","sym"));CK(!syscall(SYS_linkat,d,"sym",d,"sym-copy",0));struct stat s=metadata("sym",1),t=metadata("sym-copy",1);CK(S_ISLNK(t.st_mode)&&s.st_ino==t.st_ino&&t.st_nlink==2);
 CK(!syscall(SYS_linkat,d,"sym",d,"followed",AT_SYMLINK_FOLLOW));same("c","followed",3);
 errno=0;int empty=syscall(SYS_linkat,path,"",d,"empty",AT_EMPTY_PATH);if(require_empty)CK(empty==0);else CK(empty==0||(empty==-1&&errno==ENOENT));
 char proc[64];snprintf(proc,sizeof(proc),"/proc/self/fd/%d",path);CK(!syscall(SYS_linkat,AT_FDCWD,proc,d,"proc-alias",AT_SYMLINK_FOLLOW));same("c","proc-alias",empty?4:5);
 CK(!chmod("c",0444));CK(!syscall(SYS_linkat,d,"c",d,"owned-readonly",0));CK(!unlink("owned-readonly"));
 const char* names[]={"moved","c","followed","proc-alias","sym","sym-copy"};for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);++i)CK(!unlink(names[i]));if(!empty)CK(!unlink("empty"));
 struct stat gone;CK(!fstat(f,&gone)&&gone.st_nlink==0);CK(lseek(f,0,SEEK_SET)==0&&read(f,text,3)==3&&!strcmp(text,"new"));
 errno=0;CK(syscall(SYS_linkat,path,"",d,"resurrect",AT_EMPTY_PATH)==-1&&errno==ENOENT);
 CK(!close(path)&&!close(f)&&!close(d));CK(!chdir("/"));CK(!rmdir(base));printf("HARDLINK PASS checks=%u empty_path=%s\n",checks,empty?"permission-denied":"accepted");return 0;
}
