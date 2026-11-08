/* lb master entry point. For now it only loads and prints the configuration;
 * the supervisor (C3) and the worker mode (D5) come later. */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>

#include "config.h"

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "conf/lb.conf.example";
    struct lb_config cfg;
    char err[LB_CONFIG_ERR_LEN];

    if (lb_config_load(path, &cfg, err, sizeof err) == -1) {
        fprintf(stderr, "%s\n", err);
        return EXIT_FAILURE;
    }

    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &cfg.listen.sin_addr, ip, sizeof ip);
    printf("listen %s:%u, %d workers, %d backends\n",
           ip, ntohs(cfg.listen.sin_port), cfg.workers, cfg.n_backends);
    for (int i = 0; i < cfg.n_backends; i++) {
        const struct lb_config_backend *b = &cfg.backends[i];
        inet_ntop(AF_INET, &b->addr.sin_addr, ip, sizeof ip);
        printf("  backend %s %s:%u weight %u\n",
               b->name, ip, ntohs(b->addr.sin_port), b->weight);
    }
    return EXIT_SUCCESS;
}
