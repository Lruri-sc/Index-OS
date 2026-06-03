// AcademyCity: the home library every Esper program links against. See
// academy_city.h for the contract. v1 stays freestanding -- no globals
// (a static char[] heap would need runtime relocations our PIE loader
// doesn't apply), no malloc, just stack-bound helpers built on syscalls.

#include "academy_city.h"

// __builtin_va_* is provided by the compiler with no header in freestanding
// mode; using these intrinsics avoids needing a <stdarg.h> we don't have.

// --- string helpers --------------------------------------------------------

long ac_strlen(const char *s) {
    long n = 0;
    while (s != nullptr && s[n] != 0) {
        ++n;
    }
    return n;
}

int ac_strcmp(const char *a, const char *b) {
    while (*a != 0 && *a == *b) {
        ++a;
        ++b;
    }
    return static_cast<int>(static_cast<unsigned char>(*a)) -
           static_cast<int>(static_cast<unsigned char>(*b));
}

bool ac_starts_with(const char *s, const char *prefix) {
    while (*prefix != 0) {
        if (*s++ != *prefix++) {
            return false;
        }
    }
    return true;
}

void *ac_memcpy(void *dst, const void *src, long n) {
    auto *d = static_cast<unsigned char *>(dst);
    const auto *s = static_cast<const unsigned char *>(src);
    for (long i = 0; i < n; ++i) {
        d[i] = s[i];
    }
    return dst;
}

void *ac_memset(void *dst, int c, long n) {
    auto *d = static_cast<unsigned char *>(dst);
    const auto fill = static_cast<unsigned char>(c);
    for (long i = 0; i < n; ++i) {
        d[i] = fill;
    }
    return dst;
}

long ac_parse_uint(const char *p) {
    if (p == nullptr || *p < '0' || *p > '9') {
        return -1;
    }
    long v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        ++p;
    }
    return v;
}

// --- raw IO ----------------------------------------------------------------

void ac_putc(char c) {
    // Route through fd 1 so redirection (Komoe's `>` and `|`) catches it.
    sys_write(1, &c, 1);
}

void ac_puts(const char *s) {
    if (s == nullptr) {
        return;
    }
    sys_write(1, s, ac_strlen(s));
}

void ac_putln(const char *s) {
    ac_puts(s);
    ac_putc('\n');
}

void ac_putn(long v) {
    if (v < 0) {
        ac_putc('-');
        v = -v;
    }
    if (v == 0) {
        ac_putc('0');
        return;
    }
    char buf[24];
    int i = 0;
    while (v > 0) {
        buf[i++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    }
    while (i > 0) {
        ac_putc(buf[--i]);
    }
}

void ac_putx(unsigned long v) {
    if (v == 0) {
        ac_putc('0');
        return;
    }
    char buf[24];
    int i = 0;
    while (v > 0) {
        const unsigned long nibble = v & 0xf;
        buf[i++] = static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
        v >>= 4;
    }
    while (i > 0) {
        ac_putc(buf[--i]);
    }
}

long ac_getln(char *buf, long cap) {
    return sys_read(0, buf, cap);
}

// --- formatted print -------------------------------------------------------

namespace {

// Append one character to a small buffer; flush via sys_write when full to
// keep the syscall count down (one putc per byte adds up under big formats).
struct LineBuf {
    char data[128];
    long fill = 0;
    long total = 0;

    void flush() {
        if (fill > 0) {
            const long n = sys_write(1, data, fill);
            if (n > 0) {
                total += n;
            }
            fill = 0;
        }
    }

    void put(char c) {
        if (fill == sizeof(data)) {
            flush();
        }
        data[fill++] = c;
    }

    void put_str(const char *s) {
        if (s == nullptr) {
            s = "(null)";
        }
        while (*s) {
            put(*s++);
        }
    }

    void put_dec(long v) {
        if (v < 0) {
            put('-');
            v = -v;
        }
        if (v == 0) {
            put('0');
            return;
        }
        char tmp[24];
        int i = 0;
        while (v > 0) {
            tmp[i++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        }
        while (i > 0) {
            put(tmp[--i]);
        }
    }

    void put_udec(unsigned long v) {
        if (v == 0) {
            put('0');
            return;
        }
        char tmp[24];
        int i = 0;
        while (v > 0) {
            tmp[i++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        }
        while (i > 0) {
            put(tmp[--i]);
        }
    }

    void put_hex(unsigned long v) {
        if (v == 0) {
            put('0');
            return;
        }
        char tmp[24];
        int i = 0;
        while (v > 0) {
            const unsigned long nibble = v & 0xf;
            tmp[i++] = static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
            v >>= 4;
        }
        while (i > 0) {
            put(tmp[--i]);
        }
    }
};

} // namespace

long ac_printf(const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    LineBuf out;
    for (const char *p = fmt; *p; ++p) {
        if (*p != '%') {
            out.put(*p);
            continue;
        }
        ++p;
        switch (*p) {
        case 'd':
            out.put_dec(__builtin_va_arg(ap, long));
            break;
        case 'u':
            out.put_udec(__builtin_va_arg(ap, unsigned long));
            break;
        case 'x':
            out.put_hex(__builtin_va_arg(ap, unsigned long));
            break;
        case 's':
            out.put_str(__builtin_va_arg(ap, const char *));
            break;
        case 'c':
            out.put(static_cast<char>(__builtin_va_arg(ap, int)));
            break;
        case '%':
            out.put('%');
            break;
        case 0:
            // Trailing '%' with nothing after it; emit the literal and stop.
            out.put('%');
            --p;
            break;
        default:
            // Unknown specifier: print as-is so the user notices.
            out.put('%');
            out.put(*p);
            break;
        }
    }
    out.flush();
    __builtin_va_end(ap);
    return out.total;
}
