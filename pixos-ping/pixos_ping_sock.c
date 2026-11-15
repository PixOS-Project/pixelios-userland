/* pixos-ping: real backend (ICMP datagram socket, libc only). Builds for iOS (XNU) and for a Linux host (loopback test). */
#include "pixos_ping_sock.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static long long s_now(void *c)
{
	struct timespec ts;

	(void)c;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static void s_sleep(void *c, int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	(void)c;
	nanosleep(&ts, NULL);
}

static int s_set_target(void *c, const char *ip)
{
	struct pp_sock *s = c;

	memset(&s->dst, 0, sizeof s->dst);
	s->dst.sin_family = AF_INET;
	if (inet_pton(AF_INET, ip, &s->dst.sin_addr) != 1)
		return -EINVAL;
	return 0;
}

static int s_send(void *c, const unsigned char *pkt, size_t len)
{
	struct pp_sock *s = c;

	if (sendto(s->fd, pkt, len, 0, (struct sockaddr *)&s->dst, sizeof s->dst) != (ssize_t)len)
		return -errno;
	return 0;
}

static int s_recv(void *c, unsigned char *buf, size_t cap, int timeout_ms)
{
	struct pp_sock *s = c;
	struct pollfd p = { s->fd, POLLIN, 0 };
	int r = poll(&p, 1, timeout_ms);
	ssize_t n;

	if (r < 0)
		return errno == EINTR ? 0 : -errno;
	if (r == 0)
		return 0;
	n = recv(s->fd, buf, cap, 0);
	return n < 0 ? (errno == EINTR ? 0 : -errno) : (int)n;
}

int pp_sock_open(struct pp_sock *s, struct pp_ops *ops)
{
	memset(s, 0, sizeof *s);
	s->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
	if (s->fd < 0)
		return -errno;
	ops->ctx = s;
	ops->now_us = s_now;
	ops->sleep_ms = s_sleep;
	ops->set_target = s_set_target;
	ops->send = s_send;
	ops->recv = s_recv;
	return 0;
}
