#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include "config.h"

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
    if (inet_pton(AF_INET, ip, &tmp.sin_addr) != 1) {
        return -1;
    }
    
	
	*out = tmp;
    return 0;
}
