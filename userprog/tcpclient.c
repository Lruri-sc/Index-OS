/* tcpclient.c: a tiny TCP client. Connects to argv[1]:argv[2], sends
 * argv[3] (or a default), reads up to one buffer of response, and exits.
 * Validates Antenna's TCP path end to end from a real musl Linux binary:
 *
 *   # on the host, run a tiny TCP echo:
 *   python3 -c "import socket; s=socket.socket(); s.bind(('127.0.0.1',9999)); \
 *               s.listen(); c,_=s.accept(); d=c.recv(1024); \
 *               c.sendall(b'echo:'+d); c.close()" &
 *
 *   # in arm-Index:
 *   Index> linuxrun /bin/tcpclient 10.0.2.2 9999 hello-from-musl
 *
 * SLIRP routes 10.0.2.2 (the gateway address) to the host's localhost.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <ip> <port> [payload]\n", argv[0]);
        return 1;
    }
    const char *ipstr = argv[1];
    unsigned short port = (unsigned short)atoi(argv[2]);
    const char *payload = (argc > 3) ? argv[3] : "hello-from-arm-Index";
    size_t plen = strlen(payload);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return 1; }

    struct sockaddr_in peer = {0};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(port);
    if (inet_pton(AF_INET, ipstr, &peer.sin_addr) != 1) {
        fprintf(stderr, "tcpclient: bad ip %s\n", ipstr);
        return 1;
    }

    printf("tcpclient: connecting to %s:%u ...\n", ipstr, port);
    if (connect(s, (struct sockaddr *)&peer, sizeof(peer)) < 0) {
        perror("connect"); return 1;
    }
    printf("tcpclient: connected.\n");

    ssize_t w = write(s, payload, plen);
    if (w < 0) { perror("write"); return 1; }
    printf("tcpclient: sent %zd bytes\n", w);

    char buf[1500];
    ssize_t r = read(s, buf, sizeof(buf) - 1);
    if (r < 0) { perror("read"); return 1; }
    if (r == 0) {
        printf("tcpclient: peer closed without data.\n");
    } else {
        buf[r] = 0;
        printf("tcpclient: received %zd bytes: %s\n", r, buf);
    }
    close(s);
    return 0;
}
