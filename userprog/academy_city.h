#pragma once

// AcademyCity (学園都市): the home of every Esper. In the kernel an Esper is
// a user-space process; here AcademyCity is the home library that every Esper
// program links against -- the tiny freestanding "libc" that gives string,
// IO, and printf helpers built on top of usys.h's raw syscall wrappers. v1
// intentionally has no malloc (callers stay on the stack) so it fits inside
// the current 7-code-page Esper budget.

#include "usys.h"

// --- string helpers --------------------------------------------------------

long ac_strlen(const char *s);
int ac_strcmp(const char *a, const char *b);
bool ac_starts_with(const char *s, const char *prefix);
void *ac_memcpy(void *dst, const void *src, long n);
void *ac_memset(void *dst, int c, long n);

// Parse an unsigned decimal at *p; consumes digits and stops. Returns the
// value, or -1 if there were no digits.
long ac_parse_uint(const char *p);

// --- raw IO ----------------------------------------------------------------

void ac_putc(char c);
void ac_puts(const char *s);                   // write s, no trailing newline
void ac_putln(const char *s);                  // write s + '\n'
void ac_putn(long n);                          // print signed decimal
void ac_putx(unsigned long n);                 // print unsigned hex (no 0x)
long ac_getln(char *buf, long cap);            // read a line from fd 0

// --- formatted print -------------------------------------------------------

// Supports %d (signed decimal), %u (unsigned decimal), %x (lowercase hex),
// %s (string), %c (char), and %% literal. Width/precision are ignored. Writes
// to fd 1. Returns the number of bytes written, or -1 on syscall error.
long ac_printf(const char *fmt, ...);
