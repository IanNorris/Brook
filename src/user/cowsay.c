#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[]) {
    const char *msg = argc > 1 ? argv[1] : "Moo!";
    int len = (int)strlen(msg);
    int boxw = len + 2;

    printf(" ");
    for (int i = 0; i < boxw; i++) putchar('_');
    printf("\n< %s >\n ", msg);
    for (int i = 0; i < boxw; i++) putchar('-');
    printf("\n        \\   ^__^\n         \\  (oo)\\_______\n            (__)\\       )\\/\\\n                ||----w |\n                ||     ||\n");
    return 0;
}
