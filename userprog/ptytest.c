// pty smoke test: allocate a pty pair, fork sh on the slave end, and read
// whatever sh prints back through the master.  Expected output:
//
//   got: hello-from-pty
//
// Exercises: /dev/ptmx, TIOCGPTN, TIOCSPTLCK, /dev/pts/N, fork, setsid,
// dup2, execl on the slave; SisterRelay master->slave + slave->master rings.

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    int master = open("/dev/ptmx", O_RDWR);
    if (master < 0) { write(1, "open /dev/ptmx FAIL\n", 20); return 1; }

    int n = -1;
    if (ioctl(master, 0x80045430 /*TIOCGPTN*/, &n) < 0) {
        write(1, "TIOCGPTN FAIL\n", 14); return 1;
    }
    int unlock = 0;
    ioctl(master, 0x40045431 /*TIOCSPTLCK*/, &unlock);

    char slave_path[32];
    snprintf(slave_path, sizeof slave_path, "/dev/pts/%d", n);

    pid_t pid = fork();
    if (pid < 0) { write(1, "fork FAIL\n", 10); return 1; }
    if (pid == 0) {
        // child: switch to a new session, attach slave as stdin/stdout/stderr,
        // and exec a one-shot command on it.
        setsid();
        int slave = open(slave_path, O_RDWR);
        if (slave < 0) { write(1, "child open slave FAIL\n", 22); _exit(1); }
        close(master);
        dup2(slave, 0); dup2(slave, 1); dup2(slave, 2);
        if (slave > 2) close(slave);
        execl("/bin/sh", "sh", "-c", "echo hello-from-pty", (char *)NULL);
        _exit(127);
    }

    // parent: drain whatever the child writes onto the slave until it exits.
    char buf[256];
    ssize_t got = 0;
    // sh prints "hello-from-pty\n" then exits; read once should be enough.
    got = read(master, buf, sizeof buf - 1);
    write(1, "got: ", 5);
    if (got > 0) write(1, buf, got);
    write(1, "\n", 1);

    close(master);
    int status = 0;
    waitpid(pid, &status, 0);
    return 0;
}
