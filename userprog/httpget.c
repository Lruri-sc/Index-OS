// httpget: minimal HTTP/1.0 downloader to isolate Index's TCP receive path and
// DNS. Usage: httpget <host> <port> <path> [hostheader]
//   host       : dotted IP (inet_pton) OR a name (getaddrinfo -> tests DNS)
//   hostheader : value for the HTTP Host: header (default = host). Needed when
//                <host> is a resolved IP but the server is a CDN keyed on name.
// Reads the whole response to EOF counting bytes; prints the status line, any
// Content-Length, progress every 512 KB, and HG_DONE with total + body + the
// last read() return (0 = clean EOF, <0 = error/timeout). body==clen => COMPLETE.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "10.0.2.2";
    int port = argc > 2 ? atoi(argv[2]) : 8000;
    const char *path = argc > 3 ? argv[3] : "/big.bin";
    const char *hh = argc > 4 ? argv[4] : host;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);

    if (inet_pton(AF_INET, host, &sa.sin_addr) == 1) {
        printf("HG host=%s (literal IP)\n", host);
    } else {
        printf("HG resolving %s ...\n", host); fflush(stdout);
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        int gr = getaddrinfo(host, NULL, &hints, &res);
        if (gr != 0 || !res) { printf("DNS_FAIL gr=%d\n", gr); return 1; }
        sa.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        char ips[64]; inet_ntop(AF_INET, &sa.sin_addr, ips, sizeof ips);
        printf("HG DNS_OK %s -> %s\n", host, ips);
        freeaddrinfo(res);
    }
    fflush(stdout);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { printf("SOCKET_FAIL\n"); return 1; }
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { printf("CONNECT_FAIL\n"); return 1; }
    printf("HG connected, GET %s (Host: %s)\n", path, hh); fflush(stdout);

    char req[400];
    int n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: idx-httpget\r\nConnection: close\r\n\r\n",
                     path, hh);
    if (write(fd, req, n) != n) { printf("WRITE_FAIL\n"); return 1; }

    char buf[16384];
    long total = 0, body = 0, clen = -1, next_mark = 524288;
    int reads = 0, hdr_done = 0, printed_status = 0;
    char acc[2048]; int acclen = 0;
    long r;
    while ((r = read(fd, buf, sizeof buf)) > 0) {
        total += r;
        reads++;
        if (!hdr_done) {
            for (long i = 0; i < r && acclen < (int)sizeof acc - 1; i++) acc[acclen++] = buf[i];
            acc[acclen] = 0;
            if (!printed_status) {
                char *nl = strchr(acc, '\n');
                if (nl) { *nl = 0; printf("HG status: %s\n", acc); *nl = '\n'; printed_status = 1; fflush(stdout); }
            }
            char *p = strstr(acc, "\r\n\r\n");
            if (p) {
                hdr_done = 1;
                char *cl = strstr(acc, "Content-Length:");
                if (!cl) cl = strstr(acc, "content-length:");
                if (cl) clen = atol(cl + 15);
                long hdrbytes = (p + 4) - acc;
                body = total - hdrbytes;
                printf("HG header_done clen=%ld\n", clen); fflush(stdout);
            }
        } else {
            body += r;
        }
        if (total >= next_mark) {
            printf("HG progress total=%ld body=%ld reads=%d\n", total, body, reads);
            fflush(stdout); next_mark += 524288;
        }
    }
    const char *verdict = (clen >= 0) ? (body == clen ? "COMPLETE" : "TRUNCATED") : "NO_CLEN";
    printf("HG_DONE total=%ld body=%ld clen=%ld last_read=%ld reads=%d %s\n",
           total, body, clen, r, reads, verdict);
    fflush(stdout);
    return 0;
}
