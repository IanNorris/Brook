#define _DEFAULT_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int probe(unsigned long long* bg,unsigned long long* pending,unsigned long long* dirty){
 FILE *f=fopen("/proc/ext2probe","r");if(!f){perror("probe open");return -1;}
 char line[1024]; if(!fgets(line,sizeof(line),f)){fclose(f);return -1;}
 int rows=0;*bg=*pending=*dirty=0;
 while(fgets(line,sizeof(line),f)){unsigned long long v[16];char *p=line,*end;
  for(int i=0;i<16;i++){v[i]=strtoull(p,&end,10);if(end==p){fclose(f);return -1;}p=end;}
  *pending+=v[2];*bg+=v[11];*dirty+=v[13]+v[14]+v[15];rows++;
 }
 fclose(f);return rows?0:-1;
}
int main(int argc,char **argv){
 setbuf(stdout,NULL);unsigned long long before,pending,dirty,after;
 const char *path=argc==2?argv[1]:"/data/IDLE.TXT";
 if(probe(&before,&pending,&dirty)){puts("IDLE_PROBE_FAILED");return 1;}
 int fd=open(path,O_CREAT|O_EXCL|O_WRONLY,0600);if(fd<0){perror("open");return 2;}
 const char value[]="periodic ext2 durability witness\n";
 if(write(fd,value,sizeof(value)-1)!=(ssize_t)sizeof(value)-1 || close(fd)){perror("write/close");return 3;}
 if(probe(&after,&pending,&dirty)){puts("IDLE_PROBE_FAILED");return 4;}
 printf("IDLE_BEFORE bg=%llu pending=%llu dirty=%llu\n",after,pending,dirty);
 if(!pending && !dirty){puts("IDLE_NO_PENDING_WORK");return 5;}
 sleep(8);
 if(probe(&after,&pending,&dirty)){puts("IDLE_PROBE_FAILED");return 6;}
 printf("IDLE_AFTER bg=%llu pending=%llu dirty=%llu\n",after,pending,dirty);
 if(after<=before || pending || dirty){puts("IDLE_FLUSH_FAILED");return 7;}
 puts("IDLE_FLUSH_OK");for(;;)sleep(10);
}
