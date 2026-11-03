/* Read-only local recovery check. The kernel's maintenance admission binds
 * this device to the configured physical disk and unmounted GPT partition.
 * This program never requests a repair or a restart. */
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
    setbuf(stderr, NULL);
    static const char expected[] = "brook-ssd-recovery-readonly-v1\n";
    char identity[sizeof(expected) + 1];
    int fd = open("/etc/hardware-payload-id", O_RDONLY);
    ssize_t size = fd < 0 ? -1 : read(fd, identity, sizeof(identity));
    if (fd >= 0) close(fd);
    if (size != (ssize_t)(sizeof(expected) - 1) ||
        memcmp(identity, expected, sizeof(expected) - 1) != 0) {
        puts("Recovery mode identity invalid. No repairs attempted.");
        return 2;
    }
    printf("RECOVERY_PAYLOAD %s", expected);
    puts("RECOVERY_READONLY_CHECK_BEGIN: checking the unmounted data filesystem");
    pid_t child = fork();
    if (child < 0) {
        perror("RECOVERY_READONLY_CHECK_FORK_FAILED");
        puts("Recovery check could not start.");
        puts("No repairs attempted.");
        return 2;
    }
    if (child == 0) {
        char *const args[] = {"/boot/E2FSCK.ELF", "-f", "-n", "/dev/brook-install-disk2", NULL};
        execv(args[0], args);
        perror("RECOVERY_READONLY_CHECK_EXEC_FAILED");
        _exit(127);
    }
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        perror("RECOVERY_READONLY_CHECK_WAIT_FAILED");
        puts("Recovery check could not finish.");
        puts("No repairs attempted.");
        return 2;
    }
    if (!WIFEXITED(status)) {
        printf("RECOVERY_READONLY_CHECK_INTERRUPTED: signal=%d; no repair attempted\n",
               WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        puts("Recovery check interrupted.");
        puts("No repairs attempted.");
        return 2;
    }
    int code = WEXITSTATUS(status);
    if (code == 0) {
        puts("RECOVERY_READONLY_CHECK_CLEAN: no filesystem errors found; no repair attempted");
        puts("Recovery check: no errors found.");
        puts("No repairs attempted.");
        return 0;
    }
    printf("RECOVERY_READONLY_CHECK_INCOMPLETE: e2fsck exit=%d; no repair attempted\n", code);
    puts(code == 4 ? "Recovery check: errors found." : "Recovery check could not finish.");
    puts("No repairs attempted.");
    return 1;
}
