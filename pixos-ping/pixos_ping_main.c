/* pixos-ping entry point: `pixos-ping [IPv4]`; env PIXOS_PING_TARGET / _COUNT / _INTERVAL_MS / _TIMEOUT_MS / _WAIT_S. */
#include "pixos_ping_sock.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef PP_BUILD
#define PP_BUILD "dev"
#endif

static const char *genv(const char *name)
{
	return getenv(name);
}

int main(int argc, char **argv)
{
	struct pp_sock sock;
	struct pp_ops ops;
	int rc;

	setvbuf(stdout, NULL, _IOLBF, 0);
	rc = pp_sock_open(&sock, &ops);
	if (rc < 0) {
		printf("SWEET WLAN: ping start build=%s\n", PP_BUILD);
		printf("WIFI GAP: ping cannot open an ICMP datagram socket (errno %d)\n", -rc);
		printf("WIFI PING: RESULT fail gaps=1\n");
		return 1;
	}
	return pp_main(argc, argv, &ops, genv, PP_BUILD, (unsigned)(getpid() * 2654435761u ^ (unsigned)time(NULL)) & 0xffff, stdout);
}
