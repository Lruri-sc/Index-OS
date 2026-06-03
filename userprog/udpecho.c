/* udpecho.c: a tiny UDP echo program. Opens an IPv4 datagram socket, binds
 * to a port (argv[1] or 5556), waits for one packet, echoes the payload back
 * to its sender, then exits. Validates the Antenna socket layer end-to-end
 * from a real musl Linux binary running on arm-Index. From the host:
 *
 *   make run-qemu-ext2-net           # forwards host UDP 5556 -> guest 5556
 *   Index> linuxrun /bin/udpecho 5556
 *   $ echo hello | nc -u -w 1 127.0.0.1 5556    # on the host
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv) {
    unsigned short port = 5556;
    if (argc > 1) port = (unsigned short)atoi(argv[1]);

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("socket"); return 1; }

    struct sockaddr_in me = {0};
    me.sin_family = AF_INET;
    me.sin_port = htons(port);
    me.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (struct sockaddr *)&me, sizeof(me)) < 0) {
        perror("bind"); return 1;
    }

    printf("udpecho: listening on UDP :%u\n", port);

    char buf[1500];
    struct sockaddr_in peer = {0};
    socklen_t plen = sizeof(peer);
    ssize_t n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&peer, &plen);
    if (n <= 0) {
        printf("udpecho: recvfrom returned %ld\n", (long)n);
        return 1;
    }
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    printf("udpecho: %zd bytes from %s:%u\n", n, ip, ntohs(peer.sin_port));

    ssize_t w = sendto(s, buf, n, 0, (struct sockaddr *)&peer, plen);
    if (w < 0) perror("sendto");
    else printf("udpecho: echoed %zd bytes back\n", w);

    close(s);
    return 0;
}
