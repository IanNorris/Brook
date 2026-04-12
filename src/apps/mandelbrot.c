/* mandelbrot.c — ASCII Mandelbrot set renderer for Brook OS */
#include <stdio.h>
#include <time.h>

#define WIDTH  78
#define HEIGHT 40
#define MAX_ITER 80

static const char palette[] = " .:-=+*#%@";

int main(void) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    double xmin = -2.0, xmax = 0.8, ymin = -1.2, ymax = 1.2;
    double dx = (xmax - xmin) / WIDTH;
    double dy = (ymax - ymin) / HEIGHT;

    for (int row = 0; row < HEIGHT; row++) {
        double ci = ymin + row * dy;
        for (int col = 0; col < WIDTH; col++) {
            double cr = xmin + col * dx;
            double zr = 0, zi = 0;
            int iter = 0;
            while (zr * zr + zi * zi < 4.0 && iter < MAX_ITER) {
                double tmp = zr * zr - zi * zi + cr;
                zi = 2.0 * zr * zi + ci;
                zr = tmp;
                iter++;
            }
            int idx = iter * (sizeof(palette) - 2) / MAX_ITER;
            putchar(palette[idx]);
        }
        putchar('\n');
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    printf("\nMandelbrot %dx%d rendered in %ld ms\n", WIDTH, HEIGHT, ms);
    return 0;
}
