/* Host test for pixos-bluetool's core: fake transport (scripted controller) + fake XPC dictionary layer. Prints RESULT: PASS|FAIL. */
#include "bt_tool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cases, fails;
#define CHECK(c) do { cases++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* ---- fake dictionary ---- */
static struct {
	const char *id, *script, *btaddr;
	int has_args;
	long long result, mfr;
	char fw[100], output[20];
	int have_result, have_mfr, have_fw, have_out;
} D;

static const char *d_req_str(void *c, const char *k) { (void)c; return strcmp(k, "kMsgId") == 0 ? D.id : NULL; }
static int d_has(void *c, const char *k) { (void)c; return strcmp(k, "kMsgArgs") == 0 && D.has_args; }
static const char *d_arg(void *c, const char *k)
{
	(void)c;
	if (strcmp(k, "script") == 0)
		return D.script;
	if (strcmp(k, "btaddress") == 0)
		return D.btaddr;
	return NULL;
}
static void d_i64(void *c, const char *k, int64_t v)
{
	(void)c;
	if (!strcmp(k, "result")) { D.result = v; D.have_result++; }
	if (!strcmp(k, "manufacturer")) { D.mfr = v; D.have_mfr++; }
}
static void d_str(void *c, const char *k, const char *v)
{
	(void)c;
	if (!strcmp(k, "firmware")) { snprintf(D.fw, sizeof(D.fw), "%s", v); D.have_fw++; }
	if (!strcmp(k, "output")) { snprintf(D.output, sizeof(D.output), "%s", v); D.have_out++; }
}
static const struct btt_dict_ops dops = { d_req_str, d_has, d_arg, d_i64, d_str };

/* ---- fake controller ---- */
static struct {
	int open_rc, opened, closes, opens, termios;
	unsigned sleeps, busy_left;
	unsigned now;
	int version_rev, version_status, silent;	/* silent: never answer */
	int noise_before;	/* unrelated events before each Command Complete */
	int split;		/* deliver the response one byte at a time */
	int addr_mismatch, write_status, reset_status, acl, readfail, writefail;
	uint8_t stored[6];
	uint8_t out[600];
	size_t outn, outpos;
	unsigned sent_ops[32];
	int nsent;
	char log[8192];
} T;

static void out_put(const uint8_t *b, size_t n) { memcpy(T.out + T.outn, b, n); T.outn += n; }
static void cc(uint16_t op, const uint8_t *rp, size_t n)
{
	uint8_t h[6] = { 0x04, 0x0e, (uint8_t)(3 + n), 1, (uint8_t)op, (uint8_t)(op >> 8) };
	int i;

	for (i = 0; i < T.noise_before; i++) {
		uint8_t nz[] = { 0x04, 0x05, 4, 0, 0, 0, 0 };	/* some unrelated event */
		out_put(nz, sizeof(nz));
	}
	out_put(h, 6);
	out_put(rp, n);
}

static int t_open(void *c) { (void)c; T.opens++; if (T.open_rc) return T.open_rc; T.opened = 1; return 0; }
static void t_close(void *c) { (void)c; if (T.opened) T.closes++; T.opened = 0; }
static void t_termios(void *c) { (void)c; T.termios++; }
static int t_write(void *c, const uint8_t *b, size_t n)
{
	uint16_t op;

	(void)c;
	if (T.writefail)
		return -1;
	if (n < 4 || b[0] != 1 || b[3] != n - 4) { printf("bad H4 cmd framing\n"); fails++; return (int)n; }
	op = (uint16_t)(b[1] | b[2] << 8);
	T.sent_ops[T.nsent++] = op;
	if (T.silent)
		return (int)n;
	if (T.acl) { uint8_t a[] = { 0x02, 0, 0, 0, 0 }; out_put(a, 5); return (int)n; }
	switch (op) {
	case 0x1001: {
		uint8_t rp[9] = { (uint8_t)T.version_status, 0x0b, (uint8_t)T.version_rev, (uint8_t)(T.version_rev >> 8), 0x0b, 0x0f, 0, 0x01, 0x02 };
		cc(op, rp, 9);
		break;
	}
	case 0xfc01: {
		uint8_t rp[1] = { (uint8_t)T.write_status };
		memcpy(T.stored, b + 4, 6);
		cc(op, rp, 1);
		break;
	}
	case 0x1009: {
		uint8_t rp[7];
		rp[0] = 0;
		memcpy(rp + 1, T.stored, 6);
		if (T.addr_mismatch)
			rp[1] ^= 0xff;
		cc(op, rp, 7);
		break;
	}
	case 0x0c03: {
		uint8_t rp[1] = { (uint8_t)T.reset_status };
		cc(op, rp, 1);
		break;
	}
	default:
		break;
	}
	return (int)n;
}
static int t_read(void *c, uint8_t *b, size_t max, unsigned ms)
{
	size_t n = T.outn - T.outpos;

	(void)c;
	if (T.readfail)
		return -1;
	if (n == 0) { T.now += ms; return 0; }
	if (T.split)
		n = 1;
	if (n > max)
		n = max;
	memcpy(b, T.out + T.outpos, n);
	T.outpos += n;
	return (int)n;
}
static unsigned t_now(void *c) { (void)c; return T.now; }
static void t_sleep(void *c, unsigned ms) { (void)c; T.now += ms; T.sleeps++; if (T.busy_left && --T.busy_left == 0) T.open_rc = 0; }
static const struct btt_tx_ops tops = { t_open, t_close, t_termios, t_write, t_read, t_now, t_sleep };

static void lg(void *c, const char *l)
{
	(void)c;
	strncat(T.log, l, sizeof(T.log) - strlen(T.log) - 2);
	strcat(T.log, "\n");
}

static struct btt_env env = { &dops, NULL, &tops, NULL, lg, NULL };

static void reset(void)
{
	memset(&D, 0, sizeof(D));
	memset(&T, 0, sizeof(T));
	T.version_rev = 0x20cb;
	D.id = "RunBuiltinScript";
	D.has_args = 1;
}
static int logged(const char *s) { return strstr(T.log, s) != NULL; }
static int sent(unsigned op) { int i; for (i = 0; i < T.nsent; i++) if (T.sent_ops[i] == op) return 1; return 0; }

int
main(void)
{
	uint8_t a[6], buf[16], rp[16];
	size_t l;
	unsigned o;

	/* parse */
	CHECK(btt_parse_btaddress("00:1A:2b:3C:4d:5E", a) == 0 && a[0] == 0 && a[1] == 0x1a && a[2] == 0x2b && a[5] == 0x5e);
	CHECK(btt_parse_btaddress("00:1A:2B:3C:4D", a) < 0);
	CHECK(btt_parse_btaddress("00:1A:2B:3C:4D:5E:", a) < 0);
	CHECK(btt_parse_btaddress("00-1A-2B-3C-4D-5E", a) < 0);
	CHECK(btt_parse_btaddress("0G:1A:2B:3C:4D:5E", a) < 0);
	CHECK(btt_parse_btaddress("", a) < 0 && btt_parse_btaddress(NULL, a) < 0);
	/* framing */
	CHECK(btt_h4_cmd(buf, 0xfc01, (const uint8_t *)"\1\2\3\4\5\6", 6) == 10 && buf[0] == 1 && buf[1] == 1 && buf[2] == 0xfc && buf[3] == 6 && buf[9] == 6);
	CHECK(btt_h4_cmd(buf, 0x1001, NULL, 0) == 4 && buf[1] == 0x01 && buf[2] == 0x10 && buf[3] == 0);

	/* boot ok, with address */
	reset(); D.script = "boot"; D.btaddr = "02:11:22:33:44:55";
	CHECK(btt_handle(&env) == 0 && D.result == 0 && D.mfr == 15 && strstr(D.fw, "rev 0x20cb") && strstr(D.fw, "build 203"));
	CHECK(T.opens == 1 && T.closes == 1 && T.termios == 1 && !T.opened);
	CHECK(T.stored[0] == 0x55 && T.stored[5] == 0x02);	/* LSB first */
	CHECK(sent(0x1001) && sent(0xfc01) && sent(0x1009) && !sent(0x0c03) && T.nsent == 3);
	CHECK(logged("BT GAP: boot: sleep mode 0xfc27") && logged("calibration") && logged("local name") && logged("baud"));
	CHECK(!logged("02:11") && !logged("55:44"));	/* no address in logs */
	CHECK(D.have_result == 1 && D.have_mfr == 1 && D.have_fw == 1);
	/* boot ok, without address */
	reset(); D.script = "boot";
	CHECK(btt_handle(&env) == 0 && !sent(0xfc01) && logged("no btaddress argument") && T.closes == 1);
	/* noise events + byte-wise delivery */
	reset(); D.script = "boot"; D.btaddr = "02:11:22:33:44:55"; T.noise_before = 5; T.split = 1;
	CHECK(btt_handle(&env) == 0 && logged("(5 other events)"));
	/* wrong revs */
	reset(); D.script = "boot"; T.version_rev = 0x2000;
	CHECK(btt_handle(&env) == 1 && D.result == 1 && D.mfr == 0 && D.fw[0] == 0 && logged("BT GAP: HCI revision 0x2000") && T.closes == 1 && !sent(0xfc01));
	reset(); D.script = "boot"; T.version_rev = 0x1234;
	CHECK(btt_handle(&env) == 1);
	reset(); D.script = "boot"; T.version_status = 0x01;
	CHECK(btt_handle(&env) == 1 && logged("controller status 0x01") && T.closes == 1);
	/* malformed / zero address: nothing opened */
	reset(); D.script = "boot"; D.btaddr = "garbage";
	CHECK(btt_handle(&env) == 1 && T.opens == 0 && logged("malformed btaddress"));
	reset(); D.script = "boot"; D.btaddr = "00:00:00:00:00:00";
	CHECK(btt_handle(&env) == 1 && T.opens == 0 && logged("all-zero"));
	/* readback mismatch, write status, timeout, open failure */
	reset(); D.script = "boot"; D.btaddr = "02:11:22:33:44:55"; T.addr_mismatch = 1;
	CHECK(btt_handle(&env) == 1 && logged("readback does not match") && T.closes == 1);
	reset(); D.script = "boot"; D.btaddr = "02:11:22:33:44:55"; T.write_status = 0x12;
	CHECK(btt_handle(&env) == 1 && !sent(0x1009) && T.closes == 1);
	reset(); D.script = "boot"; T.silent = 1;
	CHECK(btt_handle(&env) == 1 && logged("no Command Complete within 2000 ms") && T.now >= 2000 && T.closes == 1);
	reset(); D.script = "boot"; T.open_rc = -1;
	CHECK(btt_handle(&env) == 1 && logged("cannot open com.apple.uart.bluetooth") && T.closes == 0 && T.nsent == 0 && T.now >= 25000 && T.sleeps >= 50);
	/* 2026-11-11 bt-def1: the kernel bring-up refuses the control (EBUSY) for ~42 s; boot waits and then succeeds (no spin) */
	reset(); D.script = "boot"; T.open_rc = -1; T.busy_left = 6;
	CHECK(btt_handle(&env) == 0 && logged("controller busy") && logged("control opened after 6 retries") && T.sleeps == 6 && T.closes == 1);
	reset(); D.script = "boot"; T.acl = 1;
	CHECK(btt_handle(&env) == 1 && logged("unexpected H4 packet type") && T.closes == 1);
	reset(); D.script = "boot"; T.readfail = 1;
	CHECK(btt_handle(&env) == 1 && logged("UART read failed") && T.closes == 1);
	reset(); D.script = "boot"; T.writefail = 1;
	CHECK(btt_handle(&env) == 1 && logged("UART write failed") && T.closes == 1);
	/* init */
	reset(); D.script = "init";
	CHECK(btt_handle(&env) == 0 && D.result == 0 && D.mfr == 15 && T.opens == 0 && T.nsent == 0);
	/* deepsleep */
	reset(); D.script = "deepsleep";
	CHECK(btt_handle(&env) == 0 && sent(0x0c03) && T.nsent == 1 && logged("BT GAP: deepsleep") && T.closes == 1);
	reset(); D.script = "deepsleep"; T.reset_status = 3;
	CHECK(btt_handle(&env) == 1 && D.result == 1 && T.closes == 1);
	reset(); D.script = "deepsleep"; T.silent = 1;
	CHECK(btt_handle(&env) == 1);
	/* unknown script, missing args, unknown ids, RunCommand */
	reset(); D.script = "wipe";
	CHECK(btt_handle(&env) == 1 && logged("unknown script") && T.opens == 0);
	reset(); D.has_args = 0;
	CHECK(btt_handle(&env) == 1 && logged("without kMsgArgs.script"));
	reset(); D.script = NULL;
	CHECK(btt_handle(&env) == 1);
	reset(); D.id = "RunExternalScript";
	CHECK(btt_handle(&env) == 1 && logged("BT GAP: kMsgId 'RunExternalScript'") && D.have_result == 1 && T.opens == 0);
	reset(); D.id = "RunCommand";
	CHECK(btt_handle(&env) == 1 && D.result == 1 && D.have_out == 1 && D.output[0] == 0 && logged("BT GAP: RunCommand"));
	reset(); D.id = NULL;
	CHECK(btt_handle(&env) == 1 && logged("without kMsgId"));
	/* never 0xa / 0xd; the allowlist refuses other opcodes */
	reset(); D.script = "boot"; T.silent = 1;
	CHECK(btt_handle(&env) != 10 && btt_handle(&env) != 13);
	reset();
	CHECK(btt_hci_cmd(&env, 0xfc4c, NULL, 0, rp, sizeof(rp), &l, &o) == -1 && T.nsent == 0 && logged("not on the allowlist"));
	CHECK(btt_hci_cmd(&env, 0xfc01, NULL, 0, rp, sizeof(rp), &l, &o) == -1);	/* right opcode, wrong length */
	CHECK(btt_hci_cmd(&env, 0x0c13, NULL, 0, rp, sizeof(rp), &l, &o) == -1 && T.nsent == 0);

	printf("RESULT: %s pixos-bluetool-host cases=%d\n", fails ? "FAIL" : "PASS", cases);
	return fails ? 1 : 0;
}
