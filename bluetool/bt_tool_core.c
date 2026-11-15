/* pixos-bluetool core: see bt_tool.h. Sources: contract bt-skywalk-uart-channel.md s.6 [bin]; H4 framing and HCI opcodes/layouts from the
 * Bluetooth Core spec (Command packet 0x01 opcode(2 LE) plen; Event packet 0x04 code plen; Command Complete 0x0e ncmd opcode(2) status ...;
 * Command Status 0x0f status ncmd opcode(2)) [spec, read]; 0xfc01 layout as the kernel's patchram (sweet_bt_uart.c) and Linux btbcm.c [src]. */
#include "bt_tool.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void
say(const struct btt_env *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void
say(const struct btt_env *e, const char *fmt, ...)
{
	char b[200];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	e->log(e->lctx, b);
}
#define TOOL(e, ...) say((e), "BT TOOL: " __VA_ARGS__)
#define GAP(e, ...) say((e), "BT GAP: " __VA_ARGS__)

/* The independent allowlist: opcode -> exact parameter length. Anything else is refused before a byte leaves. */
static const struct { uint16_t op; uint8_t plen; } allow[] = {
	{ 0x0c03, 0 },	/* Reset */
	{ 0x1001, 0 },	/* Read Local Version Information */
	{ 0x1009, 0 },	/* Read BD_ADDR */
	{ 0xfc01, 6 },	/* Broadcom Write_BD_ADDR */
};

static int
hexv(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

int
btt_parse_btaddress(const char *s, uint8_t out[6])
{
	int i;

	if (!s || strlen(s) != 17)
		return -1;
	for (i = 0; i < 6; i++) {
		int h = hexv(s[i * 3]), l = hexv(s[i * 3 + 1]);

		if (h < 0 || l < 0 || (i < 5 && s[i * 3 + 2] != ':'))
			return -1;
		out[i] = (uint8_t)(h << 4 | l);
	}
	return 0;
}

size_t
btt_h4_cmd(uint8_t *buf, uint16_t opcode, const uint8_t *params, uint8_t plen)
{
	buf[0] = 0x01;
	buf[1] = (uint8_t)(opcode & 0xff);
	buf[2] = (uint8_t)(opcode >> 8);
	buf[3] = plen;
	if (plen)
		memcpy(buf + 4, params, plen);
	return 4u + plen;
}

int
btt_hci_cmd(const struct btt_env *e, uint16_t opcode, const uint8_t *params, uint8_t plen, uint8_t *rp, size_t rpmax, size_t *rplen,
    unsigned *other)
{
	uint8_t pkt[4 + 255], in[3 + 255];
	size_t have = 0, i, n;
	unsigned t0, el, oth = 0;
	int ok = 0, rc;

	if (other)
		*other = 0;
	for (i = 0; i < sizeof(allow) / sizeof(allow[0]); i++)
		if (allow[i].op == opcode && allow[i].plen == plen)
			ok = 1;
	if (!ok) {
		GAP(e, "opcode 0x%04x plen %u is not on the allowlist, not sent", opcode, plen);
		return -1;
	}
	n = btt_h4_cmd(pkt, opcode, params, plen);
	if (e->tx->write(e->tctx, pkt, n) != (int)n) {
		GAP(e, "opcode 0x%04x: UART write failed", opcode);
		return -2;
	}
	t0 = e->tx->now_ms(e->tctx);
	for (;;) {
		el = e->tx->now_ms(e->tctx) - t0;
		if (el >= BTT_CMD_TIMEOUT_MS) {
			GAP(e, "opcode 0x%04x: no Command Complete within %u ms (%u other events)", opcode, BTT_CMD_TIMEOUT_MS, oth);
			rc = -3;
			goto out;
		}
		/* complete any buffered event first */
		if (have >= 3 && in[0] == 0x04 && have >= 3u + in[2]) {
			size_t tot = 3u + in[2];
			uint8_t code = in[1];
			const uint8_t *p = in + 3;

			if (code == 0x0e && in[2] >= 4 && (uint16_t)(p[1] | p[2] << 8) == opcode) {
				size_t l = (size_t)in[2] - 3;	/* return parameters after ncmd + opcode */

				if (l > rpmax)
					l = rpmax;
				memcpy(rp, p + 3, l);
				if (rplen)
					*rplen = l;
				if (l == 0 || rp[0] != 0) {
					GAP(e, "opcode 0x%04x: controller status 0x%02x", opcode, l ? rp[0] : 0xff);
					rc = -5;
				} else {
					TOOL(e, "cmd 0x%04x status 00 (%u other events)", opcode, oth);
					rc = 0;
				}
				goto out;
			}
			if (code == 0x0f && in[2] == 4 && (uint16_t)(p[2] | p[3] << 8) == opcode && p[0] != 0) {
				rp[0] = p[0];
				if (rplen)
					*rplen = 1;
				GAP(e, "opcode 0x%04x: Command Status 0x%02x", opcode, p[0]);
				rc = -5;
				goto out;
			}
			oth++;
			memmove(in, in + tot, have - tot);
			have -= tot;
			continue;
		}
		if (have >= 1 && in[0] != 0x04) {
			GAP(e, "opcode 0x%04x: unexpected H4 packet type 0x%02x (only events are handled)", opcode, in[0]);
			rc = -4;
			goto out;
		}
		{
			int r = e->tx->read(e->tctx, in + have, sizeof(in) - have, BTT_CMD_TIMEOUT_MS - el);

			if (r < 0) {
				GAP(e, "opcode 0x%04x: UART read failed", opcode);
				rc = -4;
				goto out;
			}
			if (r == 0) {
				GAP(e, "opcode 0x%04x: no Command Complete within %u ms (%u other events)", opcode, BTT_CMD_TIMEOUT_MS, oth);
				rc = -3;
				goto out;
			}
			have += (size_t)r;
		}
	}
out:
	if (other)
		*other = oth;
	return rc;
}

/* ---- scripts ---- */
struct fwinfo {
	uint16_t rev;
	char fw[BTT_FW_MAX];
};

static void
fw_string(struct fwinfo *f)
{
	/* build = low byte of the HCI revision: 0x20cb -> 203, the pair the patchram log shows (build=203 hci_rev=0x20cb) [guess for other revs] */
	snprintf(f->fw, sizeof(f->fw), "BCM4389 BCM.hcd build %u rev 0x%04x", f->rev & 0xffu, f->rev);
}

/* 2026-11-11 (bt-def1 [obs]): with patch 0296 the kernel downloads the firmware in a thread (~42 s) and refuses the control with EBUSY
 * meanwhile; bluetoothd re-sends `boot` at once on result != 0 (no back-off), so an instant refusal spins hundreds of requests per second.
 * A `boot` therefore waits for the controller: retry the open every BTT_OPEN_RETRY_MS for up to BTT_OPEN_WAIT_MS (inside BlueTool's 30 s
 * budget, docs/research/bt-skywalk-uart-channel.md s.6), then answer 1 with a GAP. */
#define BTT_OPEN_WAIT_MS	25000u
#define BTT_OPEN_RETRY_MS	500u
static int
open_ctl(const struct btt_env *e)
{
	unsigned t0 = e->tx->now_ms(e->tctx), tries = 0;

	while (e->tx->open(e->tctx) != 0) {
		tries++;
		if (e->tx->now_ms(e->tctx) - t0 >= BTT_OPEN_WAIT_MS) {
			GAP(e, "cannot open com.apple.uart.bluetooth after %u tries in %u ms (busy or not registered); no reply result 0", tries, e->tx->now_ms(e->tctx) - t0);
			return -1;
		}
		if (tries == 1)
			say(e, "BT TOOL: controller busy (kernel bring-up?); waiting up to %u ms", BTT_OPEN_WAIT_MS);
		e->tx->sleep_ms(e->tctx, BTT_OPEN_RETRY_MS);
	}
	if (tries)
		say(e, "BT TOOL: control opened after %u retries (%u ms)", tries, e->tx->now_ms(e->tctx) - t0);
	e->tx->log_termios(e->tctx);
	return 0;
}

static int
read_version(const struct btt_env *e, struct fwinfo *f)
{
	uint8_t rp[16];
	size_t l = 0;
	unsigned o;
	int r = btt_hci_cmd(e, BTT_OP_READ_LOCAL_VERSION, NULL, 0, rp, sizeof(rp), &l, &o);

	if (r != 0)
		return -1;
	/* return parameters: status, HCI version, HCI revision (2 LE), LMP version, manufacturer (2), LMP subversion (2) */
	if (l < 9) {
		GAP(e, "0x1001 return too short (%u bytes)", (unsigned)l);
		return -1;
	}
	f->rev = (uint16_t)(rp[2] | rp[3] << 8);
	TOOL(e, "hci_rev=0x%04x", f->rev);
	if (f->rev != BTT_HCI_REV_STOCK) {
		GAP(e, "HCI revision 0x%04x is not the stock-patched 0x%04x: the kernel firmware download did not happen (not replying 0)", f->rev,
		    BTT_HCI_REV_STOCK);
		return -1;
	}
	if ((uint16_t)(rp[5] | rp[6] << 8) != 0x000f)
		GAP(e, "0x1001 manufacturer is not 0x000f (replying %d as specified)", BTT_MANUFACTURER);
	fw_string(f);
	return 0;
}

static int
script_boot(const struct btt_env *e, struct fwinfo *f)
{
	const char *a = e->dict->arg_str(e->dctx, "btaddress");
	uint8_t disp[6], par[6], rp[16], any = 0;
	size_t l = 0;
	unsigned o;
	int i, rc = -1;

	if (a) {
		if (btt_parse_btaddress(a, disp) != 0) {
			GAP(e, "boot: malformed btaddress argument (refused, not replying 0)");
			return -1;
		}
		for (i = 0; i < 6; i++)
			any |= disp[i];
		if (!any) {
			GAP(e, "boot: all-zero btaddress (refused, not replying 0)");
			return -1;
		}
	}
	if (open_ctl(e) != 0)
		return -1;
	if (read_version(e, f) != 0)
		goto done;
	if (a) {
		for (i = 0; i < 6; i++)
			par[i] = disp[5 - i];	/* display order MSB first -> HCI LSB first (byte order of bluetoothd's bytes is [unknown], spec s.6.1) */
		if (btt_hci_cmd(e, BTT_OP_WRITE_BD_ADDR, par, 6, rp, sizeof(rp), &l, &o) != 0)
			goto done;
		if (btt_hci_cmd(e, BTT_OP_READ_BD_ADDR, NULL, 0, rp, sizeof(rp), &l, &o) != 0)
			goto done;
		if (l < 7 || memcmp(rp + 1, par, 6) != 0) {
			GAP(e, "boot: 0x1009 readback does not match the address written (not replying 0)");
			goto done;
		}
		TOOL(e, "btaddress written and read back");
	} else {
		GAP(e, "boot: no btaddress argument; the controller keeps its address");
	}
	GAP(e, "boot: sleep mode 0xfc27 not sent (host-wake IRQ not wired)");
	GAP(e, "boot: operational baud raise not done (kernel keeps its baud)");
	GAP(e, "boot: local name 0x0c13 not sent (bluetoothd sets its own)");
	GAP(e, "boot: calibration 0xfd18/0xfd1e and regulatory 0xfd98 not sent (no BCM4389 equivalent known)");
	rc = 0;
done:
	e->tx->close(e->tctx);	/* always, before the reply: bluetoothd opens its own control after result 0 */
	return rc;
}

static int
script_deepsleep(const struct btt_env *e)
{
	uint8_t rp[16];
	size_t l = 0;
	unsigned o;
	int rc = -1;

	if (open_ctl(e) != 0)
		return -1;
	if (btt_hci_cmd(e, BTT_OP_RESET, NULL, 0, rp, sizeof(rp), &l, &o) == 0) {
		GAP(e, "deepsleep: sleep mode 0xfc27 and wake lines not available (HCI Reset only)");
		rc = 0;
	}
	e->tx->close(e->tctx);
	return rc;
}

int64_t
btt_handle(const struct btt_env *e)
{
	const char *id = e->dict->req_str(e->dctx, "kMsgId");
	int64_t result = 1;
	struct fwinfo f;
	int mfr = 0, hasfw = 0;

	memset(&f, 0, sizeof(f));
	if (!id) {
		GAP(e, "request without kMsgId string");
		e->dict->rep_i64(e->dctx, "result", 1);
		return 1;
	}
	if (strcmp(id, "RunBuiltinScript") == 0) {
		const char *s = e->dict->req_has_dict(e->dctx, "kMsgArgs") ? e->dict->arg_str(e->dctx, "script") : NULL;

		TOOL(e, "request RunBuiltinScript script=%s", s ? s : "(none)");
		if (!s) {
			GAP(e, "RunBuiltinScript without kMsgArgs.script");
		} else if (strcmp(s, "boot") == 0) {
			if (script_boot(e, &f) == 0) {
				result = 0;
				hasfw = 1;
			}
		} else if (strcmp(s, "init") == 0) {
			/* Apple's init is `quit` [bin]: no controller access */
			snprintf(f.fw, sizeof(f.fw), "BCM4389 BCM.hcd");
			result = 0;
			hasfw = 1;
		} else if (strcmp(s, "deepsleep") == 0) {
			if (script_deepsleep(e) == 0) {
				result = 0;
				hasfw = 1;
				snprintf(f.fw, sizeof(f.fw), "BCM4389 BCM.hcd");
			}
		} else {
			GAP(e, "RunBuiltinScript: unknown script '%.20s'", s);
		}
		mfr = (result == 0);
		e->dict->rep_i64(e->dctx, "result", result);
		e->dict->rep_i64(e->dctx, "manufacturer", mfr ? BTT_MANUFACTURER : 0);
		e->dict->rep_str(e->dctx, "firmware", hasfw ? f.fw : "");
	} else if (strcmp(id, "RunCommand") == 0) {
		TOOL(e, "request RunCommand");
		GAP(e, "RunCommand (hostless mode) not supported");
		e->dict->rep_i64(e->dctx, "result", 1);
		e->dict->rep_str(e->dctx, "output", "");
	} else {
		TOOL(e, "request kMsgId unsupported");
		GAP(e, "kMsgId '%.24s' not supported", id);
		e->dict->rep_i64(e->dctx, "result", 1);
	}
	TOOL(e, "result %lld", (long long)result);
	return result;
}
