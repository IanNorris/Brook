#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Leave links for offline fsck and reboot readback. Use a fresh directory. */
int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) return 2;
    if (mkdir(argv[1], 0755) && errno != EEXIST) return 3;
    const unsigned lengths[] = {1, 58, 59, 60, 61, 254};
    unsigned failures = 0;
    for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        unsigned length = lengths[i];
        char target[256], path[512], got[256];
        memset(target, 'a' + i, length); target[length] = 0;
        snprintf(path, sizeof(path), "%s/link-%u", argv[1], length);
        int created = symlink(target, path);
        if (created && errno != EEXIST) { ++failures; continue; }
        struct stat info;
        memset(got, 0x5a, sizeof(got));
        ssize_t bytes = readlink(path, got, sizeof(got));
        int ok = !lstat(path, &info) && S_ISLNK(info.st_mode) &&
            info.st_size == length && bytes == length &&
            !memcmp(got, target, length) && got[length] == 0x5a;
        printf("SYMLINKCHECK %s target_bytes=%u\n", ok ? "PASS" : "FAIL", length);
        failures += !ok;
    }
    char missing[512],regular[512],got[32];
    snprintf(missing,sizeof(missing),"%s/missing",argv[1]);
    snprintf(regular,sizeof(regular),"%s/regular",argv[1]);
    int fd=open(regular,O_CREAT|O_WRONLY,0600);
    if(fd<0)return 4;
    close(fd);
    errno=0;ssize_t bytes=readlink(missing,got,sizeof(got));
    int ok=bytes==-1&&errno==ENOENT;
    printf("SYMLINKCHECK %s missing=ENOENT\n",ok?"PASS":"FAIL");failures+=!ok;
    errno=0;bytes=readlink(regular,got,sizeof(got));ok=bytes==-1&&errno==EINVAL;
    printf("SYMLINKCHECK %s regular=EINVAL\n",ok?"PASS":"FAIL");failures+=!ok;
    errno=0;bytes=readlink(argv[1],got,sizeof(got));ok=bytes==-1&&errno==EINVAL;
    printf("SYMLINKCHECK %s directory=EINVAL\n",ok?"PASS":"FAIL");failures+=!ok;
    int directory=open(argv[1],O_RDONLY|O_DIRECTORY);if(directory<0)return 5;
    errno=0;bytes=readlinkat(directory,"missing",got,sizeof(got));ok=bytes==-1&&errno==ENOENT;
    printf("SYMLINKCHECK %s readlinkat-missing=ENOENT\n",ok?"PASS":"FAIL");failures+=!ok;
    bytes=readlinkat(directory,"link-58",got,3);ok=bytes==3&&!memcmp(got,"bbb",3);
    printf("SYMLINKCHECK %s readlinkat-truncated-target\n",ok?"PASS":"FAIL");failures+=!ok;
    close(directory);
    char cwd[512];if(!getcwd(cwd,sizeof(cwd))||chdir(argv[1]))return 6;
    bytes=readlink("link-58",got,3);ok=bytes==3&&!memcmp(got,"bbb",3);
    printf("SYMLINKCHECK %s relative-dangling-target\n",ok?"PASS":"FAIL");failures+=!ok;
    if(chdir(cwd))return 7;
    if(argc==3) {
        errno=0;bytes=readlink(argv[2],got,sizeof(got));ok=bytes==-1&&errno==EINVAL;
        printf("SYMLINKCHECK %s mount-root=EINVAL\n",ok?"PASS":"FAIL");failures+=!ok;
    }
    sync();
    printf("SYMLINKCHECK %s failures=%u\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
