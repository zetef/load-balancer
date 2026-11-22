/* Teste pentru lb_config_load. Ruleaza din radacina repo-ului: make test */
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "config.h"

#define DIR "tests/unit/configs/"

struct bad_case {
    const char *file;
    int         line;
    const char *msg;
};

static const struct bad_case bad_cases[] = {
    { "bad_dup_name.conf",          4,  "duplicate backend name 'web1'" },
    { "bad_unknown_directive.conf", 3,  "unknown directive 'bakend'" },
    { "bad_listen_args.conf",       2,  "listen needs 1 argument, got 2" },
    { "bad_listen_hostname.conf",   2,  "listen must be <ipv4>:<port> (no hostnames), got 'localhost:8080'" },
    { "bad_duplicate_listen.conf",  3,  "duplicate listen" },
    { "bad_backend_args.conf",      3,  "backend needs <name> <ip>:<port> [weight N]" },
    { "bad_backend_port.conf",      3,  "backend address must be <ipv4>:<port> (no hostnames), got '127.0.0.1:70000'" },
    { "bad_backend_noport.conf",    3,  "backend address must be <ipv4>:<port> (no hostnames), got '1.2.3.4'" },
    { "bad_name_too_long.conf",     3,  "backend name 'abcdefghijklmnopqrstuvwxyz012345' too long (max 31 characters)" },
    { "bad_weight.conf",            3,  "weight must be 1..1000, got '1001'" },
    { "bad_weight_keyword.conf",    3,  "expected 'weight', got 'wieght'" },
    { "bad_workers.conf",           3,  "workers must be 1..64, got '0'" },
    { "bad_workers_text.conf",      3,  "workers must be 1..64, got '4x'" },
    { "bad_algorithm.conf",         3,  "algorithm must be rr, leastconn or iphash, got 'random'" },
    { "bad_health_args.conf",       4,  "health needs 4 arguments: <interval_ms> <timeout_ms> <rise> <fall>, got 3" },
    { "bad_health_timeout.conf",    4,  "health timeout_ms (500) must be less than interval_ms (500)" },
    { "bad_timeout_kind.conf",      4,  "timeout must be 'connect' or 'idle', got 'read'" },
    { "bad_too_many_words.conf",    3,  "too many words on line" },
    /* erorile din validate() au linia = ultima linie */
    { "bad_missing_listen.conf",    2,  "missing required directive 'listen'" },
    { "bad_no_backend.conf",        3,  "at least one 'backend' is required" },
    /* b1..b65 pe liniile 3..67 */
    { "bad_too_many_backends.conf", 67, "too many backends (max 64)" },
};

/* -1, mesaj exact si struct neatins. 0 = trecut, 1 = picat */
static int check_bad(const struct bad_case *c, struct lb_config *cfg)
{
    char path[128];
    char expected[LB_CONFIG_ERR_LEN];
    char err[LB_CONFIG_ERR_LEN] = "";
    struct lb_config before = *cfg;

    snprintf(path, sizeof path, DIR "%s", c->file);
    snprintf(expected, sizeof expected, "%s:%d: %s", path, c->line, c->msg);

    if (lb_config_load(path, cfg, err, sizeof err) != -1) {
        printf("FAIL %s: was accepted, expected an error\n", c->file);
        return 1;
    }
    if (strcmp(err, expected) != 0) {
        printf("FAIL %s\n  expected: %s\n  got:      %s\n", c->file, expected, err);
        return 1;
    }
    if (memcmp(cfg, &before, sizeof *cfg) != 0) {
        printf("FAIL %s: caller struct was changed on failure\n", c->file);
        return 1;
    }
    printf("ok   %s\n", err);
    return 0;
}

int main(void)
{
    struct lb_config cfg;
    char err[LB_CONFIG_ERR_LEN];
    int failed = 0;

    if (lb_config_load(DIR "good.conf", &cfg, err, sizeof err) != 0) {
        printf("FAIL good.conf: %s\n", err);
        return 1;
    }
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &cfg.backends[0].addr.sin_addr, ip, sizeof ip);
    assert(ntohs(cfg.listen.sin_port) == 8080);
    assert(cfg.workers == 4);
    assert(cfg.algorithm == LB_ALGO_LEASTCONN);
    assert(cfg.n_backends == 2);
    assert(strcmp(cfg.backends[0].name, "web1") == 0);
    assert(strcmp(ip, "127.0.0.1") == 0);
    assert(ntohs(cfg.backends[0].addr.sin_port) == 9001);
    assert(cfg.backends[0].weight == 2);
    assert(strcmp(cfg.backends[1].name, "web2") == 0);
    assert(cfg.backends[1].weight == 1);
    assert(cfg.hc_interval_ms == 2000 && cfg.hc_timeout_ms == 500);
    assert(cfg.hc_rise == 2 && cfg.hc_fall == 3);
    assert(cfg.connect_timeout_ms == 500 && cfg.idle_timeout_ms == 30000);
    assert(strcmp(cfg.admin_socket, "/tmp/lb.sock") == 0);
    printf("ok   good.conf\n");

    /* exact limita trebuie acceptata */
    struct lb_config cfg64;
    if (lb_config_load(DIR "good_64_backends.conf", &cfg64, err, sizeof err) != 0) {
        printf("FAIL good_64_backends.conf: %s\n", err);
        return 1;
    }
    assert(cfg64.n_backends == LB_MAX_BACKENDS);
    assert(strcmp(cfg64.backends[63].name, "b64") == 0);
    printf("ok   good_64_backends.conf (%d backends)\n", cfg64.n_backends);

    for (size_t i = 0; i < sizeof bad_cases / sizeof bad_cases[0]; i++) {
        failed += check_bad(&bad_cases[i], &cfg);
    }

    struct lb_config before = cfg;
    assert(lb_config_load(DIR "does_not_exist.conf", &cfg, err, sizeof err) == -1);
    assert(strstr(err, "cannot open: No such file or directory") != NULL);
    assert(memcmp(&cfg, &before, sizeof cfg) == 0);
    printf("ok   %s\n", err);

    if (failed) {
        printf("%d config tests FAILED\n", failed);
        return 1;
    }
    printf("all config tests passed (%zu bad configs)\n", sizeof bad_cases / sizeof bad_cases[0]);
    return 0;
}
