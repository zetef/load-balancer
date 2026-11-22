/* lb master: lb [--check] [--worker-bin <path>] [--worker-extra "<args>"]
 *              [--fd-audit | --fd-audit-leak] [config]
 * SIGINT/SIGTERM = stop, SIGHUP = re-apply the config, SIGCHLD = reap and respawn. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "config.h"
#include "shm.h"
#include "supervisor.h"

#define STOP_DEADLINE_MS 5000

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

/* semnalele raman blocate si sunt citite cu sigtimedwait (C4: signalfd) */
static int block_signals(sigset_t *set, sigset_t *old)
{
    sigemptyset(set);
    sigaddset(set, SIGINT);
    sigaddset(set, SIGTERM);
    sigaddset(set, SIGHUP);
    sigaddset(set, SIGCHLD);
    return sigprocmask(SIG_BLOCK, set, old);
}

/* stub_worker din directorul lui lb */
static int default_worker_bin(char *out, size_t len)
{
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n == -1) {
        return -1;
    }
    self[n] = '\0';
    char *slash = strrchr(self, '/');
    if (slash != NULL) {
        *slash = '\0';
    }
    int w = snprintf(out, len, "%s/stub_worker", self);
    return (w < 0 || (size_t)w >= len) ? -1 : 0;
}

/* C3 pasul 5: leak = 1 deschide socketul fara CLOEXEC */
static int open_audit_fds(const char *cfg_path, int leak, int fds[3])
{
    fds[0] = open(cfg_path, O_RDONLY | O_CLOEXEC);
    fds[1] = socket(AF_UNIX, SOCK_STREAM | (leak ? 0 : SOCK_CLOEXEC), 0);
    fds[2] = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    for (int i = 0; i < 3; i++) {
        if (fds[i] == -1) {
            fprintf(stderr, "lb: fd audit: %s\n", strerror(errno));
            return -1;
        }
    }
    fprintf(stderr, "lb: fd audit: config fd %d, unix socket fd %d%s, timerfd fd %d\n",
            fds[0], fds[1], leak ? " (NO CLOEXEC, leaks into workers)" : "", fds[2]);
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [--check] [--worker-bin <path>] [--worker-extra \"<args>\"] "
                    "[--fd-audit | --fd-audit-leak] [config]\n", prog);
    exit(2);
}

int main(int argc, char **argv)
{
    int check_only = 0;
    int fd_audit = 0;   /* 1 = cu CLOEXEC, 2 = cu un fd scapat */
    const char *path = "conf/lb.conf.example";
    const char *worker_bin = NULL;
    const char *worker_extra = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0) {
            check_only = 1;
        } else if (strcmp(argv[i], "--worker-bin") == 0 && i + 1 < argc) {
            worker_bin = argv[++i];
        } else if (strcmp(argv[i], "--worker-extra") == 0 && i + 1 < argc) {
            worker_extra = argv[++i];
        } else if (strcmp(argv[i], "--fd-audit") == 0) {
            fd_audit = 1;
        } else if (strcmp(argv[i], "--fd-audit-leak") == 0) {
            fd_audit = 2;
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
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

    char bin[PATH_MAX];
    if (worker_bin == NULL) {
        if (default_worker_bin(bin, sizeof bin) == -1) {
            fprintf(stderr, "lb: cannot find the worker program next to lb\n");
            return EXIT_FAILURE;
        }
    } else {
        snprintf(bin, sizeof bin, "%s", worker_bin);
    }
    if (access(bin, X_OK) == -1) {
        fprintf(stderr, "lb: worker program %s: %s\n", bin, strerror(errno));
        return EXIT_FAILURE;
    }

    sigset_t set, orig;
    if (block_signals(&set, &orig) == -1) {
        fprintf(stderr, "lb: sigprocmask: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    int audit_fds[3] = { -1, -1, -1 };
    if (fd_audit && open_audit_fds(path, fd_audit == 2, audit_fds) == -1) {
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

    static struct lb_supervisor sup;
    int status = EXIT_SUCCESS;
    if (supervisor_init(&sup, &cfg, bin, LB_SHM_NAME, worker_extra, shm, &orig) == -1) {
        lb_shm_destroy(shm, LB_SHM_NAME);
        return EXIT_FAILURE;
    }
    fflush(stdout);
    supervisor_start_all(&sup);

    int stop = 0;
    while (!stop) {
        long timeout = supervisor_next_timeout_ms(&sup);
        siginfo_t info;
        int sig;
        if (timeout < 0) {
            sig = sigwaitinfo(&set, &info);
        } else {
            struct timespec ts = { timeout / 1000, (timeout % 1000) * 1000000L };
            sig = sigtimedwait(&set, &info, &ts);      /* EAGAIN = timpul unui restart */
        }

        if (sig == -1) {
            if (errno != EAGAIN && errno != EINTR) {
                fprintf(stderr, "lb: sigtimedwait: %s\n", strerror(errno));
                status = EXIT_FAILURE;
                break;
            }
        } else if (sig == SIGCHLD) {
            supervisor_reap(&sup);
        } else if (sig == SIGHUP) {
            int rc = lb_shm_apply_config(shm, &cfg);
            if (rc != 0) {
                fprintf(stderr, "lb: fatal: cannot update shm: %s\n", strerror(rc));
                status = EXIT_FAILURE;
                break;
            }
            printf("lb: config re-applied, generation %llu\n",
                   (unsigned long long)atomic_load(&shm->generation));
            fflush(stdout);
        } else {
            fprintf(stderr, "lb: %s received, stopping\n", strsignal(sig));
            stop = 1;
        }

        supervisor_tick(&sup);
        if (supervisor_all_gave_up(&sup)) {
            fprintf(stderr, "lb: every worker slot is in a crash loop, exiting\n");
            status = EXIT_FAILURE;
            break;
        }
    }

    supervisor_stop_all(&sup, STOP_DEADLINE_MS);

    for (int i = 0; i < 3; i++) {
        if (audit_fds[i] != -1) {
            close(audit_fds[i]);
        }
    }
    if (lb_shm_destroy(shm, LB_SHM_NAME) == -1) {
        fprintf(stderr, "lb: shm cleanup: %s\n", strerror(errno));
        status = EXIT_FAILURE;
    }
    printf("lb: shm %s removed, bye\n", LB_SHM_NAME);
    return status;
}
