/* sprint-bt (2026-11-09): BluetoothManager path of BTApp, the API stock Settings uses (docs/research/T-a52f-btmanager-pairing-path.md
 * in the private super repo). Two parts:
 *   - the attach step: sharedInstance -> BluetoothAvailabilityChangedNotification(YES) -> setPowered:YES -> classic scan
 *     (setDeviceScanningEnabled:YES) counting BluetoothDeviceDiscoveredNotification; RESULT line `BT MGR: RESULT: ...`;
 *   - the pair step's state machine (btm_pair_*), fed by the iOS glue (bt_mgr_ios.c) with the BluetoothManager notifications.
 * The state machine and the result accounting are plain C and host-tested (bt_mgr_host_test.c); the glue only translates
 * notifications into btm_ev values. Counts only: no device name or address is ever printed (rule 4). */
#ifndef BT_MGR_H
#define BT_MGR_H

#include "bt_client.h"

/* ---- attach step ---- */
struct btm_attach {
	int mg_bluetooth;	/* MGGetBoolAnswer("bluetooth"): 1/0, -1 = symbol missing */
	int shared;		/* 1 = +sharedInstance returned non-nil */
	long init_err;		/* +lastInitError when shared == 0 */
	int available;		/* 1 = BluetoothAvailabilityChangedNotification object YES seen (or -available YES) */
	unsigned avail_ms;	/* sharedInstance -> available */
	int power_req;		/* 1 = setPowered:YES sent */
	int powered;		/* 1 = -powered YES after the request */
	unsigned power_ms;
	int scan_req;		/* 1 = setDeviceScanningEnabled:YES sent */
	unsigned discovered;	/* BluetoothDeviceDiscoveredNotification count */
	unsigned unique;	/* distinct BluetoothDevice objects among them */
	unsigned gaps;
};
/* PASS needs shared, available, powered; the scan count is reported, the LE scan (CoreBluetooth) result is a separate line */
int btm_attach_pass(const struct btm_attach *a);
/* the RESULT line (no newline) into buf */
void btm_attach_result(const struct btm_attach *a, char *buf, unsigned long n);

/* ---- pair step ---- */
enum btm_ev {
	BTM_EV_START,		/* pair step begins: a = bonded devices now */
	BTM_EV_FOUND,		/* the peer was discovered (the glue matched it; nothing about it is passed) */
	BTM_EV_CONNECT_SENT,	/* connectDevice: issued */
	BTM_EV_CONFIRM,		/* UserConfirmation or NumericComparision notification for the peer */
	BTM_EV_ANSWERED,	/* acceptSSP:0 forDevice: sent */
	BTM_EV_PIN_OK,		/* BluetoothPairingPINResultSuccessNotification for the peer */
	BTM_EV_PIN_FAIL,	/* ...PINResultFailedNotification: a = error */
	BTM_EV_CONNECT_FAIL,	/* BluetoothDeviceConnectFailedNotification: a = error */
	BTM_EV_CONNECTED,	/* BluetoothDeviceConnectSuccessNotification for the peer */
	BTM_EV_PAIRED,		/* -[BluetoothDevice paired] is YES (after BluetoothPairedStatusChangedNotification) */
	BTM_EV_PEER_BONDED,	/* the peer is among -[BluetoothManager pairedDevices] at the start (bond survived); the glue then sends connectDevice: once */
	BTM_EV_END,		/* timeout or done: a = bonded devices now */
};
struct btm_pair_sm {
	struct btc_pair r;
	int done;		/* 1 = an outcome is final (no further event changes it) */
	unsigned t0_ms;
};
void btm_pair_init(struct btm_pair_sm *s, unsigned now_ms);
/* feed one event; returns 1 when the step is finished (the glue stops waiting) */
int btm_pair_feed(struct btm_pair_sm *s, enum btm_ev ev, long a, unsigned now_ms);

#endif
