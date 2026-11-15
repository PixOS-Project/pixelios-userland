/* Host test for pixos-ping: the core against a fake network on a fake clock, plus a loopback echo through a real ICMP datagram
 * socket when the host allows one (Linux: net.ipv4.ping_group_range). A skipped loopback is printed, never silent.
 * Prints RESULT: PASS|FAIL ping_host_test. */
#include "pixos_ping_sock.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct scen {
	const char *name;
	const char *target;
	int count, interval_ms, timeout_ms, wait_s;
	int route_up_ms;               /* sends fail with ENETUNREACH until the fake clock reaches this, < 0 never */
	long reply_us[8];              /* per seq: delay of the reply, < 0 none */
	int iphdr;                     /* prefix replies with a 20 byte IP header (XNU) */
	int corrupt_seq;               /* flip a payload byte of this seq's reply, -1 none */
	int dup_seq;                   /* deliver this seq's reply twice, -1 none */
	int foreign;                   /* deliver an unrelated ICMP (echo request type 8) before the first reply */
	int send_err_seq;              /* send of this seq fails with EPERM, -1 none */
	int recv_err;                  /* recv fails with this errno once after the first send, 0 none */
	int want_rc;
	const char *want[8];           /* substrings of the output */
	const char *absent[3];
};

struct net {
	const struct scen *sc;
	long long clock;
	struct { long long at; unsigned char d[2048]; int len; } q[32];
	int nq, recv_err_done, set_target_rc;
	unsigned nonce;
};

static long long f_now(void *c) { return ((struct net *)c)->clock; }
static void f_sleep(void *c, int ms) { ((struct net *)c)->clock += ms * 1000LL; }
static int f_set_target(void *c, const char *ip)
{
	(void)c;
	return strcmp(ip, "192.0.2.1") == 0 ? 0 : -EINVAL;
}

static void enqueue(struct net *n, long long at, const unsigned char *icmp, int len, int iphdr)
{
	int off = 0;

	if (iphdr) {
		memset(n->q[n->nq].d, 0, 20);
		n->q[n->nq].d[0] = 0x45;
		n->q[n->nq].d[9] = 1;
		off = 20;
	}
	memcpy(n->q[n->nq].d + off, icmp, len);
	n->q[n->nq].len = len + off;
	n->q[n->nq++].at = at;
}

static int f_send(void *c, const unsigned char *pkt, size_t len)
{
	struct net *n = c;
	const struct scen *s = n->sc;
	unsigned seq = pkt[6] << 8 | pkt[7];
	unsigned char r[8 + PP_PAYLOAD];

	if (s->route_up_ms < 0 || n->clock < s->route_up_ms * 1000LL)
		return -ENETUNREACH;
	if ((int)seq == s->send_err_seq)
		return -EPERM;
	CHECK(len == 8 + PP_PAYLOAD && pkt[0] == 8 && pkt[1] == 0, "echo request header");
	CHECK(pp_cksum(pkt, len) == 0, "request checksum valid seq=%u", seq);
	if (s->foreign && seq == 0) {
		enqueue(n, n->clock + 100, pkt, (int)len, s->iphdr);       /* a request is not a reply: must be ignored */
	}
	if (seq < 8 && s->reply_us[seq] >= 0) {
		memcpy(r, pkt, len);
		r[0] = 0;                                                  /* echo reply, checksum recomputed */
		r[2] = r[3] = 0;
		unsigned k = pp_cksum(r, len);
		r[2] = k >> 8, r[3] = k;
		if ((int)seq == s->corrupt_seq)
			r[30] ^= 0xff;
		enqueue(n, n->clock + s->reply_us[seq], r, (int)len, s->iphdr);
		if ((int)seq == s->dup_seq)
			enqueue(n, n->clock + s->reply_us[seq] + 50, r, (int)len, s->iphdr);
	}
	return 0;
}

static int f_recv(void *c, unsigned char *buf, size_t cap, int timeout_ms)
{
	struct net *n = c;
	long long limit = n->clock + timeout_ms * 1000LL;
	int best = -1, i;

	if (n->sc->recv_err && !n->recv_err_done && n->nq) {
		n->recv_err_done = 1;
		return -n->sc->recv_err;
	}
	for (i = 0; i < n->nq; i++)
		if (n->q[i].at <= limit && (best < 0 || n->q[i].at < n->q[best].at))
			best = i;
	if (best < 0) {
		n->clock = limit;
		return 0;
	}
	if (n->q[best].at > n->clock)
		n->clock = n->q[best].at;
	(void)cap;
	memcpy(buf, n->q[best].d, n->q[best].len);
	i = n->q[best].len;
	n->q[best] = n->q[--n->nq];
	return i;
}

static const struct scen scenarios[] = {
	{ "all5", "192.0.2.1", 5, 1000, 2000, 5, 0, { 1200, 1000, 3000, 2000, 1800 }, 0, -1, -1, 0, -1, 0, 0,
	  { "sent=5 got=5 lost=0 rtt_us min/avg/max=1000/1800/3000 bad=0 ignored=0", "reply seq=2 rtt_us=3000", "RESULT pass gaps=0" },
	  { "WIFI GAP" } },
	{ "iphdr", "192.0.2.1", 3, 1000, 2000, 5, 0, { 500, 500, 500 }, 1, -1, -1, 1, -1, 0, 0,
	  { "sent=3 got=3 lost=0 rtt_us min/avg/max=500/500/500 bad=0 ignored=1", "RESULT pass gaps=0" }, { "WIFI GAP" } },
	{ "lost2", "192.0.2.1", 5, 1000, 2000, 5, 0, { 900, -1, 1100, -1, 1000 }, 0, -1, -1, 0, -1, 0, 0,
	  { "sent=5 got=3 lost=2 rtt_us min/avg/max=900/1000/1100", "RESULT pass gaps=0" }, { "WIFI GAP" } },
	{ "noreply", "192.0.2.1", 3, 1000, 2000, 5, 0, { -1, -1, -1 }, 0, -1, -1, 0, -1, 0, 1,
	  { "sent=3 got=0 lost=3 rtt_us min/avg/max=0/0/0", "WIFI GAP: ping no echo reply", "RESULT fail gaps=1" }, { "reply seq" } },
	{ "late", "192.0.2.1", 2, 1000, 2000, 5, 0, { 1500000, 800 }, 0, -1, -1, 0, -1, 0, 0,    /* seq 0 answers after seq 1 was sent */
	  { "sent=2 got=2 lost=0 rtt_us min/avg/max=800/750400/1500000" }, { "WIFI GAP" } },
	{ "routewait", "192.0.2.1", 2, 1000, 2000, 10, 3000, { 700, 700 }, 0, -1, -1, 0, -1, 0, 0,
	  { "route ready after 3000 ms", "sent=2 got=2", "RESULT pass gaps=0" }, { "WIFI GAP" } },
	{ "noroute", "192.0.2.1", 2, 1000, 2000, 2, -1, { 700, 700 }, 0, -1, -1, 0, -1, 0, 1,
	  { "WIFI GAP: ping no route after 2 s (errno 101)", "sent=0 got=0 lost=0", "RESULT fail gaps=1" }, { "reply seq" } },
	{ "badtarget", "198.51.100.9", 2, 1000, 2000, 2, 0, { 700, 700 }, 0, -1, -1, 0, -1, 0, 2,
	  { "WIFI GAP: ping target is not an IPv4 address", "RESULT fail gaps=1" }, { "reply seq" } },
	{ "notarget", NULL, 2, 1000, 2000, 2, 0, { 700, 700 }, 0, -1, -1, 0, -1, 0, 2,
	  { "WIFI GAP: ping no target", "RESULT fail gaps=1" }, { "reply seq" } },
	{ "badcksum", "192.0.2.1", 3, 1000, 2000, 5, 0, { 600, 600, 600 }, 0, 1, -1, 0, -1, 0, 0,
	  { "sent=3 got=2 lost=1", "bad=1", "WIFI GAP: ping 1 reply(ies) with a bad checksum", "RESULT pass gaps=1" }, { "reply seq=1 " } },
	{ "dup", "192.0.2.1", 2, 1000, 2000, 5, 0, { 600, 600 }, 0, -1, 0, 0, -1, 0, 0,
	  { "sent=2 got=2 lost=0", "ignored=1", "RESULT pass gaps=0" }, { "WIFI GAP" } },
	{ "senderr", "192.0.2.1", 3, 1000, 2000, 5, 0, { 600, 600, 600 }, 0, -1, -1, 0, 1, 0, 0,
	  { "WIFI GAP: ping send seq=1 failed errno=1", "sent=2 got=2 lost=0", "RESULT pass gaps=1" }, { "reply seq=1 " } },
	{ "recverr", "192.0.2.1", 2, 1000, 2000, 5, 0, { 600, 600 }, 0, -1, -1, 0, -1, EIO, 0,
	  { "WIFI GAP: ping recv failed errno=5", "RESULT pass gaps=1" }, { "ZZZ" } },
};

static const char *g_env[8][2];
static const char *fenv(const char *k)
{
	int i;

	for (i = 0; i < 8 && g_env[i][0]; i++)
		if (strcmp(g_env[i][0], k) == 0)
			return g_env[i][1];
	return NULL;
}

static char *run_capture(const struct pp_ops *ops, const struct scen *s, int *rc)
{
	static char buf[16384];
	struct pp_cfg cfg = { s->count, s->interval_ms, s->timeout_ms, s->wait_s };
	FILE *f = fmemopen(buf, sizeof buf, "w");

	memset(buf, 0, sizeof buf);
	*rc = pp_run(ops, &cfg, s->target, 0x1234, f);
	fclose(f);
	return buf;
}

static void test_scenarios(void)
{
	size_t i;
	int j;

	for (i = 0; i < sizeof scenarios / sizeof *scenarios; i++) {
		const struct scen *s = &scenarios[i];
		struct net n;
		struct pp_ops ops = { &n, f_now, f_sleep, f_set_target, f_send, f_recv };
		int rc;
		char *out;

		memset(&n, 0, sizeof n);
		n.sc = s;
		out = run_capture(&ops, s, &rc);
		CHECK(rc == s->want_rc, "%s: rc %d want %d\n%s", s->name, rc, s->want_rc, out);
		for (j = 0; j < 8 && s->want[j]; j++)
			CHECK(strstr(out, s->want[j]) != NULL, "%s: missing '%s'\n%s", s->name, s->want[j], out);
		for (j = 0; j < 3 && s->absent[j]; j++)
			CHECK(strstr(out, s->absent[j]) == NULL, "%s: unexpected '%s'\n%s", s->name, s->absent[j], out);
	}
}

static void test_main_env(void)
{
	struct scen s = scenarios[0];
	struct net n;
	struct pp_ops ops = { &n, f_now, f_sleep, f_set_target, f_send, f_recv };
	static char buf[16384];
	char *argv[] = { "pixos-ping", NULL };
	FILE *f;
	int rc;

	memset(&n, 0, sizeof n);
	n.sc = &s;
	g_env[0][0] = "PIXOS_PING_TARGET", g_env[0][1] = "192.0.2.1";
	g_env[1][0] = "PIXOS_PING_COUNT", g_env[1][1] = "0";          /* invalid: loud gap, default 5 used */
	g_env[2][0] = "PIXOS_PING_TIMEOUT_MS", g_env[2][1] = "2000";
	memset(buf, 0, sizeof buf);
	f = fmemopen(buf, sizeof buf, "w");
	rc = pp_main(1, argv, &ops, fenv, "testbuild", 0x1234, f);
	fclose(f);
	CHECK(rc == 0, "env main rc %d\n%s", rc, buf);
	CHECK(strstr(buf, "ping start build=testbuild") != NULL, "start line\n%s", buf);
	CHECK(strstr(buf, "WIFI GAP: ping PIXOS_PING_COUNT=0 is not an integer in 1..100, using 5") != NULL, "count gap\n%s", buf);
	CHECK(strstr(buf, "configuration value(s) ignored") != NULL, "config summary gap\n%s", buf);
	CHECK(strstr(buf, "sent=5 got=5") != NULL, "default count used\n%s", buf);
	CHECK(strstr(buf, "192.0.2.1") == NULL, "target address must never be printed\n%s", buf);
	memset(g_env, 0, sizeof g_env);
}

static void test_checksum(void)
{
	static const unsigned char v[] = { 0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7 };   /* RFC 1071 s.3: sum 0xddf2 */
	static const unsigned char odd[] = { 0x01, 0x02, 0x03 };

	CHECK(pp_cksum(v, sizeof v) == 0x220d, "RFC 1071 vector got %04x", pp_cksum(v, sizeof v));
	CHECK(pp_cksum(odd, sizeof odd) == (~(0x0102 + 0x0300) & 0xffff), "odd length");
}

static const char *test_loopback(void)
{
	struct pp_sock sock;
	struct pp_ops ops;
	static char buf[16384];
	struct pp_cfg cfg = { 3, 50, 1000, 2 };
	FILE *f;
	int rc;
	static char why[96];

	rc = pp_sock_open(&sock, &ops);
	if (rc < 0) {
		snprintf(why, sizeof why, "skipped(socket errno=%d, net.ipv4.ping_group_range?)", -rc);
		return why;
	}
	memset(buf, 0, sizeof buf);
	f = fmemopen(buf, sizeof buf, "w");
	rc = pp_run(&ops, &cfg, "127.0.0.1", 0x4242, f);
	fclose(f);
	CHECK(rc == 0, "loopback rc %d\n%s", rc, buf);
	CHECK(strstr(buf, "sent=3 got=3 lost=0") != NULL, "loopback summary\n%s", buf);
	CHECK(strstr(buf, "RESULT pass gaps=0") != NULL, "loopback result\n%s", buf);
	return "ok";
}

int main(void)
{
	const char *lb;

	test_checksum();
	test_scenarios();
	test_main_env();
	lb = test_loopback();
	if (failures) {
		printf("RESULT: FAIL ping_host_test failures=%d of %d checks\n", failures, checks);
		return 1;
	}
	printf("RESULT: PASS ping_host_test checks=%d loopback=%s\n", checks, lb);
	return 0;
}
