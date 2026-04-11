#include <unistd.h>

int main(void) {
    const char msg[] = "Hello from Brook OS!\n";
    write(1, msg, sizeof(msg) - 1);
    return 0;
}
