#ifndef PIXOS_PING_SOCK_H
#define PIXOS_PING_SOCK_H
#include "pixos_ping.h"

#include <netinet/in.h>

struct pp_sock {
	int fd;
	struct sockaddr_in dst;
};

/* Opens the ICMP datagram socket and fills ops; returns 0 or -errno. */
int pp_sock_open(struct pp_sock *s, struct pp_ops *ops);
#endif
