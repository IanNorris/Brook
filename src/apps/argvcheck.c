// Verify execve's initial stack above the old 32-argument boundary.
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 41 || argv[argc] != NULL) {
        fprintf(stderr,"argvcheck: FAIL argc=%d (expected 41 with null terminator)\n",argc);
        return 1;
    }
    for (int i=1; i<argc; ++i) {
        char expected[16];
        snprintf(expected,sizeof(expected),"arg%d",i);
        if (!argv[i] || strcmp(argv[i],expected)) {
            fprintf(stderr,"argvcheck: FAIL argument %d\n",i); return 1;
        }
    }
    puts("argvcheck: PASS 41 arguments in order, null terminator intact");
    return 0;
}
