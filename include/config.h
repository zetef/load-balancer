#ifndef LB_CONFIG_H
  #define LB_CONFIG_H

  #include <stdint.h>
  #include <netinet/in.h>
  #include "lb_shm.h"
  #define LB_ADMIN_PATH_LEN 108

  struct lb_config_backend {
      char               name[LB_NAME_LEN];
      struct sockaddr_in addr;
      uint32_t           weight;
  };

  //De ce nu avem pointeri?
  //La SIGHUP, masterul parsează din nou fișierul într-un struct lb_config nou și apoi face cfg_old = cfg_new;. 
  //În C, o atribuire de structuri copiază octet cu octet. Cu char *name, s-ar copia doar adresa, nu textul, 
  //și ambele structuri ar arăta spre același text.
  struct lb_config {
    struct sockaddr_in listen;
    int workers;
    enum lb_algorithm algorithm;
    struct lb_config_backend backends[LB_MAX_BACKENDS];
    int n_backends;
    long hc_interval_ms;
    long hc_timeout_ms;
    int hc_rise;
    int hc_fall;
    long connect_timeout_ms;
    long idle_timeout_ms;
    char admin_socket[LB_ADMIN_PATH_LEN];
};

int parse_long(const char *s, long min, long max, long *out);
int parse_ip_port(const char *s, struct sockaddr_in *out);

#endif


/*
   *   directive     arguments                                 required  default        range
   *   listen        <ip>:<port>                               yes       -              IPv4, port 1..65535
   *   workers       <n>                                       no        1              1..64
   *   algorithm     rr | leastconn | iphash                   no        rr             -
   *   backend       <name> <ip>:<port> [weight N]             yes       weight 1       max 64, weight 1..1000
   *   health        <interval_ms> <timeout_ms> <rise> <fall>  no        2000 500 2 3   > 0, timeout < interval, rise/fall >= 1
   *   timeout       connect <ms>                              no        500            > 0
   *   timeout       idle <ms>                                 no        30000          > 0
   *   admin_socket  <path>                                    no        /tmp/lb.sock   < 108 bytes with '\0'
*/