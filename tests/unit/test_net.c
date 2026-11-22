/* lb_write_all through a socketpair: every byte arrives, in order. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net.h"

int main(void)
{
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    char out[4096], in[4096];
    for (size_t i = 0; i < sizeof out; i++)
        out[i] = (char)(i * 7);

    assert(lb_write_all(sv[0], out, sizeof out) == (ssize_t)sizeof out);
    close(sv[0]);

    size_t got = 0;
    ssize_t n;
    while ((n = read(sv[1], in + got, sizeof in - got)) > 0)
        got += (size_t)n;
    assert(n == 0);
    assert(got == sizeof in);
    assert(memcmp(in, out, sizeof in) == 0);
    close(sv[1]);

    /* Writing to a closed fd fails with -1. */
    assert(lb_write_all(-1, out, 1) == -1);

    /* lb_set_nonblock: an empty pipe read returns EAGAIN instead of sleeping. */
    int pp[2];
    char c;
    assert(pipe(pp) == 0);
    assert(lb_set_nonblock(pp[0]) == 0);
    assert(read(pp[0], &c, 1) == -1 && errno == EAGAIN);
    assert(lb_set_nonblock(-1) == -1);
    close(pp[0]);
    close(pp[1]);

    printf("ok  lb_write_all, lb_set_nonblock\n");
    return 0;
}
