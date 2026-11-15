/* pixos-bluetool: PixOS's open-source com.apple.BlueTool service for the BCM4389 (spec: docs/research/bt-pixos-bluetool-spec.md in the
 * private super repo; contract of the Apple service read from bluetoothd, bt-skywalk-uart-channel.md s.6 [bin]).
 * The core is plain C: dispatch, reply decisions, btaddress parse, H4 framing and the script runners. The XPC dictionary layer and the
 * UART transport are two ops tables, so the host test (bt_tool_host_test.c) runs every path with fakes and the iOS glue
 * (bt_tool_ios.c) supplies libxpc and the kctl socket. Rule 7: every unsupported path logs a "BT GAP:" row and never returns
 * plain success. Rule 4: opcodes and status only are logged, never addresses or payload bytes. */
#ifndef BT_TOOL_H
#define BT_TOOL_H

#include <stddef.h>
#include <stdint.h>

#define BTT_CMD_TIMEOUT_MS 2000u	/* spec: 2 s per command */
#define BTT_HCI_REV_STOCK 0x20cb	/* spec: the stock BP31 BCM.hcd answer (sweet_bt_uart.c SBU_HCD_STOCK_REV; bt-pr1 hci_rev=0x20cb) */
#define BTT_MANUFACTURER 15		/* Broadcom company id 0x000F (spec) */
#define BTT_FW_MAX 80

/* HCI opcodes the tool may ever send (the allowlist in bt_tool_core.c is the independent copy checked at send time). */
#define BTT_OP_RESET 0x0c03
#define BTT_OP_READ_LOCAL_VERSION 0x1001
#define BTT_OP_READ_BD_ADDR 0x1009
#define BTT_OP_WRITE_BD_ADDR 0xfc01	/* Broadcom Write_BD_ADDR, plen 6, LSB first */

/* The XPC dictionary layer: ctx is the request message (and its reply). Strings are owned by the layer, valid until the call returns. */
struct btt_dict_ops {
	const char *(*req_str)(void *ctx, const char *key);	/* top-level string value; NULL if absent or not a string */
	int (*req_has_dict)(void *ctx, const char *key);	/* 1 if the value is a dictionary */
	const char *(*arg_str)(void *ctx, const char *key);	/* string inside kMsgArgs; NULL if absent or not a string */
	void (*rep_i64)(void *ctx, const char *key, int64_t v);
	void (*rep_str)(void *ctx, const char *key, const char *v);
};

/* The UART control transport. read: >0 bytes, 0 = timeout, <0 = error. */
struct btt_tx_ops {
	int (*open)(void *ctx);		/* 0 ok; <0 error (EBUSY ...) */
	void (*close)(void *ctx);
	void (*log_termios)(void *ctx);	/* TIOCGETA, logged, never changed */
	int (*write)(void *ctx, const uint8_t *b, size_t n);	/* n on success */
	int (*read)(void *ctx, uint8_t *b, size_t max, unsigned timeout_ms);
	unsigned (*now_ms)(void *ctx);
	void (*sleep_ms)(void *ctx, unsigned ms);	/* 2026-11-11: open_ctl waits for the kernel bring-up (EBUSY) */
};

struct btt_env {
	const struct btt_dict_ops *dict;
	void *dctx;
	const struct btt_tx_ops *tx;
	void *tctx;
	void (*log)(void *lctx, const char *line);	/* one finished line, no newline */
	void *lctx;
};

/* ---- pieces (exported for the host test) ---- */
/* "%02X:%02X:%02X:%02X:%02X:%02X" (hex digits of either case) -> out[0..5] in display order; 0 ok, -1 malformed */
int btt_parse_btaddress(const char *s, uint8_t out[6]);
/* one H4 command packet into buf (>= 4 + plen); returns the length */
size_t btt_h4_cmd(uint8_t *buf, uint16_t opcode, const uint8_t *params, uint8_t plen);

/* Send one allowlisted command and wait for its Command Complete. rp gets the return parameters (status first); *rplen their length.
 * 0 ok (status byte 0), -1 refused (not allowlisted / bad length), -2 transport write error, -3 timeout, -4 framing/read error,
 * -5 controller status != 0 (rp[0] valid). *other counts unrelated events skipped. */
int btt_hci_cmd(const struct btt_env *e, uint16_t opcode, const uint8_t *params, uint8_t plen, uint8_t *rp, size_t rpmax, size_t *rplen,
    unsigned *other);

/* ---- the service ---- */
/* Handle one request message: runs the script (transport opened and closed inside) and fills the reply. Returns the `result` sent. */
int64_t btt_handle(const struct btt_env *e);

#endif
