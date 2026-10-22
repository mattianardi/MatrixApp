#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>

static int lfd;
static void *server(void *arg)
{
    (void)arg;
    int c = accept(lfd, NULL, NULL);
    char b[128]; ssize_t n = read(c, b, sizeof b - 1);
    for (ssize_t i = 0; i < n; i++) if (b[i] >= 'a' && b[i] <= 'z') b[i] -= 32;
    write(c, b, (size_t)n); close(c);
    return NULL;
}

int main(int argc, char **argv)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(7001), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (bind(lfd, (void *)&a, sizeof a) || listen(lfd, 4)) { perror("listen"); return 1; }
    pthread_t t; pthread_create(&t, NULL, server, NULL);
    int c = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(c, (void *)&a, sizeof a)) { perror("connect"); return 1; }
    write(c, "hello over loopback", 19);
    char buf[128] = {0}; ssize_t n = read(c, buf, sizeof buf - 1);
    printf("NET tcp loopback: %zd bytes: %s\n", n, buf);
    close(c); pthread_join(t, NULL); close(lfd);

    int u1 = socket(AF_INET, SOCK_DGRAM, 0), u2 = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in ua = { .sin_family = AF_INET, .sin_port = htons(7002), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    bind(u1, (void *)&ua, sizeof ua);
    sendto(u2, "datagram", 8, 0, (void *)&ua, sizeof ua);
    struct pollfd pf = { u1, POLLIN, 0 };
    int pr = poll(&pf, 1, 2000);
    char d[64] = {0}; struct sockaddr_in from; socklen_t fl = sizeof from;
    n = recvfrom(u1, d, sizeof d - 1, 0, (void *)&from, &fl);
    printf("NET udp loopback: poll=%d %zd bytes: %s\n", pr, n, d);

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res;
    int gr = getaddrinfo("localhost", "80", &hints, &res);
    if (!gr) { char ip[32]; inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, ip, sizeof ip); printf("NET getaddrinfo localhost -> %s\n", ip); }
    else printf("NET getaddrinfo failed: %s\n", gai_strerror(gr));

    if (argc > 1) {                                                  /* an HTTP server reachable over the virtual network, e.g. 10.0.2.2 */
        int s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in h = { .sin_family = AF_INET, .sin_port = htons((unsigned short)(argc > 2 ? atoi(argv[2]) : 80)) };
        inet_pton(AF_INET, argv[1], &h.sin_addr);
        if (connect(s, (void *)&h, sizeof h)) { perror("connect host"); return 1; }
        const char *rq = "GET /index.html HTTP/1.0\r\nHost: x\r\n\r\n";
        write(s, rq, strlen(rq));
        char r[512]; size_t tot = 0; while ((n = read(s, r + tot, sizeof r - 1 - tot)) > 0) tot += (size_t)n; r[tot] = 0;
        char *e = strchr(r, 13); if (e) *e = 0;
        printf("NET host http: %s\n", r);
        close(s);
    }
    printf("NET done\n");
    return 0;
}
