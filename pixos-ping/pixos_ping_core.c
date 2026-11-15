/* pixos-ping core: sequence, timing, parsing and log lines. See pixos_ping.h. */
#include "pixos_ping.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define MAGIC "PIXOSPNG"

unsigned pp_cksum(const unsigned char *p, size_t n)
{
	unsigned long sum = 0;

	for (; n > 1; p += 2, n -= 2)
		sum += (unsigned long)p[0] << 8 | p[1];
	if (n)
		sum += (unsigned long)p[0] << 8;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (unsigned)(~sum & 0xffff);
}

static void put32(unsigned char *p, unsigned v)
{
	p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v;
}

static unsigned get32(const unsigned char *p)
{
	return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
}

/* ICMP echo request: type 8 code 0 cksum id seq, payload = MAGIC + nonce + seq + counting pad */
static size_t build_echo(unsigned char *pkt, unsigned nonce, unsigned seq)
{
	size_t i;

	memset(pkt, 0, 8 + PP_PAYLOAD);
	pkt[0] = 8;
	pkt[4] = nonce >> 8, pkt[5] = nonce;                   /* id (the kernel does not rewrite it on XNU; Linux does) */
	pkt[6] = seq >> 8, pkt[7] = seq;
	memcpy(pkt + 8, MAGIC, 8);
	put32(pkt + 16, nonce);
	put32(pkt + 20, seq);
	for (i = 24; i < PP_PAYLOAD; i++)
		pkt[8 + i] = (unsigned char)i;
	i = pp_cksum(pkt, 8 + PP_PAYLOAD);
	pkt[2] = i >> 8, pkt[3] = i;
	return 8 + PP_PAYLOAD;
}

/* 1 = our echo reply (*seq set), 0 = not ours (other ICMP, wrong nonce, too short), -1 = ours but the checksum is wrong */
static int parse_reply(const unsigned char *b, int n, unsigned nonce, unsigned *seq)
{
	if (n >= 20 && (b[0] >> 4) == 4) {                     /* XNU delivers the IP header; an echo reply (type 0) never looks like this */
		int ihl = (b[0] & 15) * 4;

		if (ihl < 20 || ihl > n)
			return 0;
		b += ihl, n -= ihl;
	}
	if (n < 8 + 24 || b[0] != 0 || b[1] != 0)
		return 0;
	if (memcmp(b + 8, MAGIC, 8) != 0 || get32(b + 16) != nonce)
		return 0;
	if (pp_cksum(b, (size_t)n) != 0)
		return -1;
	*seq = get32(b + 20);
	return 1;
}

static int is_route_err(int e)
{
	return e == ENETUNREACH || e == EHOSTUNREACH || e == ENETDOWN || e == EADDRNOTAVAIL || e == ENOBUFS;
}

static void print_summary(FILE *out, int sent, int got, long long mn, long long sum, long long mx, int bad, int ign, int gaps)
{
	fprintf(out, "SWEET WLAN: ping summary sent=%d got=%d lost=%d rtt_us min/avg/max=%lld/%lld/%lld bad=%d ignored=%d\n",
		sent, got, sent - got, got ? mn : 0, got ? sum / got : 0, got ? mx : 0, bad, ign);
	fprintf(out, "WIFI PING: RESULT %s gaps=%d\n", got >= 1 ? "pass" : "fail", gaps);
}

int pp_run(const struct pp_ops *ops, const struct pp_cfg *cfg, const char *target, unsigned nonce, FILE *out)
{
	unsigned char pkt[8 + PP_PAYLOAD], buf[2048];
	long long sent_at[PP_MAX_COUNT], t0, mn = 0, mx = 0, sum = 0;
	char ok[PP_MAX_COUNT], answered[PP_MAX_COUNT];
	int i, rc, sent = 0, got = 0, bad = 0, ign = 0, gaps = 0, waited = 0, any_sent = 0;

	if (!target || !*target) {
		fputs("WIFI GAP: ping no target (set PIXOS_PING_TARGET or give an IPv4 address as argument)\n", out);
		print_summary(out, 0, 0, 0, 0, 0, 0, 0, 1);
		return 2;
	}
	rc = ops->set_target(ops->ctx, target);
	if (rc < 0) {
		fprintf(out, "WIFI GAP: ping target is not an IPv4 address (errno %d)\n", -rc);
		print_summary(out, 0, 0, 0, 0, 0, 0, 0, 1);
		return 2;
	}
	memset(ok, 0, sizeof ok);
	memset(answered, 0, sizeof answered);
	t0 = ops->now_us(ops->ctx);
	for (i = 0; i < cfg->count; i++) {
		long long deadline;
		size_t len = build_echo(pkt, nonce, (unsigned)i);

		for (;;) {                                         /* until the first send works the link may still be coming up */
			sent_at[i] = ops->now_us(ops->ctx);
			rc = ops->send(ops->ctx, pkt, len);
			if (rc >= 0 || any_sent || !is_route_err(-rc))
				break;
			if (sent_at[i] - t0 >= (long long)cfg->wait_s * 1000000LL) {
				fprintf(out, "WIFI GAP: ping no route after %d s (errno %d)\n", cfg->wait_s, -rc);
				gaps++;
				print_summary(out, 0, 0, 0, 0, 0, 0, 0, gaps);
				return 1;
			}
			waited = 1;
			ops->sleep_ms(ops->ctx, 500);
		}
		if (waited && !any_sent && rc >= 0) {
			fprintf(out, "SWEET WLAN: ping route ready after %lld ms\n", (sent_at[i] - t0) / 1000);
			waited = 0;
		}
		if (rc < 0) {
			fprintf(out, "WIFI GAP: ping send seq=%d failed errno=%d\n", i, -rc);
			gaps++;
		} else {
			sent++, any_sent = 1, ok[i] = 1;
		}
		deadline = sent_at[i] + (i == cfg->count - 1 ? cfg->timeout_ms : cfg->interval_ms) * 1000LL;
		for (;;) {                                         /* collect replies until the next probe is due */
			long long now = ops->now_us(ops->ctx), left = deadline - now;
			unsigned seq = 0;
			int n, pr;

			if (left <= 0)
				break;
			n = ops->recv(ops->ctx, buf, sizeof buf, (int)((left + 999) / 1000));
			if (n == 0)
				continue;
			if (n < 0) {
				fprintf(out, "WIFI GAP: ping recv failed errno=%d\n", -n);
				gaps++;
				ops->sleep_ms(ops->ctx, (int)(left / 1000));
				break;
			}
			pr = parse_reply(buf, n, nonce, &seq);
			if (pr < 0) {
				bad++;
			} else if (pr == 0 || seq >= (unsigned)cfg->count || !ok[seq] || answered[seq]) {
				ign++;
			} else {
				long long rtt = ops->now_us(ops->ctx) - sent_at[seq];

				answered[seq] = 1;
				if (!got || rtt < mn)
					mn = rtt;
				if (!got || rtt > mx)
					mx = rtt;
				sum += rtt, got++;
				fprintf(out, "SWEET WLAN: ping reply seq=%u rtt_us=%lld\n", seq, rtt);
			}
		}
	}
	if (bad) {
		fprintf(out, "WIFI GAP: ping %d reply(ies) with a bad checksum\n", bad);
		gaps++;
	}
	if (sent && !got) {
		fputs("WIFI GAP: ping no echo reply (check the link, the gateway address, ARP)\n", out);
		gaps++;
	}
	print_summary(out, sent, got, mn, sum, mx, bad, ign, gaps);
	return got >= 1 ? 0 : 1;
}

static int env_int(const char *(*genv)(const char *), const char *name, int def, int lo, int hi, int *gaps, FILE *out)
{
	const char *v = genv(name);
	char *end;
	long x;

	if (!v || !*v)
		return def;
	x = strtol(v, &end, 10);
	if (*end || x < lo || x > hi) {
		fprintf(out, "WIFI GAP: ping %s=%s is not an integer in %d..%d, using %d\n", name, v, lo, hi, def);
		(*gaps)++;
		return def;
	}
	return (int)x;
}

int pp_main(int argc, char **argv, const struct pp_ops *ops, const char *(*genv)(const char *), const char *build,
	    unsigned nonce, FILE *out)
{
	struct pp_cfg cfg = PP_DEFAULT_CFG;
	const char *target = argc > 1 ? argv[1] : genv("PIXOS_PING_TARGET");
	int cfg_gaps = 0, rc;

	fprintf(out, "SWEET WLAN: ping start build=%s\n", build);
	cfg.count = env_int(genv, "PIXOS_PING_COUNT", cfg.count, 1, PP_MAX_COUNT, &cfg_gaps, out);
	cfg.interval_ms = env_int(genv, "PIXOS_PING_INTERVAL_MS", cfg.interval_ms, 10, 10000, &cfg_gaps, out);
	cfg.timeout_ms = env_int(genv, "PIXOS_PING_TIMEOUT_MS", cfg.timeout_ms, 10, 30000, &cfg_gaps, out);
	cfg.wait_s = env_int(genv, "PIXOS_PING_WAIT_S", cfg.wait_s, 0, 600, &cfg_gaps, out);
	rc = pp_run(ops, &cfg, target, nonce, out);
	if (cfg_gaps)
		fprintf(out, "WIFI GAP: ping %d configuration value(s) ignored (see above)\n", cfg_gaps);
	return rc;
}
