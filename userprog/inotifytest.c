// Functional test for inotify ("Kazakiri"): watch /tmp, then create + write +
// delete a file there and confirm IN_CREATE / IN_MODIFY / IN_DELETE arrive with
// the right basename. Built static-musl, baked into the rootfs as /bin/inotifytest.
// Prints INOTIFY_OK on success so a boot harness can grep for it.
#include <sys/inotify.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

int main(void) {
    int ifd = inotify_init1(0);
    if (ifd < 0) { printf("INOTIFY_FAIL init\n"); return 1; }
    int wd = inotify_add_watch(ifd, "/tmp", IN_CREATE | IN_MODIFY | IN_DELETE);
    if (wd < 0) { printf("INOTIFY_FAIL add_watch\n"); return 1; }

    // Generate the three events on a file inside the watched directory.
    int f = open("/tmp/itest.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644); // IN_CREATE
    if (f < 0) { printf("INOTIFY_FAIL open\n"); return 1; }
    if (write(f, "hello", 5) != 5) { printf("INOTIFY_FAIL write\n"); return 1; } // IN_MODIFY
    close(f);
    unlink("/tmp/itest.txt"); // IN_DELETE

    // Drain the event ring; OR together the masks seen for "itest.txt".
    char buf[4096];
    uint32_t seen = 0;
    for (int iter = 0; iter < 20; ++iter) {
        struct pollfd p; p.fd = ifd; p.events = POLLIN; p.revents = 0;
        if (poll(&p, 1, 500) <= 0) break;
        ssize_t n = read(ifd, buf, sizeof(buf));
        if (n <= 0) break;
        for (char *q = buf; q < buf + n; ) {
            struct inotify_event *e = (struct inotify_event *)q;
            if (e->len == 0 || strcmp(e->name, "itest.txt") == 0) seen |= e->mask;
            q += sizeof(struct inotify_event) + e->len;
        }
        if ((seen & IN_CREATE) && (seen & IN_MODIFY) && (seen & IN_DELETE)) break;
    }

    printf("INOTIFY seen: CREATE=%d MODIFY=%d DELETE=%d\n",
           (seen & IN_CREATE) != 0, (seen & IN_MODIFY) != 0, (seen & IN_DELETE) != 0);
    if ((seen & IN_CREATE) && (seen & IN_MODIFY) && (seen & IN_DELETE))
        printf("INOTIFY_OK\n");
    else
        printf("INOTIFY_FAIL mask=0x%x\n", (unsigned)seen);
    return 0;
}
