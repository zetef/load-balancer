/* Supervisor (C3): fork + exec, waitpid, respawn cu backoff. */
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "supervisor.h"

int64_t lb_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

long supervisor_backoff_ms(unsigned k)
{
    long d = LB_SUP_BASE_MS;
    for (unsigned i = 0; i < k && d < LB_SUP_MAX_MS; i++) {
        d *= 2;
    }
    return d < LB_SUP_MAX_MS ? d : LB_SUP_MAX_MS;
}

static int copy_arg(char *dst, const char *src)
{
    int n = snprintf(dst, LB_SUP_ARG_LEN, "%s", src);
    return (n < 0 || n >= LB_SUP_ARG_LEN) ? -1 : 0;
}

int supervisor_init(struct lb_supervisor *s, const struct lb_config *cfg,
                    const char *bin, const char *shm_name, const char *extra,
                    struct lb_shm *shm, const sigset_t *child_mask)
{
    memset(s, 0, sizeof *s);
    if (cfg->workers < 1 || cfg->workers > LB_MAX_WORKERS) {
        fprintf(stderr, "lb: supervisor: workers must be 1..%d\n", LB_MAX_WORKERS);
        return -1;
    }
    s->n = cfg->workers;
    s->master_pid = getpid();
    s->child_mask = *child_mask;
    s->shm = shm;

    int n = snprintf(s->bin, sizeof s->bin, "%s", bin);
    if (n < 0 || (size_t)n >= sizeof s->bin) {
        fprintf(stderr, "lb: supervisor: worker path too long\n");
        return -1;
    }
    if (copy_arg(s->shm_name, shm_name) == -1) {
        fprintf(stderr, "lb: supervisor: shm name too long\n");
        return -1;
    }

    char ip[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &cfg->listen.sin_addr, ip, sizeof ip) == NULL) {
        fprintf(stderr, "lb: supervisor: inet_ntop: %s\n", strerror(errno));
        return -1;
    }
    snprintf(s->listen, sizeof s->listen, "%s:%u", ip, ntohs(cfg->listen.sin_port));
    snprintf(s->connect_ms, sizeof s->connect_ms, "%ld", cfg->connect_timeout_ms);
    snprintf(s->idle_ms, sizeof s->idle_ms, "%ld", cfg->idle_timeout_ms);

    if (extra != NULL) {
        char buf[LB_SUP_MAX_EXTRA * LB_SUP_ARG_LEN];
        n = snprintf(buf, sizeof buf, "%s", extra);
        if (n < 0 || (size_t)n >= sizeof buf) {
            fprintf(stderr, "lb: supervisor: --worker-extra too long\n");
            return -1;
        }
        char *save = NULL;
        for (char *tok = strtok_r(buf, " ", &save); tok != NULL; tok = strtok_r(NULL, " ", &save)) {
            if (s->n_extra == LB_SUP_MAX_EXTRA || copy_arg(s->extra[s->n_extra], tok) == -1) {
                fprintf(stderr, "lb: supervisor: --worker-extra: at most %d words of %d characters\n",
                        LB_SUP_MAX_EXTRA, LB_SUP_ARG_LEN - 1);
                return -1;
            }
            s->n_extra++;
        }
    }

    for (int i = 0; i < s->n; i++) {
        s->slots[i].state = LB_SLOT_EMPTY;
    }
    return 0;
}

int supervisor_spawn(struct lb_supervisor *s, int slot)
{
    if (slot < 0 || slot >= s->n) {
        return -1;
    }
    struct lb_worker_slot *w = &s->slots[slot];

    char idx[16];
    snprintf(idx, sizeof idx, "%d", slot);
    char *argv[12 + LB_SUP_MAX_EXTRA];
    int a = 0;
    argv[a++] = s->bin;
    argv[a++] = "--worker";
    argv[a++] = idx;
    argv[a++] = "--listen";
    argv[a++] = s->listen;
    argv[a++] = "--shm";
    argv[a++] = s->shm_name;
    argv[a++] = "--connect-timeout";
    argv[a++] = s->connect_ms;
    argv[a++] = "--idle-timeout";
    argv[a++] = s->idle_ms;
    for (int i = 0; i < s->n_extra; i++) {
        argv[a++] = s->extra[i];
    }
    argv[a] = NULL;

    if (s->shm != NULL) {
        atomic_store_explicit(&s->shm->workers[slot].active_conns, 0, memory_order_relaxed);
        atomic_store_explicit(&s->shm->workers[slot].total_conns, 0, memory_order_relaxed);
    }

    pid_t pid = fork();
    if (pid == -1) {
        int saved = errno;
        w->state = LB_SLOT_WAITING;
        w->restart_at_ms = lb_now_ms() + supervisor_backoff_ms(w->backoff_k);
        fprintf(stderr, "lb: worker %d: fork: %s, retrying later\n", slot, strerror(saved));
        return -1;
    }

    if (pid == 0) {
        /* copil: pana la exec doar functii async-signal-safe */
        sigprocmask(SIG_SETMASK, &s->child_mask, NULL);
        if (prctl(PR_SET_PDEATHSIG, SIGTERM) == -1) {
            _exit(126);
        }
        /* masterul a murit inainte de prctl */
        if (getppid() != s->master_pid) {
            _exit(126);
        }
        execv(s->bin, argv);
        static const char msg[] = "lb: worker exec failed\n";
        ssize_t r = write(STDERR_FILENO, msg, sizeof msg - 1);
        (void)r;
        _exit(127);
    }

    w->pid = pid;
    w->state = LB_SLOT_RUNNING;
    w->started_ms = lb_now_ms();
    if (s->shm != NULL) {
        s->shm->workers[slot].pid = pid;
        s->shm->workers[slot].restarts = w->restarts;
    }
    fprintf(stderr, "lb: worker %d started, pid %d\n", slot, (int)pid);
    return 0;
}

int supervisor_start_all(struct lb_supervisor *s)
{
    int ret = 0;
    for (int i = 0; i < s->n; i++) {
        if (supervisor_spawn(s, i) == -1) {
            ret = -1;
        }
    }
    return ret;
}

static void log_exit(int slot, pid_t pid, int status, int64_t ran_ms)
{
    if (WIFEXITED(status)) {
        fprintf(stderr, "lb: worker %d pid %d exited with %d after %lld ms\n",
                slot, (int)pid, WEXITSTATUS(status), (long long)ran_ms);
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "lb: worker %d pid %d killed by signal %d (%s)%s after %lld ms\n",
                slot, (int)pid, WTERMSIG(status), strsignal(WTERMSIG(status)),
                WCOREDUMP(status) ? ", core dumped" : "", (long long)ran_ms);
    } else {
        fprintf(stderr, "lb: worker %d pid %d changed state (status 0x%x)\n",
                slot, (int)pid, (unsigned)status);
    }
}

void supervisor_on_exit(struct lb_supervisor *s, int slot, int status, int64_t now_ms)
{
    struct lb_worker_slot *w = &s->slots[slot];
    int64_t ran = now_ms - w->started_ms;

    log_exit(slot, w->pid, status, ran);
    w->last_status = status;
    w->pid = 0;
    if (s->shm != NULL) {
        s->shm->workers[slot].pid = 0;
    }

    if (s->stopping) {
        w->state = LB_SLOT_EMPTY;
        return;
    }

    if (ran >= LB_SUP_STABLE_MS) {
        w->backoff_k = 0;
    }

    if (w->loop_count == 0 || now_ms - w->loop_start_ms > LB_SUP_LOOP_WINDOW_MS) {
        w->loop_start_ms = now_ms;
        w->loop_count = 0;
    }
    w->loop_count++;
    if (w->loop_count >= LB_SUP_LOOP_LIMIT) {
        w->state = LB_SLOT_GAVE_UP;
        fprintf(stderr, "lb: worker %d: crash loop (%u exits in %lld ms), giving up on this slot\n",
                slot, w->loop_count, (long long)(now_ms - w->loop_start_ms));
        return;
    }

    long delay = supervisor_backoff_ms(w->backoff_k);
    if (w->backoff_k < 31) {
        w->backoff_k++;
    }
    w->state = LB_SLOT_WAITING;
    w->restart_at_ms = now_ms + delay;
    fprintf(stderr, "lb: worker %d: restart in %ld ms\n", slot, delay);
}

static int find_slot(const struct lb_supervisor *s, pid_t pid)
{
    for (int i = 0; i < s->n; i++) {
        if (s->slots[i].state == LB_SLOT_RUNNING && s->slots[i].pid == pid) {
            return i;
        }
    }
    return -1;
}

int supervisor_reap(struct lb_supervisor *s)
{
    int reaped = 0;
    for (;;) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid > 0) {
            int slot = find_slot(s, pid);
            if (slot >= 0) {
                supervisor_on_exit(s, slot, status, lb_now_ms());
            } else {
                fprintf(stderr, "lb: reaped unknown child pid %d\n", (int)pid);
            }
            reaped++;
            continue;
        }
        if (pid == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == ECHILD) {
            for (int i = 0; i < s->n; i++) {
                if (s->slots[i].state == LB_SLOT_RUNNING) {
                    fprintf(stderr, "lb: worker %d pid %d vanished\n", i, (int)s->slots[i].pid);
                    s->slots[i].state = LB_SLOT_EMPTY;
                    s->slots[i].pid = 0;
                }
            }
            break;
        }
        fprintf(stderr, "lb: waitpid: %s\n", strerror(errno));
        break;
    }
    return reaped;
}

void supervisor_tick(struct lb_supervisor *s)
{
    if (s->stopping) {
        return;
    }
    int64_t now = lb_now_ms();
    for (int i = 0; i < s->n; i++) {
        struct lb_worker_slot *w = &s->slots[i];
        if (w->state == LB_SLOT_WAITING && w->restart_at_ms <= now) {
            w->restarts++;
            supervisor_spawn(s, i);
        }
    }
}

long supervisor_next_timeout_ms(const struct lb_supervisor *s)
{
    if (s->stopping) {
        return -1;
    }
    int64_t now = lb_now_ms();
    long best = -1;
    for (int i = 0; i < s->n; i++) {
        const struct lb_worker_slot *w = &s->slots[i];
        if (w->state != LB_SLOT_WAITING) {
            continue;
        }
        int64_t left = w->restart_at_ms - now;
        if (left < 0) {
            left = 0;
        }
        if (best == -1 || left < best) {
            best = (long)left;
        }
    }
    return best;
}

int supervisor_running(const struct lb_supervisor *s)
{
    int n = 0;
    for (int i = 0; i < s->n; i++) {
        if (s->slots[i].state == LB_SLOT_RUNNING) {
            n++;
        }
    }
    return n;
}

int supervisor_all_gave_up(const struct lb_supervisor *s)
{
    for (int i = 0; i < s->n; i++) {
        if (s->slots[i].state != LB_SLOT_GAVE_UP) {
            return 0;
        }
    }
    return s->n > 0;
}

static void kill_running(const struct lb_supervisor *s, int sig)
{
    for (int i = 0; i < s->n; i++) {
        const struct lb_worker_slot *w = &s->slots[i];
        if (w->state == LB_SLOT_RUNNING && kill(w->pid, sig) == -1 && errno != ESRCH) {
            fprintf(stderr, "lb: kill worker %d pid %d: %s\n", i, (int)w->pid, strerror(errno));
        }
    }
}

static void nap_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

void supervisor_stop_all(struct lb_supervisor *s, long deadline_ms)
{
    s->stopping = 1;
    for (int i = 0; i < s->n; i++) {
        if (s->slots[i].state == LB_SLOT_WAITING) {
            s->slots[i].state = LB_SLOT_EMPTY;
        }
    }

    kill_running(s, SIGTERM);
    int64_t end = lb_now_ms() + deadline_ms;
    while (supervisor_running(s) > 0 && lb_now_ms() < end) {
        supervisor_reap(s);
        if (supervisor_running(s) > 0) {
            nap_ms(20);
        }
    }

    if (supervisor_running(s) > 0) {
        for (int i = 0; i < s->n; i++) {
            if (s->slots[i].state == LB_SLOT_RUNNING) {
                fprintf(stderr, "lb: worker %d pid %d did not stop in %ld ms, SIGKILL\n",
                        i, (int)s->slots[i].pid, deadline_ms);
            }
        }
        kill_running(s, SIGKILL);
        while (supervisor_running(s) > 0) {
            supervisor_reap(s);
            if (supervisor_running(s) > 0) {
                nap_ms(10);
            }
        }
    }
}
