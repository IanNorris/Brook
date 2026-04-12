// echotest.c — Interactive keyboard input test for Brook OS
// Reads lines from stdin, echoes them back. Type 'quit' to exit.

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    printf("=== ECHO TEST ===\n");
    printf("Type lines and press Enter. Type 'quit' to exit.\n");
    printf("> ");

    char buf[256];
    while (1)
    {
        int n = read(0, buf, sizeof(buf) - 1);
        if (n <= 0)
        {
            printf("\n[EOF]\n");
            break;
        }
        buf[n] = '\0';

        // Strip trailing newline for comparison
        char* nl = buf;
        while (*nl && *nl != '\n') nl++;
        *nl = '\0';

        if (strcmp(buf, "quit") == 0 || strcmp(buf, "exit") == 0)
        {
            printf("Goodbye!\n");
            break;
        }

        printf("You typed: '%s'\n> ", buf);
    }

    printf("=== ECHO TEST COMPLETE ===\n");
    return 0;
}
