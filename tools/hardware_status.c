/* Finite boot status for a read-only hardware bring-up image. */
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
int main(void) {
    FILE* payload=fopen("/etc/hardware-payload-id","r");
    if(payload){char id[96];if(fgets(id,sizeof(id),payload))printf("HARDWARE_PAYLOAD_ID %s",id);fclose(payload);}

    FILE* snapshot=fopen("/proc/brook_app_snapshot","r");
    if(snapshot){char line[320];if(fgets(line,sizeof(line),snapshot))printf("HARDWARE_APP_SNAPSHOT %s",line);fclose(snapshot);}
    int fd=socket(AF_INET,SOCK_DGRAM,0);
    if(fd<0){perror("HARDWARE_NETWORK socket");return 1;}
    for(unsigned attempt=0;attempt<30;++attempt) {
        for(int index=1;index<=8;++index) {
            struct ifreq request;memset(&request,0,sizeof(request));request.ifr_ifindex=index;
            if(ioctl(fd,SIOCGIFNAME,&request)<0 || !strcmp(request.ifr_name,"lo"))continue;
            if(ioctl(fd,SIOCGIFADDR,&request)<0)continue;
            struct sockaddr_in* address=(struct sockaddr_in*)&request.ifr_addr;
            if(!address->sin_addr.s_addr)continue;
            char text[INET_ADDRSTRLEN];
            if(!inet_ntop(AF_INET,&address->sin_addr,text,sizeof(text)))continue;
            printf("HARDWARE_NETWORK_READY %s %s debug_tcp=1234\n",request.ifr_name,text);
            puts("Share this IP for boot-pinned Brook hardware MCP access.\nHome/settings are RAM-only. Audio starts muted; internal Dell codec is not implemented.");
            close(fd);return 0;
        }
        if(!attempt){puts("HARDWARE_NETWORK waiting for an IPv4 address (up to 60s)");fflush(stdout);}
        sleep(2);
    }
    puts("HARDWARE_NETWORK_UNAVAILABLE no IPv4 address; use kernel logs/panic scanner for diagnostics.");
    close(fd);return 2;
}
