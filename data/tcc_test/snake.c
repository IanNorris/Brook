/* snake.c — Simple snake game for Brook OS terminal.
 * Compile with: cc -o snake snake.c
 * Controls: WASD or arrow keys, Q to quit.
 * Tests: non-canonical input, cursor positioning, colors, rapid screen update.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#define ESC "\033"
#define W 40
#define H 20
#define MAX_LEN 200

static int sx[MAX_LEN], sy[MAX_LEN];
static int slen = 3;
static int dx = 1, dy = 0;
static int fx, fy; /* food */
static int score = 0;
static int running = 1;

static struct termios orig_term;

static void raw_mode(void) {
    struct termios t;
    tcgetattr(0, &orig_term);
    t = orig_term;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 1; /* 100ms timeout */
    tcsetattr(0, TCSANOW, &t);
}

static void restore_mode(void) {
    tcsetattr(0, TCSANOW, &orig_term);
}

static void place_food(void) {
    int i, ok;
    do {
        fx = 1 + (rand() % (W - 2));
        fy = 1 + (rand() % (H - 2));
        ok = 1;
        for (i = 0; i < slen; i++)
            if (sx[i] == fx && sy[i] == fy) { ok = 0; break; }
    } while (!ok);
}

static void draw(void) {
    int x, y, i;
    printf(ESC "[H");  /* cursor home */
    /* Top border */
    printf(ESC "[33m");
    for (x = 0; x < W; x++) printf("#");
    printf(ESC "[0m\n");
    /* Field */
    for (y = 1; y < H - 1; y++) {
        printf(ESC "[33m#" ESC "[0m");
        for (x = 1; x < W - 1; x++) {
            int drawn = 0;
            /* Check snake */
            for (i = 0; i < slen; i++) {
                if (sx[i] == x && sy[i] == y) {
                    if (i == 0)
                        printf(ESC "[1;32m@" ESC "[0m"); /* head */
                    else
                        printf(ESC "[32mo" ESC "[0m"); /* body */
                    drawn = 1;
                    break;
                }
            }
            if (!drawn && x == fx && y == fy)
                { printf(ESC "[1;31m*" ESC "[0m"); drawn = 1; }
            if (!drawn)
                printf(" ");
        }
        printf(ESC "[33m#" ESC "[0m\n");
    }
    /* Bottom border */
    printf(ESC "[33m");
    for (x = 0; x < W; x++) printf("#");
    printf(ESC "[0m\n");
    printf("Score: %d  |  WASD/Arrows=move  Q=quit\n", score);
}

static void update(void) {
    int i;
    int nx = sx[0] + dx;
    int ny = sy[0] + dy;

    /* Wall collision */
    if (nx <= 0 || nx >= W - 1 || ny <= 0 || ny >= H - 1) {
        running = 0;
        return;
    }
    /* Self collision */
    for (i = 0; i < slen; i++)
        if (sx[i] == nx && sy[i] == ny) { running = 0; return; }

    /* Move body */
    if (nx == fx && ny == fy) {
        if (slen < MAX_LEN) slen++;
        score += 10;
        place_food();
    }
    for (i = slen - 1; i > 0; i--) {
        sx[i] = sx[i - 1];
        sy[i] = sy[i - 1];
    }
    sx[0] = nx;
    sy[0] = ny;
}

int main(void) {
    int i;
    char buf[8];

    srand(42);

    /* Init snake in center */
    for (i = 0; i < slen; i++) {
        sx[i] = W / 2 - i;
        sy[i] = H / 2;
    }
    place_food();

    raw_mode();
    printf(ESC "[2J"); /* clear screen */
    printf(ESC "[?25l"); /* hide cursor */

    while (running) {
        draw();
        usleep(120000); /* 120ms tick */

        /* Read input (non-blocking via VTIME) */
        int n = read(0, buf, sizeof(buf));
        if (n > 0) {
            if (buf[0] == 'q' || buf[0] == 'Q') break;
            if (buf[0] == 'w' || buf[0] == 'W') { if (dy != 1)  { dx = 0; dy = -1; } }
            if (buf[0] == 's' || buf[0] == 'S') { if (dy != -1) { dx = 0; dy = 1;  } }
            if (buf[0] == 'a' || buf[0] == 'A') { if (dx != 1)  { dx = -1; dy = 0; } }
            if (buf[0] == 'd' || buf[0] == 'D') { if (dx != -1) { dx = 1; dy = 0;  } }
            /* Arrow key sequences: ESC [ A/B/C/D */
            if (n >= 3 && buf[0] == 0x1b && buf[1] == '[') {
                if (buf[2] == 'A' && dy != 1)  { dx = 0; dy = -1; }
                if (buf[2] == 'B' && dy != -1) { dx = 0; dy = 1;  }
                if (buf[2] == 'D' && dx != 1)  { dx = -1; dy = 0; }
                if (buf[2] == 'C' && dx != -1) { dx = 1; dy = 0;  }
            }
        }
        update();
    }

    printf(ESC "[?25h"); /* show cursor */
    printf(ESC "[%d;1H", H + 2);
    printf("Game Over! Score: %d\n", score);
    restore_mode();
    return 0;
}
