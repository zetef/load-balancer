/* lb master entry point: lb [--check] [config]
 * --check: only load and print the config. Otherwise create the shm region and wait:
 * SIGINT/SIGTERM = clean exit, SIGHUP = re-apply the config (C4 makes it a real reload).
 * The supervisor (C3) and the worker mode (D5) come later. */
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "shm.h"

static volatile sig_atomic_t got_stop;
static volatile sig_atomic_t got_hup;

static void on_signal(int sig)
{
    if (sig == SIGHUP) {
        got_hup = 1;
    } else {
        got_stop = 1;
    }
}

static void print_config(const struct lb_config *cfg)
{
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &cfg->listen.sin_addr, ip, sizeof ip);
    printf("listen %s:%u, %d workers, %d backends\n",
           ip, ntohs(cfg->listen.sin_port), cfg->workers, cfg->n_backends);
    for (int i = 0; i < cfg->n_backends; i++) {
        const struct lb_config_backend *b = &cfg->backends[i];
        inet_ntop(AF_INET, &b->addr.sin_addr, ip, sizeof ip);
        printf("  backend %s %s:%u weight %u\n",
               b->name, ip, ntohs(b->addr.sin_port), b->weight);
    }
    fflush(stdout);
}

/* Semnalele raman blocate si ajung doar in sigsuspend: niciun semnal pierdut */
static int setup_signals(sigset_t *old)
{
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGHUP);
    if (sigprocmask(SIG_BLOCK, &block, old) == -1) {
        return -1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1 ||
        sigaction(SIGHUP, &sa, NULL) == -1) {
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int check_only = 0;
    const char *path = "conf/lb.conf.example";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0) {
            check_only = 1;
        } else {
            path = argv[i];
        }
    }

    struct lb_config cfg;
    char err[LB_CONFIG_ERR_LEN];
    if (lb_config_load(path, &cfg, err, sizeof err) == -1) {
        fprintf(stderr, "%s\n", err);
        return EXIT_FAILURE;
    }
    print_config(&cfg);
    if (check_only) {
        return EXIT_SUCCESS;
    }

    sigset_t old;
    if (setup_signals(&old) == -1) {
        fprintf(stderr, "lb: signals: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    struct lb_shm *shm = lb_shm_create(LB_SHM_NAME, &cfg);
    if (shm == NULL) {
        fprintf(stderr, "lb: cannot create shm %s: %s\n", LB_SHM_NAME, strerror(errno));
        return EXIT_FAILURE;
    }
    printf("lb: shm %s ready, %zu bytes, generation %llu (pid %d)\n",
           LB_SHM_NAME, lb_shm_size(),
           (unsigned long long)atomic_load(&shm->generation), (int)getpid());
    fflush(stdout);

    int status = EXIT_SUCCESS;
    while (!got_stop) {
        sigsuspend(&old);

        if (got_hup) {
            got_hup = 0;
            int rc = lb_shm_apply_config(shm, &cfg);
            if (rc != 0) {
                fprintf(stderr, "lb: fatal: cannot update shm: %s\n", strerror(rc));
                status = EXIT_FAILURE;
                break;
            }
            printf("lb: config re-applied, generation %llu\n",
                   (unsigned long long)atomic_load(&shm->generation));
            fflush(stdout);
        }
    }

    if (lb_shm_destroy(shm, LB_SHM_NAME) == -1) {
        fprintf(stderr, "lb: shm cleanup: %s\n", strerror(errno));
        status = EXIT_FAILURE;
    }
    printf("lb: shm %s removed, bye\n", LB_SHM_NAME);
    return status;
}
