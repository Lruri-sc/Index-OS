// Komoe (月詠小萌): a tiny shell that runs entirely at EL0, the user-space
// counterpart to the kernel's Necessarius. Like the teacher who manages her
// students, it spawns and oversees child processes: reads a command line,
// then either prints a file (cat -- builtin) or fork+exec+wait's a disk
// program. Now also pipes them together with `cmd1 | cmd2` (via the kernel's
// Aiwass pipe) and redirects input with `cmd < file`. Built only on system
// calls.

#include "usys.h"

namespace {

constexpr unsigned long kMaxWords = 8;
constexpr unsigned long kProgCap = 64;
constexpr unsigned long kPathCap = 24;

struct CmdSpec {
    char prog[kProgCap];
    char in_file[kPathCap];
    char out_file[kPathCap];
};

void ustrcpy_n(char *dst, const char *src, unsigned long cap) {
    unsigned long i = 0;
    while (src != nullptr && i + 1 < cap && src[i]) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

void spec_clear(CmdSpec *s) {
    s->prog[0] = 0;
    s->in_file[0] = 0;
    s->out_file[0] = 0;
}

// Cut `seg` on spaces, in place. Returns number of word pointers stored.
unsigned long tokenize(char *seg, char *words[kMaxWords]) {
    unsigned long n = 0;
    char *p = seg;
    while (*p && n < kMaxWords) {
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (!*p) {
            break;
        }
        words[n++] = p;
        while (*p && *p != ' ' && *p != '\t') {
            ++p;
        }
        if (*p) {
            *p++ = 0;
        }
    }
    return n;
}

// Parse one pipeline segment: optional `< file`, optional `> file`, and the
// program name. Sticks the file targets in *spec. Returns false if the segment
// has no program word or a stray `<`/`>` with no target.
bool parse_spec(char *seg, CmdSpec *spec) {
    spec_clear(spec);
    char *words[kMaxWords];
    const unsigned long n = tokenize(seg, words);
    for (unsigned long i = 0; i < n; ++i) {
        char *w = words[i];
        if (w[0] == '<') {
            const char *src = w[1] ? &w[1] : (i + 1 < n ? words[++i] : nullptr);
            if (src == nullptr) {
                return false;
            }
            ustrcpy_n(spec->in_file, src, sizeof(spec->in_file));
        } else if (w[0] == '>') {
            const char *src = w[1] ? &w[1] : (i + 1 < n ? words[++i] : nullptr);
            if (src == nullptr) {
                return false;
            }
            ustrcpy_n(spec->out_file, src, sizeof(spec->out_file));
        } else if (!spec->prog[0]) {
            ustrcpy_n(spec->prog, w, sizeof(spec->prog));
        }
        // Extra non-redirect words are ignored (no argv plumbing yet).
    }
    return spec->prog[0] != 0;
}

// Apply redirect bookkeeping inside a freshly-forked child, *before* exec.
// Exits the child if a requested redirect cannot be honoured.
[[noreturn]] void child_failed(const char *why, const char *what) {
    uputs("komoe: ");
    uputs(why);
    uputs(": ");
    uputs(what);
    uputs("\n");
    sys_exit(127);
}

void apply_redirects(const CmdSpec &spec) {
    if (spec.in_file[0]) {
        const long ifd = sys_open(spec.in_file);
        if (ifd < 0) {
            child_failed("cannot open input", spec.in_file);
        }
        sys_dup2(ifd, 0);
        sys_close(ifd);
    }
    if (spec.out_file[0]) {
        // No write-capable FS yet (FAT is read-only); fail loudly so it's
        // obvious why output didn't land instead of silently dropping it.
        child_failed("> redirect needs writable FS", spec.out_file);
    }
}

// Plain (non-pipeline) command, possibly with redirects. Keeps the existing
// fork+exec+wait pattern so the non-redirect case has no extra ceremony.
void do_cat(const char *name) {
    const long fd = sys_open(name);
    if (fd < 0) {
        uputs("cat: no such file: ");
        uputs(name);
        uputs("\n");
        return;
    }
    char buf[128];
    for (;;) {
        const long n = sys_read(fd, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        sys_write(1, buf, n);
    }
    sys_close(fd);
}

void run_single(const CmdSpec &spec) {
    const long pid = sys_fork();
    if (pid < 0) {
        uputs("komoe: fork failed\n");
        return;
    }
    if (pid == 0) {
        apply_redirects(spec);
        sys_exec(spec.prog);
        child_failed("cannot exec", spec.prog);
    }
    long status = -1;
    const long reaped = sys_wait(&status);
    uputs("[komoe] pid ");
    uputdec(reaped);
    uputs(" exited code ");
    uputdec(status);
    uputs("\n");
}

// Background: fork+exec like run_single, but don't wait. Print the pid so
// the user can `kill <pid>` it later. The zombie sits exited until something
// reaps it (or the process table recycles its slot for the next fork).
void run_background(const CmdSpec &spec) {
    const long pid = sys_fork();
    if (pid < 0) {
        uputs("komoe: fork failed\n");
        return;
    }
    if (pid == 0) {
        apply_redirects(spec);
        sys_exec(spec.prog);
        child_failed("cannot exec", spec.prog);
    }
    uputs("[komoe] background pid ");
    uputdec(pid);
    uputs("\n");
}

// Parse an unsigned decimal at *p (stops at first non-digit). Returns the
// number, or -1 if there were no digits.
long parse_uint(const char *p) {
    if (*p < '0' || *p > '9') {
        return -1;
    }
    long v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        ++p;
    }
    return v;
}

// `left | right`: Aiwass pipe between two children. Parent holds neither end
// after the children take their copies, so when both children close the pipe
// drains (broken pipe / EOF flows correctly).
void run_pipeline(const CmdSpec &left, const CmdSpec &right) {
    int fds[2];
    if (sys_pipe(fds) < 0) {
        uputs("komoe: pipe failed\n");
        return;
    }
    const long lpid = sys_fork();
    if (lpid < 0) {
        uputs("komoe: fork failed (left)\n");
        sys_close(fds[0]);
        sys_close(fds[1]);
        return;
    }
    if (lpid == 0) {
        // Left child: stdout -> pipe write end.
        sys_dup2(fds[1], 1);
        sys_close(fds[0]);
        sys_close(fds[1]);
        apply_redirects(left);
        sys_exec(left.prog);
        child_failed("cannot exec", left.prog);
    }
    const long rpid = sys_fork();
    if (rpid < 0) {
        uputs("komoe: fork failed (right)\n");
        sys_close(fds[0]);
        sys_close(fds[1]);
        sys_wait(nullptr);
        return;
    }
    if (rpid == 0) {
        // Right child: stdin -> pipe read end.
        sys_dup2(fds[0], 0);
        sys_close(fds[0]);
        sys_close(fds[1]);
        apply_redirects(right);
        sys_exec(right.prog);
        child_failed("cannot exec", right.prog);
    }
    // Parent drops both ends so the kernel pipe lives only as long as the
    // children's copies; EOF flows when the left child exits.
    sys_close(fds[0]);
    sys_close(fds[1]);
    long s1 = -1, s2 = -1;
    const long r1 = sys_wait(&s1);
    const long r2 = sys_wait(&s2);
    uputs("[komoe] pipe done: pid ");
    uputdec(r1);
    uputs(" code ");
    uputdec(s1);
    uputs(", pid ");
    uputdec(r2);
    uputs(" code ");
    uputdec(s2);
    uputs("\n");
}

} // namespace

extern "C" [[noreturn]] void _start() {
    uputs("Komoe: an EL0 shell. commands: <prog> [< file] [| <prog>] [&] | cat <file> | kill <pid> | exit\n");

    char line[128];
    for (;;) {
        uputs("komoe$ ");
        const long n = sys_read(0, line, sizeof(line) - 1);
        if (n < 0) {
            continue;
        }
        line[n] = 0;

        char *cmd = line;
        while (*cmd == ' ') {
            ++cmd;
        }
        if (*cmd == 0) {
            continue;
        }
        if (ustreq(cmd, "exit")) {
            uputs("Komoe: bye.\n");
            sys_exit(0);
        }
        if (ustarts(cmd, "cat ")) {
            do_cat(cmd + 4);
            continue;
        }
        if (ustarts(cmd, "kill ")) {
            const char *p = cmd + 5;
            while (*p == ' ') {
                ++p;
            }
            const long pid = parse_uint(p);
            if (pid <= 0) {
                uputs("komoe: bad pid for kill\n");
                continue;
            }
            if (sys_kill(pid, SIGTERM) < 0) {
                uputs("komoe: no such pid: ");
                uputdec(pid);
                uputs("\n");
            } else {
                uputs("[komoe] SIGTERM -> pid ");
                uputdec(pid);
                uputs("\n");
            }
            continue;
        }

        // Trailing `&` means run in the background (no wait).
        bool background = false;
        long line_len = ustrlen(cmd);
        while (line_len > 0 && cmd[line_len - 1] == ' ') {
            cmd[--line_len] = 0;
        }
        if (line_len > 0 && cmd[line_len - 1] == '&') {
            background = true;
            cmd[--line_len] = 0;
            while (line_len > 0 && cmd[line_len - 1] == ' ') {
                cmd[--line_len] = 0;
            }
        }

        // Find first '|' (no quoting). Pipeline iff present.
        char *bar = cmd;
        while (*bar && *bar != '|') {
            ++bar;
        }
        if (*bar == '|') {
            if (background) {
                uputs("komoe: `&` with pipelines not supported yet\n");
                continue;
            }
            *bar = 0;
            CmdSpec left, right;
            if (!parse_spec(cmd, &left)) {
                uputs("komoe: parse error before '|'\n");
                continue;
            }
            if (!parse_spec(bar + 1, &right)) {
                uputs("komoe: parse error after '|'\n");
                continue;
            }
            run_pipeline(left, right);
        } else {
            CmdSpec spec;
            if (!parse_spec(cmd, &spec)) {
                uputs("komoe: parse error\n");
                continue;
            }
            if (background) {
                run_background(spec);
            } else {
                run_single(spec);
            }
        }
    }
}
