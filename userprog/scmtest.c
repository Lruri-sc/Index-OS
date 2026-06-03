/* scmtest.c: SCM_RIGHTS file-descriptor passing over AF_UNIX socketpairs --
 * the mechanism OpenSSH's privsep monitor uses (mm_send_fd / mm_receive_fd).
 * Each test forks; the parent passes one or more descriptors to the child via
 * sendmsg(SCM_RIGHTS); the child uses only the *received* fds (it closes its
 * inherited copies first) and the parent verifies the data came back through
 * them -- proving each fd crossed the process boundary onto the same kernel
 * object, with the backend refcount surviving the send->receive handoff.
 *
 *   test1  single pipe fd
 *   test2  two pipe fds in one cmsg (exercises the multi-fd cmsg path)
 *   test3  a pty slave fd (what sshd actually hands a session) via SisterRelay
 *
 *   Index> scmtest        # -> "SCMTEST: ALL PASS"
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>

#define MAXFD 4

static int send_fds(int sock, const int *fds, int n) {
    char dummy = 'X';
    struct iovec iov = {&dummy, 1};
    char cbuf[CMSG_SPACE(MAXFD * sizeof(int))];
    memset(cbuf, 0, sizeof(cbuf));
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = CMSG_SPACE(n * sizeof(int));
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(n * sizeof(int));
    memcpy(CMSG_DATA(c), fds, n * sizeof(int));
    return (int)sendmsg(sock, &msg, 0);
}

static int recv_fds(int sock, int *out, int maxn) {
    char dummy = 0;
    struct iovec iov = {&dummy, 1};
    char cbuf[CMSG_SPACE(MAXFD * sizeof(int))];
    memset(cbuf, 0, sizeof(cbuf));
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);
    if (recvmsg(sock, &msg, 0) < 0) return -1;
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    if (c == NULL || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
        return 0;
    int n = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
    if (n > maxn) n = maxn;
    memcpy(out, CMSG_DATA(c), n * sizeof(int));
    return n;
}

static int test_single(void) {
    int sv[2], p[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0 || pipe(p) < 0) return 1;
    pid_t k = fork();
    if (k == 0) {
        close(sv[0]); close(p[0]); close(p[1]);
        int rfd;
        if (recv_fds(sv[1], &rfd, 1) != 1) _exit(1);
        ssize_t w = write(rfd, "MISAKA", 6);
        close(rfd);
        _exit(w == 6 ? 0 : 2);
    }
    close(sv[1]);
    send_fds(sv[0], &p[1], 1);
    close(p[1]);
    char buf[16];
    memset(buf, 0, sizeof(buf));
    ssize_t n = read(p[0], buf, sizeof(buf) - 1);
    waitpid(k, 0, 0);
    int ok = (n == 6 && memcmp(buf, "MISAKA", 6) == 0);
    printf("[test1 single-fd] %s  (read \"%s\")\n", ok ? "PASS" : "FAIL", buf);
    return ok ? 0 : 1;
}

static int test_multi(void) {
    int sv[2], pa[2], pb[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0 ||
        pipe(pa) < 0 || pipe(pb) < 0) return 1;
    pid_t k = fork();
    if (k == 0) {
        close(sv[0]);
        close(pa[0]); close(pa[1]); close(pb[0]); close(pb[1]);
        int rf[2];
        if (recv_fds(sv[1], rf, 2) != 2) _exit(1);
        write(rf[0], "AAA", 3);
        write(rf[1], "BBB", 3);
        close(rf[0]); close(rf[1]);
        _exit(0);
    }
    close(sv[1]);
    int two[2] = {pa[1], pb[1]};
    send_fds(sv[0], two, 2);
    close(pa[1]); close(pb[1]);
    char ba[8], bb[8];
    memset(ba, 0, sizeof(ba));
    memset(bb, 0, sizeof(bb));
    ssize_t na = read(pa[0], ba, 7);
    ssize_t nb = read(pb[0], bb, 7);
    waitpid(k, 0, 0);
    int ok = (na == 3 && memcmp(ba, "AAA", 3) == 0 &&
              nb == 3 && memcmp(bb, "BBB", 3) == 0);
    printf("[test2 multi-fd ] %s  (a=\"%s\" b=\"%s\")\n", ok ? "PASS" : "FAIL", ba, bb);
    return ok ? 0 : 1;
}

static int test_pty(void) {
    int master = open("/dev/ptmx", O_RDWR);
    if (master < 0) { printf("[test3 pty-fd   ] FAIL (open ptmx)\n"); return 1; }
    int n = -1;
    if (ioctl(master, 0x80045430 /*TIOCGPTN*/, &n) < 0) {
        printf("[test3 pty-fd   ] FAIL (TIOCGPTN)\n"); close(master); return 1;
    }
    int unlock = 0;
    ioctl(master, 0x40045431 /*TIOCSPTLCK*/, &unlock);
    char sp[32];
    snprintf(sp, sizeof sp, "/dev/pts/%d", n);
    int slave = open(sp, O_RDWR);
    if (slave < 0) { printf("[test3 pty-fd   ] FAIL (open slave)\n"); close(master); return 1; }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) return 1;
    pid_t k = fork();
    if (k == 0) {
        close(sv[0]); close(master); close(slave);
        int rs;
        if (recv_fds(sv[1], &rs, 1) != 1) _exit(1);
        write(rs, "PTYPASS", 7);
        close(rs);
        _exit(0);
    }
    close(sv[1]);
    send_fds(sv[0], &slave, 1); /* hand the pty slave to the child */
    close(slave);
    char buf[32];
    memset(buf, 0, sizeof(buf));
    ssize_t got = read(master, buf, sizeof(buf) - 1); /* child's write, via SisterRelay s2m */
    waitpid(k, 0, 0);
    close(master);
    int ok = (got >= 7 && memcmp(buf, "PTYPASS", 7) == 0);
    printf("[test3 pty-fd   ] %s  (master read \"%.*s\")\n",
           ok ? "PASS" : "FAIL", (int)(got > 0 ? got : 0), buf);
    return ok ? 0 : 1;
}

int main(void) {
    int rc = 0;
    rc |= test_single();
    rc |= test_multi();
    rc |= test_pty();
    printf(rc == 0 ? "SCMTEST: ALL PASS\n" : "SCMTEST: FAIL\n");
    return rc;
}
