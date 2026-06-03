/* ttytest.c: exercise the TTY line discipline. First confirms isatty(0), then
 * switches the console to raw mode (no echo, byte-at-a-time) and echoes a few
 * keypresses back uppercased -- proving cooked/raw switching via termios works.
 * Also installs a SIGINT handler so Ctrl-C runs the handler instead of killing
 * the program. Type 5 keys; Ctrl-C is reported and counts as one of them. */

#include <stdio.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include <ctype.h>

static volatile int got_sigint = 0;
static void on_int(int s) { (void)s; got_sigint = 1; }

int main(void) {
    printf("isatty(0)=%d\n", isatty(0));

    signal(SIGINT, on_int);

    struct termios old, raw;
    tcgetattr(0, &old);
    raw = old;
    raw.c_lflag &= ~(ICANON | ECHO); /* raw: no line editing, no echo */
    tcsetattr(0, TCSANOW, &raw);

    printf("raw mode: type 5 keys (Ctrl-C ok)\n");
    for (int i = 0; i < 5; i++) {
        char c;
        long n = read(0, &c, 1);
        if (n == 1) {
            printf("  key %d: '%c' (0x%02x)\n", i, isprint((unsigned char)c) ? c : '?', (unsigned char)c);
        } else if (got_sigint) {
            printf("  key %d: <SIGINT caught>\n", i);
            got_sigint = 0;
        }
    }

    tcsetattr(0, TCSANOW, &old); /* restore cooked mode */
    printf("restored cooked mode; done\n");
    return 0;
}
