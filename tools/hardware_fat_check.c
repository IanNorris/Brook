/* Diagnostic for a disposable installer fixture. Not an authorization gate:
 * the installer device currently permits writes and ownership is not retained
 * across the checker and a later mount. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    setbuf(stdout, NULL);
    static const char expected[]="brook-ssd-recovery-readonly-v1\n";
    char identity[sizeof(expected)+1];
    int fd=open("/etc/hardware-payload-id",O_RDONLY);
    ssize_t size=fd<0?-1:read(fd,identity,sizeof(identity));
    if(fd>=0)close(fd);
    if(size!=(ssize_t)(sizeof(expected)-1) || memcmp(identity,expected,sizeof(expected)-1))return 2;
    printf("RECOVERY_PAYLOAD %s",expected);
    puts("FAT_CHECK_BEGIN: read-only diagnostic");
    pid_t child=fork();
    if(child<0) {perror("FAT_CHECK_FORK_FAILED");return 2;}
    if(!child) {
        char *const args[]={"/boot/FATFSCK.ELF","-n","--variant=standard","/dev/brook-install-disk1",NULL};
        execv(args[0],args);perror("FAT_CHECK_EXEC_FAILED");_exit(127);
    }
    int status;
    while(waitpid(child,&status,0)<0) {
        if(errno==EINTR)continue;
        perror("FAT_CHECK_WAIT_FAILED");return 2;
    }
    if(!WIFEXITED(status)) {
        printf("FAT_CHECK_INTERRUPTED signal=%d\n",WIFSIGNALED(status)?WTERMSIG(status):0);
        return 2;
    }
    int code=WEXITSTATUS(status);
    printf("FAT_CHECK_RESULT exit=%d\n",code);
    return code==0?0:1;
}
