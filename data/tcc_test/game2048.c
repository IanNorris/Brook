/* game2048.c — 2048 number game for Brook OS terminal.
 * Compile with: cc -o 2048 game2048.c
 * Controls: WASD or arrow keys, Q to quit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>

#define ESC "\033"
#define SIZE 4

static int board[SIZE][SIZE];
static int score = 0;
static struct termios orig_term;

static void raw_mode(void) {
    struct termios t;
    tcgetattr(0, &orig_term);
    t = orig_term;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
}

static void restore_mode(void) {
    tcsetattr(0, TCSANOW, &orig_term);
}

/* Color based on tile value */
static const char* tile_color(int val) {
    switch (val) {
    case 2:    return ESC "[47;30m";   /* white bg, black fg */
    case 4:    return ESC "[43;30m";   /* yellow bg */
    case 8:    return ESC "[46;30m";   /* cyan bg */
    case 16:   return ESC "[44;37m";   /* blue bg */
    case 32:   return ESC "[45;37m";   /* magenta bg */
    case 64:   return ESC "[41;37m";   /* red bg */
    case 128:  return ESC "[42;30m";   /* green bg */
    case 256:  return ESC "[43;37m";   /* bright yellow */
    case 512:  return ESC "[46;37m";   /* bright cyan */
    case 1024: return ESC "[44;33m";   /* blue/yellow */
    case 2048: return ESC "[41;33m";   /* red/yellow */
    default:   return ESC "[100;37m";  /* dark grey */
    }
}

static void add_random(void) {
    int empty[SIZE * SIZE], n = 0, i, j;
    for (i = 0; i < SIZE; i++)
        for (j = 0; j < SIZE; j++)
            if (board[i][j] == 0) empty[n++] = i * SIZE + j;
    if (n == 0) return;
    int pos = empty[rand() % n];
    board[pos / SIZE][pos % SIZE] = (rand() % 10 < 9) ? 2 : 4;
}

static void draw(void) {
    int i, j;
    printf(ESC "[H");
    printf(ESC "[1;36m  === 2048 ===" ESC "[0m\n");
    printf("  Score: %d\n\n", score);

    for (i = 0; i < SIZE; i++) {
        printf("  ");
        for (j = 0; j < SIZE; j++) {
            if (board[i][j] == 0)
                printf(ESC "[40m  .   " ESC "[0m");
            else
                printf("%s%5d " ESC "[0m", tile_color(board[i][j]), board[i][j]);
        }
        printf("\n\n");
    }
    printf("  WASD/Arrows = move, Q = quit\n");
}

static int slide_row(int row[SIZE]) {
    int i, j, moved = 0;
    /* Remove zeros */
    int tmp[SIZE] = {0};
    j = 0;
    for (i = 0; i < SIZE; i++)
        if (row[i] != 0) tmp[j++] = row[i];
    /* Merge adjacent */
    for (i = 0; i < SIZE - 1; i++) {
        if (tmp[i] != 0 && tmp[i] == tmp[i + 1]) {
            tmp[i] *= 2;
            score += tmp[i];
            tmp[i + 1] = 0;
        }
    }
    /* Remove zeros again */
    int out[SIZE] = {0};
    j = 0;
    for (i = 0; i < SIZE; i++)
        if (tmp[i] != 0) out[j++] = tmp[i];
    for (i = 0; i < SIZE; i++) {
        if (row[i] != out[i]) moved = 1;
        row[i] = out[i];
    }
    return moved;
}

static int move_left(void) {
    int i, moved = 0;
    for (i = 0; i < SIZE; i++)
        moved |= slide_row(board[i]);
    return moved;
}

static int move_right(void) {
    int i, j, moved = 0;
    for (i = 0; i < SIZE; i++) {
        int rev[SIZE];
        for (j = 0; j < SIZE; j++) rev[j] = board[i][SIZE - 1 - j];
        moved |= slide_row(rev);
        for (j = 0; j < SIZE; j++) board[i][SIZE - 1 - j] = rev[j];
    }
    return moved;
}

static int move_up(void) {
    int i, j, moved = 0;
    for (j = 0; j < SIZE; j++) {
        int col[SIZE];
        for (i = 0; i < SIZE; i++) col[i] = board[i][j];
        moved |= slide_row(col);
        for (i = 0; i < SIZE; i++) board[i][j] = col[i];
    }
    return moved;
}

static int move_down(void) {
    int i, j, moved = 0;
    for (j = 0; j < SIZE; j++) {
        int col[SIZE];
        for (i = 0; i < SIZE; i++) col[i] = board[SIZE - 1 - i][j];
        moved |= slide_row(col);
        for (i = 0; i < SIZE; i++) board[SIZE - 1 - i][j] = col[i];
    }
    return moved;
}

static int can_move(void) {
    int i, j;
    for (i = 0; i < SIZE; i++)
        for (j = 0; j < SIZE; j++) {
            if (board[i][j] == 0) return 1;
            if (j < SIZE - 1 && board[i][j] == board[i][j + 1]) return 1;
            if (i < SIZE - 1 && board[i][j] == board[i + 1][j]) return 1;
        }
    return 0;
}

int main(void) {
    char buf[8];
    int moved;

    srand(42);
    add_random();
    add_random();

    raw_mode();
    printf(ESC "[2J");
    printf(ESC "[?25l");

    while (1) {
        draw();

        if (!can_move()) {
            printf("\n  " ESC "[1;31mGame Over!" ESC "[0m\n");
            break;
        }

        int n = read(0, buf, sizeof(buf));
        if (n <= 0) continue;

        moved = 0;
        if (buf[0] == 'q' || buf[0] == 'Q') break;

        if (buf[0] == 'w' || buf[0] == 'W') moved = move_up();
        else if (buf[0] == 's' || buf[0] == 'S') moved = move_down();
        else if (buf[0] == 'a' || buf[0] == 'A') moved = move_left();
        else if (buf[0] == 'd' || buf[0] == 'D') moved = move_right();
        else if (n >= 3 && buf[0] == 0x1b && buf[1] == '[') {
            if (buf[2] == 'A') moved = move_up();
            else if (buf[2] == 'B') moved = move_down();
            else if (buf[2] == 'D') moved = move_left();
            else if (buf[2] == 'C') moved = move_right();
        }

        if (moved) add_random();
    }

    printf(ESC "[?25h");
    printf(ESC "[%d;1H", SIZE * 2 + 7);
    restore_mode();
    return 0;
}
