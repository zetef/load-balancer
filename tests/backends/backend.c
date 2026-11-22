/* backend: HTTP backend de test care raspunde cu ID-ul lui (S4).
 *   backend <id> <port> [--delay <ms>] [--bind <ipv4>]
 * POSIX, fara liblbcore: D10 il compileaza pe Alpine (musl).
 * Bucla: accept -> read pana la linia goala -> delay -> write_all -> close.
 * O eroare intr-o conexiune duce doar la close, niciodata la exit.
 */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define BACKLOG      64
#define REQ_MAX      4096
#define ID_MAX       32
#define RECV_TIMEOUT 5      /* secunde */

static volatile sig_atomic_t stop;

static void on_stop(int sig)
{
    (void)sig;
    stop = 1;
}

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s <id> <port> [--delay <ms>] [--bind <ipv4>]\n", prog);
    exit(2);
}

static long parse_num(const char *s, long min, long max)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || errno != 0 || v < min || v > max) {
        return -1;
    }
    return v;
}

static int write_all(int fd, const char *p, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* 1 = cerere completa, 0 = clientul a plecat, -1 = eroare */
static int read_request(int cfd)
{
    char buf[REQ_MAX + 1];
    size_t used = 0;

    while (used < REQ_MAX) {
        ssize_t n = read(cfd, buf + used, REQ_MAX - used);
        if (n == 0) {
            return 0;
        }
        if (n == -1) {
            if (errno == EINTR && !stop) {
                continue;
            }
            return -1;          /* EAGAIN = SO_RCVTIMEO */
        }
        used += (size_t)n;
        buf[used] = '\0';
        if (strstr(buf, "\r\n\r\n") != NULL || strstr(buf, "\n\n") != NULL) {
            return 1;
        }
    }
    return 1;                   /* buffer plin: raspundem oricum */
}

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR && !stop) {
    }
}

static void serve(int cfd, const char *id, long delay_ms)
{
    struct timeval tv = { RECV_TIMEOUT, 0 };
    if (setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == -1) {
        perror("setsockopt SO_RCVTIMEO");
        return;
    }

    int r = read_request(cfd);
    if (r != 1) {
        if (r == -1) {
            perror("read");
        }
        return;
    }
    if (delay_ms > 0) {
        sleep_ms(delay_ms);
    }

    char body[ID_MAX + 16];
    int blen = snprintf(body, sizeof body, "backend %s\n", id);
    char resp[256];
    int rlen = snprintf(resp, sizeof resp,
                        "HTTP/1.0 200 OK\r\n"
                        "Content-Type: text/plain\r\n"
                        "Content-Length: %d\r\n"
                        "Connection: close\r\n"
                        "\r\n"
                        "%s", blen, body);
    if (write_all(cfd, resp, (size_t)rlen) == -1) {
        perror("write");
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        usage(argv[0]);
    }
    const char *id = argv[1];
    if (strlen(id) == 0 || strlen(id) >= ID_MAX) {
        fprintf(stderr, "id must be 1..%d characters\n", ID_MAX - 1);
        return 2;
    }
    long port = parse_num(argv[2], 1, 65535);
    if (port == -1) {
        fprintf(stderr, "invalid port: %s\n", argv[2]);
        return 2;
    }
    long delay_ms = 0;
    const char *bind_ip = "127.0.0.1";
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--delay") == 0 && i + 1 < argc) {
            delay_ms = parse_num(argv[++i], 0, 60000);
            if (delay_ms == -1) {
                fprintf(stderr, "--delay must be 0..60000 ms\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) {
            bind_ip = argv[++i];
        } else {
            usage(argv[0]);
        }
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid --bind address: %s\n", bind_ip);
        return 2;
    }

    /* EPIPE in loc de SIGPIPE */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPIPE, &sa, NULL) == -1) {
        perror("sigaction SIGPIPE");
        return 1;
    }
    /* fara SA_RESTART: accept se intoarce cu EINTR */
    sa.sa_handler = on_stop;
    if (sigaction(SIGINT, &sa, NULL) == -1 || sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction");
        return 1;
    }

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd == -1) {
        perror("socket");
        return 1;
    }
    int one = 1;
    if (setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) == -1) {
        perror("setsockopt SO_REUSEADDR");
        close(lfd);
        return 1;
    }
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) == -1) {
        perror("bind");
        close(lfd);
        return 1;
    }
    if (listen(lfd, BACKLOG) == -1) {
        perror("listen");
        close(lfd);
        return 1;
    }
    fprintf(stderr, "backend %s listening on %s:%ld%s\n", id, bind_ip, port,
            delay_ms > 0 ? " (with delay)" : "");

    while (!stop) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd == -1) {
            if (errno != EINTR) {
                perror("accept");
                sleep_ms(100);      /* EMFILE s-ar repeta imediat */
            }
            continue;
        }
        serve(cfd, id, delay_ms);
        close(cfd);
    }

    close(lfd);
    fprintf(stderr, "backend %s: stopped\n", id);
    return 0;
}
