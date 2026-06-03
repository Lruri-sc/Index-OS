/* tcpserver.c: a tiny TCP server. Binds to argv[1] (default 5557), listens,
 * accepts ONE connection, reads up to one buffer, echoes "echo:<input>" back,
 * closes. Validates Antenna's passive-TCP path (Phase Net-3) end to end.
 *
 *   # in arm-Index (run-qemu-ext2-net forwards host TCP 5557 -> guest 5557):
 *   Index> linuxrun /bin/tcpserver 5557
 *
 *   # on the host:
 *   python3 -c "import socket; s=socket.socket(); s.connect(('127.0.0.1',5557)); \
 *               s.sendall(b'hello'); print(s.recv(1024)); s.close()"
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv) {
    unsigned short port = 5557;
    if (argc > 1) port = (unsigned short)atoi(argv[1]);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return 1; }

    struct sockaddr_in me = {0};
    me.sin_family = AF_INET;
    me.sin_port = htons(port);
    me.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (struct sockaddr *)&me, sizeof(me)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(s, 4) < 0) { perror("listen"); return 1; }
    printf("tcpserver: listening on TCP :%u\n", port);

    struct sockaddr_in peer = {0};
    socklen_t plen = sizeof(peer);
    int c = accept(s, (struct sockaddr *)&peer, &plen);
    if (c < 0) { perror("accept"); return 1; }
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    printf("tcpserver: accepted from %s:%u\n", ip, ntohs(peer.sin_port));

    char buf[1500];
    ssize_t r = read(c, buf, sizeof(buf) - 1);
    if (r <= 0) { printf("tcpserver: read returned %zd\n", r); return 1; }
    buf[r] = 0;
    printf("tcpserver: got %zd bytes: %s\n", r, buf);

    char out[1600];
    int n = snprintf(out, sizeof(out), "echo:%s", buf);
    ssize_t w = write(c, out, n);
    if (w < 0) perror("write");
    else printf("tcpserver: echoed %zd bytes\n", w);

    close(c);
    close(s);
    return 0;
}
