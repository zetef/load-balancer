#include <assert.h>
#include <stdio.h>

#include "config.h"

int main(void)
{
	 /* parse_long */
        long v;
        assert(parse_long("8080", 1, 65535, &v) == 0 && v == 8080);
        assert(parse_long("1", 1, 65535, &v) == 0 && v == 1);          		/* lower bound */
        assert(parse_long("65535", 1, 65535, &v) == 0 && v == 65535);  		/* upper bound */
        assert(parse_long("", 1, 65535, &v) == -1);                    		/* no digits */
        assert(parse_long("abc", 0, 65535, &v) == -1);                 		/* no digits, min 0 */
        assert(parse_long("80a", 1, 65535, &v) == -1);                 		/* trailing junk */
        assert(parse_long("99999999999999999999", 1, 65535, &v) == -1); 	/* overflow */
        assert(parse_long("0", 1, 65535, &v) == -1);                   		/* below min */
        assert(parse_long("-1", 1, 65535, &v) == -1);                  		/* negative */
        assert(parse_long("70000", 1, 65535, &v) == -1);               		/* above max */

        v = 42;
        assert(parse_long("70000", 1, 65535, &v) == -1 && v == 42);    		/* *out untouched on error */

        /* parse_ip_port */
        struct sockaddr_in a;
        assert(parse_ip_port("127.0.0.1:9001", &a) == 0);
        assert(a.sin_family == AF_INET);
        assert(ntohs(a.sin_port) == 9001);
        assert(ntohl(a.sin_addr.s_addr) == 0x7f000001);                		/* 127.0.0.1 */

        assert(parse_ip_port("1.2.3.4", &a) == -1);                    		/* no port */
        assert(parse_ip_port("1.2.3.4:", &a) == -1);                   		/* empty port */
        assert(parse_ip_port(":80", &a) == -1);                        		/* empty ip */
        assert(parse_ip_port("256.1.1.1:80", &a) == -1);               		/* bad octet */
        assert(parse_ip_port("localhost:80", &a) == -1);               		/* hostname refused */
        assert(parse_ip_port("1.2.3.4:70000", &a) == -1);              		/* port out of range */
        assert(parse_ip_port("1111111111111111111.2.3.4:80", &a) == -1); 	/* ip too long */

        printf("all config helper tests passed\n");
        return 0;
}

//Build
//gcc -std=c11 -Wall -Wextra -D_GNU_SOURCE -Iinclude tests/unit/test_config_helpers.c src/master/config.c -o /tmp/test_config && /tmp/test_config