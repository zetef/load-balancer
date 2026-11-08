#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include "config.h"
#include <errno.h>

#define MAX_TOKENS 8

struct parse_ctx {
    const char       *path;
    int               lineno;
    char             *err;
    size_t            errlen;
    struct lb_config *cfg;
    int               seen_listen;
};

int parse_long(const char *s, long min, long max, long *out)
{
	char* end;
	errno = 0;
	long v = strtol(s, &end, 10); // atoi wannabe
	if(end == s || errno == ERANGE || v < min || v > max || *end != '\0') {
		return -1;
	}
	*out = v;
	return 0;
}

int parse_ip_port(const char *s, struct sockaddr_in *out)
{
    const char *p = strrchr(s, ':');
    if (!p) {
        return -1;
    }

    long port;
    if (parse_long(p + 1, 1, 65535, &port) != 0) {
        return -1;
    }

    size_t len = (size_t)(p - s);
	/*INET_ADDRSTRLEN = 16 = "255.255.255.255" + '\0'. */
    char ip[INET_ADDRSTRLEN];
    if (len == 0 || len >= sizeof ip) {
        return -1;
    }

    memcpy(ip, s, len);
    ip[len] = '\0';

    struct sockaddr_in tmp;
    memset(&tmp, 0, sizeof tmp);
	tmp.sin_family = AF_INET;           /* adresa e IPv4 */
    /* htons = "host to network short": intoarce portul in ordinea octetilor din retea (big-endian) */
    tmp.sin_port = htons((uint16_t)port);


    /* inet_pton transforma textul "127.0.0.1" in cei 4 octeti ai adresei.*/
    if(inet_pton(AF_INET, ip, &tmp.sin_addr) != 1) {
        return -1;
    }
    
	
	*out = tmp;
    return 0;
}

/* Scrie "path:lineno: <mesaj formatat>" in ctx->err */
static int set_err(struct parse_ctx *ctx, const char *fmt, ...)
{
    char msg[200];

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(ctx->err, ctx->errlen, "%s:%d: %s", ctx->path, ctx->lineno, msg);
    return -1;
}

/* Valorile implicite */
static void config_defaults(struct lb_config *c)
{
    memset(c, 0, sizeof *c);
    c->workers            = 1;
    c->algorithm          = LB_ALGO_RR;
    c->hc_interval_ms     = 2000;
    c->hc_timeout_ms      = 500;
    c->hc_rise            = 2;
    c->hc_fall            = 3;
    c->connect_timeout_ms = 500;
    c->idle_timeout_ms    = 30000;

    snprintf(c->admin_socket, sizeof c->admin_socket, "%s", "/tmp/lb.sock"); //strncpy
}

/* Imparte linia in token-uri si returneaza numarul lor. */
static int tokenize(char *line, char **tok, int max)
{
    char *save;
    int n = 0;

    char *t = strtok_r(line, " \t\r\n", &save); // strtok_r este threadsafe pentru ca strtok pastreaza un semn intr o variabila globala.
    while(t != NULL) {
        if (n == max) {
            return -1;
        }
        tok[n] = t;
        n++;
        t = strtok_r(NULL, " \t\r\n", &save);
    }
    return n;
}

/* handlere (tok[0] = numele directivei) */

static int parse_listen(struct parse_ctx *ctx, char **tok, int ntok)
{
    if(ntok != 2) {
        return set_err(ctx, "listen needs 1 argument, got %d", ntok - 1);
    }
    if(ctx->seen_listen) {
        return set_err(ctx, "duplicate listen");
    }
    if(parse_ip_port(tok[1], &ctx->cfg->listen) != 0) {
        return set_err(ctx, "listen must be <ipv4>:<port> (no hostnames), got '%s'", tok[1]);
    }
    ctx->seen_listen = 1;
    return 0;
}

static int parse_workers(struct parse_ctx *ctx, char **tok, int ntok)
{
    long v;
    if(ntok != 2) {
        return set_err(ctx, "workers needs 1 argument, got %d", ntok - 1);
    }
    if(parse_long(tok[1], 1, LB_MAX_WORKERS, &v) != 0) {
        return set_err(ctx, "workers must be 1..%d, got '%s'", LB_MAX_WORKERS, tok[1]);
    }
    ctx->cfg->workers = (int)v;
    return 0;
}

static int parse_algorithm(struct parse_ctx *ctx, char **tok, int ntok)
{
    if(ntok != 2) {
        return set_err(ctx, "algorithm needs 1 argument, got %d", ntok - 1);
    }

    if(strcmp(tok[1], "rr") == 0) {
        ctx->cfg->algorithm = LB_ALGO_RR;
    } else if (strcmp(tok[1], "leastconn") == 0) {
        ctx->cfg->algorithm = LB_ALGO_LEASTCONN;
    } else if (strcmp(tok[1], "iphash") == 0) {
        ctx->cfg->algorithm = LB_ALGO_IPHASH;
    } else {
        return set_err(ctx, "algorithm must be rr, leastconn or iphash, got '%s'", tok[1]);
    }
    return 0;
}

static int parse_backend(struct parse_ctx *ctx, char **tok, int ntok)
{
    struct lb_config *c = ctx->cfg;
    struct lb_config_backend b;
    memset(&b, 0, sizeof b);

    if(ntok != 3 && ntok != 5) {
        return set_err(ctx, "backend needs <name> <ip>:<port> [weight N]");
    }

    if(c->n_backends >= LB_MAX_BACKENDS) {
        return set_err(ctx, "too many backends (max %d)", LB_MAX_BACKENDS);
    }

    if(strlen(tok[1]) >= LB_NAME_LEN) {
        return set_err(ctx, "backend name '%s' too long (max %d characters)", tok[1], LB_NAME_LEN - 1);
    }
    for(int i = 0; i < c->n_backends; i++) {
        if (strcmp(c->backends[i].name, tok[1]) == 0) {
            return set_err(ctx, "duplicate backend name '%s'", tok[1]);
        }
    }
    snprintf(b.name, sizeof b.name, "%s", tok[1]);

    if(parse_ip_port(tok[2], &b.addr) != 0) {
        return set_err(ctx, "backend address must be <ipv4>:<port> (no hostnames), got '%s'", tok[2]);
    }

    b.weight = 1;
    if(ntok == 5) {
        long w;
        if (strcmp(tok[3], "weight") != 0) {
            return set_err(ctx, "expected 'weight', got '%s'", tok[3]);
        }
        if (parse_long(tok[4], 1, 1000, &w) != 0) {
            return set_err(ctx, "weight must be 1..1000, got '%s'", tok[4]);
        }
        b.weight = (uint32_t)w;
    }

    c->backends[c->n_backends] = b;
    c->n_backends++;
    return 0;
}


#include <limits.h>
static int parse_health(struct parse_ctx *ctx, char **tok, int ntok)
{
    long interval, timeout, rise, fall;

    if(ntok != 5) {
        return set_err(ctx, "health needs 4 arguments: <interval_ms> <timeout_ms> <rise> <fall>, got %d", ntok - 1);
    }
    if(parse_long(tok[1], 1, LONG_MAX, &interval) != 0) {
        return set_err(ctx, "health interval_ms must be > 0, got '%s'", tok[1]);
    }
    if(parse_long(tok[2], 1, LONG_MAX, &timeout) != 0) {
        return set_err(ctx, "health timeout_ms must be > 0, got '%s'", tok[2]);
    }
    if(parse_long(tok[3], 1, INT_MAX, &rise) != 0) {
        return set_err(ctx, "health rise must be >= 1, got '%s'", tok[3]);
    }
    if(parse_long(tok[4], 1, INT_MAX, &fall) != 0) {
        return set_err(ctx, "health fall must be >= 1, got '%s'", tok[4]);
    }
    if(timeout >= interval) {
        return set_err(ctx, "health timeout_ms (%ld) must be less than interval_ms (%ld)", timeout, interval);
    }

    ctx->cfg->hc_interval_ms = interval;
    ctx->cfg->hc_timeout_ms  = timeout;
    ctx->cfg->hc_rise        = (int)rise;
    ctx->cfg->hc_fall        = (int)fall;
    return 0;
}

static int parse_timeout(struct parse_ctx *ctx, char **tok, int ntok)
{
    long ms;

    if(ntok != 3) {
        return set_err(ctx, "timeout needs 2 arguments: connect|idle <ms>, got %d", ntok - 1);
    }
    if(parse_long(tok[2], 1, LONG_MAX, &ms) != 0) {
        return set_err(ctx, "timeout %s must be > 0 ms, got '%s'", tok[1], tok[2]);
    }

    if(strcmp(tok[1], "connect") == 0) {
        ctx->cfg->connect_timeout_ms = ms;
    } else if(strcmp(tok[1], "idle") == 0) {
        ctx->cfg->idle_timeout_ms = ms;
    } else {
        return set_err(ctx, "timeout must be 'connect' or 'idle', got '%s'", tok[1]);
    }
    return 0;

}

static int parse_admin_socket(struct parse_ctx *ctx, char **tok, int ntok)
{
    if(ntok != 2) {
        return set_err(ctx, "admin_socket needs 1 argument, got %d", ntok - 1);
    }
    if(strlen(tok[1]) >= LB_ADMIN_PATH_LEN) {
        return set_err(ctx, "admin_socket path too long (max %d characters)", LB_ADMIN_PATH_LEN - 1);
    }

    snprintf(ctx->cfg->admin_socket, sizeof ctx->cfg->admin_socket, "%s", tok[1]);  // strncpy
    return 0;
}


/* Cauta tok[0] in tabela si apeleaza handler-ul. */
static int dispatch(struct parse_ctx *ctx, char **tok, int ntok)
{
    if (strcmp(tok[0], "listen") == 0)       return parse_listen(ctx, tok, ntok);
    if (strcmp(tok[0], "workers") == 0)      return parse_workers(ctx, tok, ntok);
    if (strcmp(tok[0], "algorithm") == 0)    return parse_algorithm(ctx, tok, ntok);
    if (strcmp(tok[0], "backend") == 0)      return parse_backend(ctx, tok, ntok);
    if (strcmp(tok[0], "health") == 0)       return parse_health(ctx, tok, ntok);
    if (strcmp(tok[0], "timeout") == 0)      return parse_timeout(ctx, tok, ntok);
    if (strcmp(tok[0], "admin_socket") == 0) return parse_admin_socket(ctx, tok, ntok);

    return set_err(ctx, "unknown directive '%s'", tok[0]);
}

static int validate(struct parse_ctx *ctx)
{
    if (!ctx->seen_listen) {
        return set_err(ctx, "missing required directive 'listen'");
    }
    if (ctx->cfg->n_backends == 0) {
        return set_err(ctx, "at least one 'backend' is required");
    }
    return 0;
}

static int parse_line(struct parse_ctx *ctx, char *line)
{
    char *tok[MAX_TOKENS];

    char *hash = strchr(line, '#');
    if (hash != NULL) {
        *hash = '\0';
    }

    int ntok = tokenize(line, tok, MAX_TOKENS);
    if (ntok == 0) {
        return 0;
    }
    if (ntok < 0) {
        return set_err(ctx, "too many words on line");
    }
    return dispatch(ctx, tok, ntok);
}

int lb_config_load(const char *path, struct lb_config *out, char *err, size_t errlen)
{
    struct lb_config tmp;
    config_defaults(&tmp);
    struct parse_ctx ctx = { .path = path, .err = err, .errlen = errlen, .cfg = &tmp };

    FILE *fp = fopen(path, "re");
    if (fp == NULL) {
        return set_err(&ctx, "cannot open: %s", strerror(errno));
    }

    char  *line = NULL;
    size_t cap  = 0;
    int    ret  = 0;
    while (ret == 0 && getline(&line, &cap, fp) != -1) {
        ctx.lineno++;
        ret = parse_line(&ctx, line);
    }
    if (ret == 0 && ferror(fp)) {
        ret = set_err(&ctx, "read error");
    }
    free(line);
    fclose(fp);

    if (ret == 0) {
        ret = validate(&ctx);
    }
    if (ret == 0) {
        *out = tmp;
    }
    return ret;
}
