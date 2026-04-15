/* ansi_test.c — ANSI terminal escape sequence stress test for Brook OS.
 * Compile with: cc -o ansi_test ansi_test.c
 * Tests: colors, cursor movement, line editing, screen clear, rapid output.
 */
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#define ESC "\033"

void test_colors(void) {
    int c;
    printf("--- Colors ---\n");
    for (c = 30; c <= 37; c++)
        printf(ESC "[%dm FG%d " ESC "[0m", c, c);
    printf("\n");
    for (c = 40; c <= 47; c++)
        printf(ESC "[%dm BG%d " ESC "[0m", c, c);
    printf("\n");
    /* Bright colors */
    for (c = 90; c <= 97; c++)
        printf(ESC "[%dm HI%d " ESC "[0m", c, c);
    printf("\n\n");
}

void test_cursor(void) {
    printf("--- Cursor Movement ---\n");
    printf(ESC "[sPosition saved.");
    printf(ESC "[5C  (moved right 5)");
    printf(ESC "[u" ESC "[1B\nRestored + down.\n\n");
}

void test_erase(void) {
    printf("--- Erase Tests ---\n");
    printf("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX\n");
    printf(ESC "[A" ESC "[15G" ESC "[K");
    printf("(erased from col 15)\n");
    printf("Full line: XXXXXXXXXXXXXXXXXXXXXXX");
    printf("\r" ESC "[2K");
    printf("(line erased)\n\n");
}

void test_scroll(void) {
    int i;
    printf("--- Scroll Region ---\n");
    /* Set scroll region to lines 2-5 of remaining area */
    printf("Line 1 (fixed)\n");
    printf("Line 2\n");
    printf("Line 3\n");
    printf("Line 4\n");
    printf("Line 5\n");
    printf("Line 6 (fixed)\n\n");
}

void test_insert_delete(void) {
    printf("--- Insert/Delete ---\n");
    printf("ABCDEFGHIJ\n");
    printf(ESC "[A" ESC "[5G" ESC "[@" ESC "[@" ESC "[@");
    printf("___");
    printf(ESC "[B\n");
    printf("(inserted 3 blanks at col 5)\n\n");
}

void test_box(void) {
    int r, c;
    int w = 30, h = 8;
    printf("--- Box Drawing ---\n");
    /* Top border */
    printf("+");
    for (c = 0; c < w - 2; c++) printf("-");
    printf("+\n");
    /* Sides */
    for (r = 0; r < h - 2; r++) {
        printf("|");
        for (c = 0; c < w - 2; c++) printf(" ");
        printf("|\n");
    }
    /* Label in center */
    printf(ESC "[%dA" ESC "[%dG" ESC "[1;36mBrook OS" ESC "[0m",
           h/2, (w - 8) / 2 + 1);
    printf(ESC "[%dB\n", h/2);
    /* Bottom border */
    printf("+");
    for (c = 0; c < w - 2; c++) printf("-");
    printf("+\n\n");
}

void test_rapid(void) {
    int i;
    printf("--- Rapid Output ---\n");
    for (i = 0; i < 50; i++) {
        printf(ESC "[%dm*" ESC "[0m", 31 + (i % 7));
    }
    printf("\n(50 colored stars)\n\n");
}

int main(void) {
    printf(ESC "[2J" ESC "[H");  /* Clear screen, cursor home */
    printf("=== Brook ANSI Terminal Test ===\n\n");

    test_colors();
    test_cursor();
    test_erase();
    test_insert_delete();
    test_box();
    test_rapid();

    printf("=== All Tests Complete ===\n");
    return 0;
}
