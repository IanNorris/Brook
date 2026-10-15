#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void die(const char *s) { perror(s); exit(2); }
static void path(char *out, const char *root, const char *suffix) {
    if (snprintf(out, PATH_MAX, "%s/%s", root, suffix) >= PATH_MAX) exit(2);
}
static int report(const char *expected) {
    char actual[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", actual, sizeof(actual)-1);
    if (n < 0) die("readlink exe");
    actual[n] = 0;
    int ok = !strcmp(actual, expected);
    printf("EXEPATHCHECK %s expected=%s actual=%s\n", ok ? "PASS" : "FAIL", expected, actual);
    return !ok;
}
static int run(const char *name, const char *expected, int script) {
    fflush(NULL);
    pid_t child = fork();
    if (child < 0) die("fork");
    if (!child) {
        char *argv[] = {(char *)name, script ? (char *)expected : "--report", (char *)expected, NULL};
        if (script) argv[2] = NULL;
        execv(name, argv);
        perror("execv"); _exit(2);
    }
    int status;
    if (waitpid(child, &status, 0) != child) die("waitpid");
    return !(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--report")) return report(argv[2]);
    if (argc == 4 && !strcmp(argv[1], "--interpreter")) {
        // Kernel supplied script spelling must survive interpreter resolution.
        if (!strstr(argv[2], "script-link")) return 3;
        return report(argv[3]);
    }
    if (argc != 2 || argv[1][0] != '/') {
        fprintf(stderr,"usage: exepathcheck /absolute/writable-directory\n"); return 2;
    }
    char root[PATH_MAX], dir[PATH_MAX], real[PATH_MAX], rel[PATH_MAX], abs[PATH_MAX], alias[PATH_MAX], script[PATH_MAX], scriptlink[PATH_MAX];
    if (mkdir(argv[1],0700) && errno != EEXIST) die("mkdir root");
    if (!realpath(argv[1],root)) die("realpath root");
    path(dir,root,"bin"); if (mkdir(dir,0700) && errno != EEXIST) die("mkdir bin");
    path(dir,root,"lib"); if (mkdir(dir,0700) && errno != EEXIST) die("mkdir lib");
    path(real,root,"lib/payload"); path(rel,root,"bin/relative"); path(abs,root,"absolute");
    path(alias,root,"alias"); path(script,root,"script"); path(scriptlink,root,"script-link");
    char source[PATH_MAX];
    ssize_t sourceLen = readlink("/proc/self/exe", source, sizeof(source)-1);
    if (sourceLen < 0) die("readlink source");
    source[sourceLen] = 0;
    int from = open(source,O_RDONLY), to = open(real,O_WRONLY|O_CREAT|O_TRUNC,0700);
    if (from < 0 || to < 0) die("open copy");
    char buf[8192]; ssize_t n;
    while ((n=read(from,buf,sizeof(buf))) > 0) {
        ssize_t done=0;
        while (done<n) { ssize_t wrote=write(to,buf+done,n-done); if(wrote<=0)die("write copy"); done+=wrote; }
    }
    if(n<0 || close(from) || fsync(to) || close(to)) die("copy");
    unlink(rel); unlink(abs); unlink(alias); unlink(scriptlink);
    if(symlink("../lib/payload",rel) || symlink(real,abs) || symlink("lib",alias) || symlink("script",scriptlink))die("symlink");
    to=open(script,O_WRONLY|O_CREAT|O_TRUNC,0700); if(to<0)die("script");
    n=snprintf(buf,sizeof(buf),"#!%s --interpreter\n",rel);
    if(n<0 || n>=(ssize_t)sizeof(buf) || write(to,buf,n)!=n || close(to))die("write script");
    int failures=run(rel,real,0)+run(abs,real,0);
    path(dir,root,"alias/payload"); failures+=run(dir,real,0);
    failures+=run(scriptlink,real,1);
    printf("EXEPATHCHECK %s cases=4 root=%s\n",failures?"FAIL":"PASS",root);
    return failures != 0;
}
