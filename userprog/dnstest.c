// dnstest: raw UDP DNS round-trip with BLOCKING recvfrom (no poll), to isolate
// whether Index's UDP DNS path works at all -- separate from musl getaddrinfo's
// poll-based wait. sendto(10.0.2.3:53, A-query) then recvfrom; parse first A.
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <time.h>
#include <errno.h>

static int build_query(unsigned char *pkt, const char *name) {
    int p = 0;
    pkt[0]=0x12;pkt[1]=0x34;pkt[2]=0x01;pkt[3]=0x00;
    pkt[4]=0;pkt[5]=1;pkt[6]=0;pkt[7]=0;pkt[8]=0;pkt[9]=0;pkt[10]=0;pkt[11]=0;
    p=12;
    const char *s=name;
    while(*s){ const char*dot=strchr(s,'.'); int len=dot?(int)(dot-s):(int)strlen(s);
        pkt[p++]=(unsigned char)len; memcpy(pkt+p,s,len); p+=len; if(!dot)break; s=dot+1; }
    pkt[p++]=0; pkt[p++]=0; pkt[p++]=1; pkt[p++]=0; pkt[p++]=1;
    return p;
}

int main(int argc, char **argv) {
    const char *name = argc > 1 ? argv[1] : "deb.debian.org";
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { printf("SOCK_FAIL\n"); return 1; }

    unsigned char pkt[512];
    int p = 0;
    pkt[0] = 0x12; pkt[1] = 0x34;          // id
    pkt[2] = 0x01; pkt[3] = 0x00;          // flags: RD
    pkt[4] = 0; pkt[5] = 1;                // qdcount=1
    pkt[6]=0;pkt[7]=0;pkt[8]=0;pkt[9]=0;pkt[10]=0;pkt[11]=0;
    p = 12;
    const char *s = name;                  // encode qname
    while (*s) {
        const char *dot = strchr(s, '.');
        int len = dot ? (int)(dot - s) : (int)strlen(s);
        pkt[p++] = (unsigned char)len;
        memcpy(pkt + p, s, len); p += len;
        if (!dot) break;
        s = dot + 1;
    }
    pkt[p++] = 0;                          // root label
    pkt[p++] = 0; pkt[p++] = 1;            // qtype A
    pkt[p++] = 0; pkt[p++] = 1;            // qclass IN

    struct sockaddr_in dns;
    memset(&dns, 0, sizeof dns);
    dns.sin_family = AF_INET;
    dns.sin_port = htons(53);
    inet_pton(AF_INET, "10.0.2.3", &dns.sin_addr);

    printf("DNS sendto 10.0.2.3:53 qlen=%d for %s\n", p, name); fflush(stdout);
    if (sendto(fd, pkt, p, 0, (struct sockaddr *)&dns, sizeof dns) < 0) { printf("SENDTO_FAIL\n"); return 1; }

    unsigned char rsp[512];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    long r = recvfrom(fd, rsp, sizeof rsp, 0, (struct sockaddr *)&from, &fl);
    printf("DNS recvfrom rc=%ld\n", r); fflush(stdout);
    if (r < 12) { printf("DNS_NO_RESPONSE (round-trip FAILED)\n"); return 1; }

    int ancount = (rsp[6] << 8) | rsp[7];
    printf("DNS ancount=%d\n", ancount);

    // ===== variant 2: sendto + poll(POLLIN) + recvfrom (what musl getaddrinfo does) =====
    int fd2 = socket(AF_INET, SOCK_DGRAM, 0);
    unsigned char q2[512]; int q2len = build_query(q2, name);
    printf("DNS2 sendto + poll(POLLIN,5s) ...\n"); fflush(stdout);
    sendto(fd2, q2, q2len, 0, (struct sockaddr *)&dns, sizeof dns);
    struct pollfd pfd; pfd.fd = fd2; pfd.events = POLLIN; pfd.revents = 0;
    int pr = poll(&pfd, 1, 5000);
    printf("DNS2 poll rc=%d revents=0x%x\n", pr, pfd.revents);
    if (pr > 0 && (pfd.revents & POLLIN)) {
        unsigned char r2[512];
        long rr = recvfrom(fd2, r2, sizeof r2, 0, NULL, NULL);
        printf("DNS2_POLL_OK recvfrom=%ld (poll-on-UDP WORKS)\n", rr);
    } else {
        printf("DNS2_POLL_FAIL (poll-on-UDP BROKEN -- this is musl getaddrinfo's path)\n");
    }

    // ===== variant 3: SOCK_NONBLOCK recvfrom on an EMPTY socket. musl's res_msend
    // drains with `while (recvfrom()>=0)` expecting -1/EAGAIN immediately. If Index
    // ignores O_NONBLOCK it blocks ~10s -> musl's loop stalls past its timeout. =====
    int fd3 = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    unsigned char tmp[64];
    long rr3 = recvfrom(fd3, tmp, sizeof tmp, 0, NULL, NULL);
    int e3 = errno;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    printf("DNS3 NONBLOCK-empty recvfrom rc=%ld errno=%d elapsed=%ldms\n", rr3, e3, ms);
    if (rr3 < 0 && ms < 200)
        printf("DNS3_NONBLOCK_HONORED (not the bug)\n");
    else
        printf("DNS3_NONBLOCK_IGNORED (blocked %ldms -- THIS breaks musl getaddrinfo)\n", ms);
    return 0;
}
