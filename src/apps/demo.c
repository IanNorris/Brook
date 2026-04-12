// demo.c — Brook OS demo: displays system info and cowsay
#include <stdio.h>
#include <string.h>

static void cowsay(const char* msg)
{
    int len = (int)strlen(msg);
    int boxw = len + 2;

    printf(" ");
    for (int i = 0; i < boxw; i++) putchar('_');
    printf("\n< %s >\n ", msg);
    for (int i = 0; i < boxw; i++) putchar('-');
    printf("\n");
    printf("        \\   ^__^\n");
    printf("         \\  (oo)\\_______\n");
    printf("            (__)\\       )\\/\\\n");
    printf("                ||----w |\n");
    printf("                ||     ||\n");
}

static void mandelbrot(void)
{
    const int W = 60, H = 16;
    const double xmin = -2.0, xmax = 0.7, ymin = -1.0, ymax = 1.0;
    const char* chars = " .-:=+*#%@";
    int nchars = 10;

    for (int row = 0; row < H; row++) {
        for (int col = 0; col < W; col++) {
            double cr = xmin + (xmax - xmin) * col / W;
            double ci = ymin + (ymax - ymin) * row / H;
            double zr = 0, zi = 0;
            int iter = 0;
            while (zr*zr + zi*zi < 4.0 && iter < 50) {
                double tmp = zr*zr - zi*zi + cr;
                zi = 2*zr*zi + ci;
                zr = tmp;
                iter++;
            }
            putchar(chars[iter % nchars]);
        }
        putchar('\n');
    }
}

// Simple Fibonacci
static long long fib(int n)
{
    long long a = 0, b = 1;
    for (int i = 0; i < n; i++) {
        long long t = a + b;
        a = b;
        b = t;
    }
    return a;
}

int main(void)
{
    // Clear screen (ANSI ESC[2J + ESC[H)
    printf("\033[2J\033[H");
    printf("\n");
    printf("  ____                   _       ___  ____  \n");
    printf(" | __ ) _ __ ___   ___ | | __  / _ \\/ ___| \n");
    printf(" |  _ \\| '__/ _ \\ / _ \\| |/ / | | | \\___ \\ \n");
    printf(" | |_) | | | (_) | (_) |   <  | |_| |___) |\n");
    printf(" |____/|_|  \\___/ \\___/|_|\\_\\  \\___/|____/ \n");
    printf("\n");
    printf("  x86-64 UEFI hobby kernel  |  SMP  |  VirtIO  |  Linux syscalls\n");
    printf("\n");

    // Fibonacci
    printf("  fib(45) = %lld\n", fib(45));
    printf("\n");

    // Mandelbrot
    mandelbrot();
    printf("\n");

    // Cowsay
    cowsay("Brook OS lives!");

    printf("\n");
    return 0;
}
