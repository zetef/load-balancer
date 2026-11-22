/* Teste pentru supervisor (C3). Ruleaza din radacina repo-ului: make test */
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "supervisor.h"

static struct lb_config cfg;
static struct lb_supervisor sup;
static sigset_t empty;

static int status_of_exit(int code)
{
    pid_t pid = fork();
    assert(pid != -1);
    if (pid == 0) {
        _exit(code);
    }
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    return status;
}

static void fresh(const char *bin)
{
    assert(supervisor_init(&sup, &cfg, bin, "/lb_shm_test", NULL, NULL, &empty) == 0);
}

static void test_backoff(void)
{
    assert(supervisor_backoff_ms(0) == 100);
    assert(supervisor_backoff_ms(1) == 200);
    assert(supervisor_backoff_ms(6) == 6400);
    assert(supervisor_backoff_ms(7) == LB_SUP_MAX_MS);     /* 12800 -> plafonat */
    assert(supervisor_backoff_ms(1000) == LB_SUP_MAX_MS);  /* fara overflow */
    printf("ok   backoff: 100, 200, ..., 6400, then capped at %d ms\n", LB_SUP_MAX_MS);
}

static void test_init_extra(void)
{
    assert(supervisor_init(&sup, &cfg, "/bin/false", "/x", "--crash-after 0", NULL, &empty) == 0);
    assert(sup.n_extra == 2);
    assert(sup.extra[0][0] == '-' && sup.extra[1][0] == '0');
    assert(supervisor_init(&sup, &cfg, "/bin/false", "/x", "a b c d e f g h i", NULL, &empty) == -1);
    printf("ok   --worker-extra split into words, more than %d refused\n", LB_SUP_MAX_EXTRA);
}

static void test_crash_loop(int st)
{
    fresh("/bin/false");
    struct lb_worker_slot *w = &sup.slots[0];
    int64_t t = 1000;
    long expect = 100;
    for (int i = 1; i < LB_SUP_LOOP_LIMIT; i++) {
        w->state = LB_SLOT_RUNNING;
        w->pid = 12345;
        w->started_ms = t;
        t += 50;
        supervisor_on_exit(&sup, 0, st, t);
        assert(w->state == LB_SLOT_WAITING);
        assert(w->restart_at_ms - t == expect);
        assert(w->pid == 0);
        t = w->restart_at_ms;
        expect = expect * 2 > LB_SUP_MAX_MS ? LB_SUP_MAX_MS : expect * 2;
    }
    w->state = LB_SLOT_RUNNING;
    w->started_ms = t;
    supervisor_on_exit(&sup, 0, st, t + 50);
    assert(w->state == LB_SLOT_GAVE_UP);
    assert(supervisor_all_gave_up(&sup) == (sup.n == 1));
    assert(supervisor_next_timeout_ms(&sup) == -1);
    printf("ok   crash loop: delays double, slot gives up at exit %d\n", LB_SUP_LOOP_LIMIT);
}

static void test_stable_resets(int st)
{
    fresh("/bin/false");
    struct lb_worker_slot *w = &sup.slots[0];
    w->backoff_k = 5;
    w->state = LB_SLOT_RUNNING;
    w->started_ms = 0;
    supervisor_on_exit(&sup, 0, st, LB_SUP_STABLE_MS);
    assert(w->restart_at_ms - LB_SUP_STABLE_MS == LB_SUP_BASE_MS);

    w->loop_count = LB_SUP_LOOP_LIMIT - 1;
    w->loop_start_ms = 0;
    w->state = LB_SLOT_RUNNING;
    w->started_ms = 100000;
    supervisor_on_exit(&sup, 0, st, 100000 + 10);
    assert(w->state == LB_SLOT_WAITING && w->loop_count == 1);
    printf("ok   stable worker resets the backoff, old crashes leave the window\n");
}

static void test_stopping(int st)
{
    fresh("/bin/false");
    sup.stopping = 1;
    sup.slots[0].state = LB_SLOT_RUNNING;
    supervisor_on_exit(&sup, 0, st, 10);
    assert(sup.slots[0].state == LB_SLOT_EMPTY);
    printf("ok   exit during stop is not a crash\n");
}

static void reap_one(void)
{
    struct timespec ts = { 0, 10 * 1000000L };
    for (int i = 0; i < 200; i++) {
        if (supervisor_reap(&sup) > 0) {
            return;
        }
        nanosleep(&ts, NULL);
    }
    assert(!"child not reaped in 2 s");
}

static void test_real_spawn(void)
{
    fresh("/bin/false");
    assert(supervisor_spawn(&sup, 0) == 0);
    assert(sup.slots[0].state == LB_SLOT_RUNNING && sup.slots[0].pid > 0);
    assert(supervisor_running(&sup) == 1);
    reap_one();
    assert(WIFEXITED(sup.slots[0].last_status) && WEXITSTATUS(sup.slots[0].last_status) == 1);
    assert(sup.slots[0].state == LB_SLOT_WAITING);
    long left = supervisor_next_timeout_ms(&sup);
    assert(left >= 0 && left <= LB_SUP_BASE_MS);
    printf("ok   fork + exec /bin/false: reaped, exit 1 decoded, restart in <= %d ms\n", LB_SUP_BASE_MS);

    fresh("/nonexistent/worker");
    assert(supervisor_spawn(&sup, 0) == 0);
    reap_one();
    assert(WIFEXITED(sup.slots[0].last_status) && WEXITSTATUS(sup.slots[0].last_status) == 127);
    printf("ok   exec failure: child exits 127 with _exit\n");

    assert(supervisor_reap(&sup) == 0);
}

int main(void)
{
    char err[LB_CONFIG_ERR_LEN];
    if (lb_config_load("tests/unit/configs/good.conf", &cfg, err, sizeof err) != 0) {
        printf("FAIL good.conf: %s\n", err);
        return 1;
    }
    cfg.workers = 1;
    sigemptyset(&empty);

    int st = status_of_exit(1);
    test_backoff();
    test_init_extra();
    test_crash_loop(st);
    test_stable_resets(st);
    test_stopping(st);
    test_real_spawn();
    printf("all supervisor tests passed\n");
    return 0;
}
