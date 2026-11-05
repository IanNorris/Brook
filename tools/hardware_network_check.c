/* Local, read-only network and debug-listener diagnostics for recovery. */
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static void print_file(const char *path)
{
    char buffer[4097];
    int fd=open(path,O_RDONLY);
    if(fd<0){printf("%s: unavailable (%s)\n",path,strerror(errno));return;}
    ssize_t n=read(fd,buffer,sizeof(buffer)-1);
    if(n>=0){buffer[n]=0;printf("%s:\n%s\n",path,buffer);}
    else printf("%s: read failed (%s)\n",path,strerror(errno));
    close(fd);
}

static void interfaces(void)
{
    int fd=socket(AF_INET,SOCK_DGRAM,0);
    if(fd<0){printf("Interface query failed: %s\n",strerror(errno));return;}
    int found=0;
    for(int i=1;i<=16;i++){
        struct ifreq r;memset(&r,0,sizeof(r));r.ifr_ifindex=i;
        if(ioctl(fd,SIOCGIFNAME,&r)<0)continue;
        char name[IFNAMSIZ];memcpy(name,r.ifr_name,sizeof(name));name[sizeof(name)-1]=0;
        if(ioctl(fd,SIOCGIFADDR,&r)==0){
            char ip[INET_ADDRSTRLEN];struct sockaddr_in *a=(struct sockaddr_in *)&r.ifr_addr;
            printf("Interface %s: %s\n",name,inet_ntop(AF_INET,&a->sin_addr,ip,sizeof(ip))?ip:"invalid address");
        }else printf("Interface %s: no IPv4 address\n",name);
        found++;
    }
    if(!found)puts("No interfaces reported.");
    close(fd);
}

static int local_bridge(unsigned short port)
{
    struct timespec started;
    if(clock_gettime(CLOCK_MONOTONIC,&started)<0){puts("Monotonic clock unavailable.");return 1;}
    int fd=socket(AF_INET,SOCK_STREAM,0);
    if(fd<0){printf("Bridge socket failed: %s\n",strerror(errno));return 1;}
    int flags=fcntl(fd,F_GETFL,0);
    if(flags<0 || fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0){printf("Nonblocking socket unavailable: %s\n",strerror(errno));close(fd);return 1;}
    struct sockaddr_in a;memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_port=htons(port);a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    int rc=connect(fd,(struct sockaddr *)&a,sizeof(a));
    if(rc<0 && errno!=EINPROGRESS){printf("Loopback connect failed: %s\n",strerror(errno));close(fd);return 1;}
    struct pollfd p={fd,POLLOUT,0};
    if(rc<0 && poll(&p,1,3000)<=0){puts("Loopback connect timed out.");close(fd);return 1;}
    int error=0;socklen_t length=sizeof(error);
    if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&length)<0 || error){printf("Loopback connection failed: %s\n",strerror(error?error:errno));close(fd);return 1;}
    unsigned long long token=((unsigned long long)started.tv_sec*1000000000ull+(unsigned long long)started.tv_nsec) ^ (unsigned)getpid();
    char command[96],begin[64],end[64];
    int command_len=snprintf(command,sizeof(command),"@%llu bridge hardware caps\n",token);
    snprintf(begin,sizeof(begin),"BRIDGE_BEGIN %llu\n",token);
    snprintf(end,sizeof(end),"BRIDGE_END %llu\n",token);
    if(command_len<0 || (size_t)command_len>=sizeof(command)){close(fd);return 1;}
    size_t sent=0;
    while(sent<(size_t)command_len){
        ssize_t n=send(fd,command+sent,(size_t)command_len-sent,MSG_NOSIGNAL);
        if(n>0){sent+=(size_t)n;continue;}
        if(n<0 && errno==EINTR)continue;
        printf("Bridge request failed: %s\n",strerror(errno));close(fd);return 1;
    }
    char response[2049];size_t used=0;
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&started);
    while(used<sizeof(response)-1){
        if(clock_gettime(CLOCK_MONOTONIC,&now)<0)break;
        long long ms=((long long)now.tv_sec-started.tv_sec)*1000+(now.tv_nsec-started.tv_nsec)/1000000;
        if(ms>=4000)break;
        p.events=POLLIN;p.revents=0;int n=poll(&p,1,(int)(4000-ms));
        if(n<0){if(errno==EINTR)continue;break;}
        if(!n)continue;
        ssize_t got=recv(fd,response+used,sizeof(response)-1-used,0);
        if(got<0 && (errno==EINTR || errno==EAGAIN))continue;
        if(got<=0)break;
        used+=(size_t)got;response[used]=0;
        const char *start=response;
        while((start=strstr(start,begin)) && start!=response && start[-1]!='\n')start++;
        const char *finish=start?strstr(start+strlen(begin),end):NULL;
        if(finish && (finish==start+strlen(begin) || finish[-1]=='\n')){
            puts("Local debug bridge: framed reply received.");close(fd);return 0;
        }
    }
    puts("Local debug bridge: no complete framed reply.");close(fd);return 1;
}

int main(int argc,char **argv)
{
    unsigned long port=1234;
    if(argc==3 && strcmp(argv[1],"--port")==0){char *end;port=strtoul(argv[2],&end,10);if(*end || port<1 || port>65535)return 2;}
    else if(argc!=1)return 2;
    setbuf(stdout,NULL);
    puts("Brook recovery: network diagnostics");
    interfaces();print_file("/proc/net/dev");print_file("/proc/net/tcp");
    int result=local_bridge((unsigned short)port);
    puts("A local failure may be loopback or another client using the single listener.");
    puts("Local success does not establish cable or external network health.");
    return result;
}
