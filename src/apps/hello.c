// hello.c — Simple hello world test for Brook OS.
// Validates: printf, argc/argv, exit.

#include <stdio.h>

int main(int argc, char** argv) {
    printf("Hello from Brook OS!\n");
    printf("argc = %d\n", argc);
    for (int i = 0; i < argc; i++)
        printf("  argv[%d] = %s\n", i, argv[i]);
    return 0;
}
