// wc: read stdin to EOF, print byte/word/line counts. The point isn't the
// counter; the point is that wc only ever talks to fd 0, so any way the data
// arrives -- `WC.ELF < HELLO.TXT` (Aiwass-less `<` redirect) or
// `INIT.ELF | WC.ELF` (Aiwass pipe) -- exercises a different end of the
// pipe/redirect machinery and lands here as bytes.

#include "academy_city.h"

extern "C" [[noreturn]] void _start() {
    long bytes = 0;
    long lines = 0;
    long words = 0;
    bool in_word = false;
    char buf[128];
    for (;;) {
        const long n = sys_read(0, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        for (long i = 0; i < n; ++i) {
            const char c = buf[i];
            ++bytes;
            if (c == '\n') {
                ++lines;
            }
            const bool ws = (c == ' ' || c == '\t' || c == '\n');
            if (!ws && !in_word) {
                ++words;
                in_word = true;
            } else if (ws) {
                in_word = false;
            }
        }
    }
    ac_printf("wc: bytes=%d words=%d lines=%d\n", bytes, words, lines);
    sys_exit(0);
}
