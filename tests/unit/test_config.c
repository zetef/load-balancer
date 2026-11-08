/* Test pentru lb_config_load: un fisier bun si unul gresit.
 * gcc -std=c11 -Wall -Wextra -g -fsanitize=address,undefined -Iinclude tests/unit/test_config.c src/master/config.c -o /tmp/test_config && /tmp/test_config
 */
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "config.h"

int main(void)
{
    struct lb_config cfg;
    char err[LB_CONFIG_ERR_LEN];

    /* 1. fisierul bun */
    assert(lb_config_load("tests/unit/configs/good.conf", &cfg, err, sizeof err) == 0);
    assert(ntohs(cfg.listen.sin_port) == 8080);
    assert(cfg.workers == 4);
    assert(cfg.algorithm == LB_ALGO_LEASTCONN);
    assert(cfg.n_backends == 2);
    assert(strcmp(cfg.backends[0].name, "web1") == 0);
    assert(cfg.backends[0].weight == 2);
    assert(cfg.backends[1].weight == 1);
    printf("ok  good.conf\n");

    /* 2. fisierul gresit pica */
    struct lb_config before = cfg;
    assert(lb_config_load("tests/unit/configs/bad.conf", &cfg, err, sizeof err) == -1);
    assert(strcmp(err, "tests/unit/configs/bad.conf:4: duplicate backend name 'web1'") == 0);
    assert(memcmp(&cfg, &before, sizeof cfg) == 0);
    printf("ok  %s\n", err);

    printf("all config tests passed\n");
    return 0;
}
