/* bt-client: PixOS own-code CoreBluetooth central (T-86aa). The core (bt_client_core.c) owns the sequence and every
 * log line; it reaches the system only through struct btc_ops, so the host test runs it with stubs. Lines carry counts
 * only: never a peripheral address, identifier or a service UUID heard on the air (rule 4); "unique" is a Bloom-filter count.
 * The one UUID printed is arm C's own scan filter (a public 16-bit assigned number from the code, T-27d4). */
#ifndef BT_CLIENT_H
#define BT_CLIENT_H

struct btc_sample {
	long state;
	int scanning;
	unsigned discovered;
	unsigned unique;
	unsigned gaps;
};

struct btc_ops {
	void *ctx;
	int (*start)(void *ctx);
	/* T-18e5/T-2ae7: the init option dictionary the CBCentralManager was created with, as "<key>=<value>": <key> is the string
	 * value of the CoreBluetooth constant CBCentralManagerOptionShowPowerAlertKey read at run time, <value> the NSNumber passed,
	 * read back from the object. 0 = created with nil options (no dictionary, or a retry). Called only after start() == 0. */
	const char *(*central_options)(void *ctx);
	/* T-2ae7: re-issue the services:nil scan with the current arm's options (nil in arm A) on the delegate queue. Returns the scan count after issuing it (> 0),
	 * 0 when the queue re-check found the central scanning or not poweredOn (nothing issued), < 0 when there is no central. */
	int (*rescan)(void *ctx);
	/* T-2ae7 arm B: on the delegate queue, stop the running scan and scan again with the arm-B options dictionary
	 * {kCBMsgArgIsPrivilegedDaemon: kCFBooleanTrue}; every later (re)scan uses it too. Fills desc with "<key>=<value>" read back
	 * from the objects passed. Returns the scan count (> 0), 0 when not poweredOn (options kept, the next poweredOn scan
	 * carries them), < 0 when the dictionary could not be built (no scan change; the caller prints a gap). */
	int (*arm_b)(void *ctx, char *desc, unsigned long desc_len);
	/* T-27d4 arm C: on the delegate queue, stop the running scan and scan again with services:@[CBUUID FD50], options:nil
	 * (the documented service-UUID scan; arm B's options are dropped); every later (re)scan uses it until arm B. Fills desc with
	 * the CBUUID's UUIDString read back from the object passed. Same return contract as arm_b. */
	int (*arm_c)(void *ctx, char *desc, unsigned long desc_len);
	void (*sample)(void *ctx, struct btc_sample *s);
	void (*sleep_s)(void *ctx, unsigned s);
	void (*out)(void *ctx, const char *line);
};

#define BTC_STATE_POWERED_ON 5L

/* T-0936: scan again on every transition into poweredOn (bt-up17: bluetoothd restarted, the probe stayed poweredOn without a scan) */
int btc_scan_due(long prev_state, long state);
/* T-2ae7: scan again when a sample reads poweredOn with isScanning == NO (the scan ended while the state stayed poweredOn) */
int btc_rescan_due(long state, int scanning);

/* armb_at_s: switch to arm B after the sample at t == armb_at_s (0 = no arm B, the bt-up18 sequence).
 * armc_at_s (T-27d4): switch to arm C after the sample at t == armc_at_s (0 = no arm C). With both, arm C must come first
 * (A -> C -> B: arm B's discoveries would fill bluetoothd's per-session duplicate map before arm C); otherwise arm C is
 * refused with a gap line. */
int btc_run(const struct btc_ops *ops, unsigned period_s, unsigned total_s, unsigned armb_at_s, unsigned armc_at_s, const char *build);

/* T-2d63: the foreground-app client (BTApp, /Applications/BTApp.app). One services:nil, options:nil scan for the whole run; the
 * app delegate (main thread) records every UIKit state change with a snapshot of the counters, and btc_app_run splits the
 * counts into windows at those snapshots, so a foreground window and a background window of the SAME scan are compared. */
#define BTC_APP_LAUNCHING (-1)	/* before the first delegate callback */
#define BTC_APP_ACTIVE 0	/* UIApplicationStateActive (applicationDidBecomeActive:) */
#define BTC_APP_INACTIVE 1	/* UIApplicationStateInactive (applicationWillResignActive:, applicationWillEnterForeground:) */
#define BTC_APP_BACKGROUND 2	/* UIApplicationStateBackground (applicationDidEnterBackground:) */
#define BTC_APP_MIXED 9	/* window label after the window list is full: later changes merged, counts neither fg nor bg */
#define BTC_APP_MAX_WINDOWS 16

struct btc_trans {
	int state;		/* the state ENTERED at this transition (BTC_APP_*) */
	unsigned t_s;		/* now_s() at the callback */
	unsigned discovered;	/* counters at the callback (0 before the central exists) */
	unsigned unique;
};

/* T-3615: one LE connect attempt (BTApp connect step). outcome is what CoreBluetooth told the app, not an HCI status (the HCI
 * Create Connection / Command Status / Connection Complete bytes are read from the host log). No address or identifier here. */
#define BTC_CONN_NOCAND 0	/* no connectable candidate was left (nothing sent) */
#define BTC_CONN_OK 1		/* didConnectPeripheral */
#define BTC_CONN_FAIL 2		/* didFailToConnectPeripheral (err = NSError code) */
#define BTC_CONN_TIMEOUT 3	/* neither callback within the timeout; the app cancelled the pending connection */
#define BTC_CONN_NOCENTRAL 4	/* no central / queue / delegate method: the call was not made (a GAP line was printed) */

struct btc_conn {
	int outcome;		/* BTC_CONN_* */
	int has_err;		/* err is valid (didFailToConnect carried an NSError) */
	long err;		/* NSError code */
	int rssi;		/* the candidate's last RSSI (dBm), 127 = none */
	unsigned ms;		/* connect call -> callback, milliseconds (timeout: the timeout) */
	unsigned cands;		/* candidates still unused at this call */
	int services;		/* BTC_CONN_OK: discoverServices count, -1 = no peripheral:didDiscoverServices: answer */
	int disc;		/* 1 = didDisconnectPeripheral seen after the app cancelled */
};

/* T-a52f: one pairing attempt with the user's peer phone (BTApp pair step). The stages are what the backend saw from the
 * pairing API (not HCI); the HCI rows (IO Capability, User Confirmation Request, Simple Pairing Complete, Link Key Notification,
 * Encryption Change) come from the host log. No name or address is carried here (rule 4). */
#define BTC_PAIR_NOMGR 0	/* the pairing API could not be used (a GAP line was printed), nothing was sent */
#define BTC_PAIR_NOPEER 1	/* no device was discovered within the timeout */
#define BTC_PAIR_OK 2		/* the bond exists at the end (paired, or reconnected on an existing bond) */
#define BTC_PAIR_FAIL 3		/* the API reported a failure (err) */
#define BTC_PAIR_TIMEOUT 4	/* the device was found but neither paired nor failed within the timeout */
/* furthest stage reached */
#define BTC_PAIR_ST_NONE 0
#define BTC_PAIR_ST_FOUND 1	/* peer discovered */
#define BTC_PAIR_ST_STARTED 2	/* connect/pair requested */
#define BTC_PAIR_ST_CONFIRM 3	/* the pairing agent asked the app to confirm (SSP) */
#define BTC_PAIR_ST_ANSWERED 4	/* the app answered the confirmation */
#define BTC_PAIR_ST_PAIRED 5	/* paired status changed to paired */
#define BTC_PAIR_ST_BONDED 6	/* the peer was already among the paired devices at the start (no discovery notification exists for it) */

struct btc_pair {
	int outcome;		/* BTC_PAIR_* */
	int stage;		/* BTC_PAIR_ST_* */
	int has_err;
	long err;
	unsigned ms;		/* request -> result */
	int bonded_start;	/* bonded devices before the attempt (a bond that survived a reboot makes this >= 1) */
	int bonded_end;		/* bonded devices after */
	int prompt;		/* 1 = a confirmation request reached the app (a reconnect on a valid bond must have 0) */
	int connected;		/* 1 = the device was connected at the end */
	int peer_bonded;	/* 1 = the peer was found among pairedDevices at the start (already bonded) */
};

struct btc_app_ops {
	const struct btc_ops *bt;	/* start, central_options, rescan, sample, sleep_s, out (arm_b/arm_c unused) */
	/* transition i (0-based) in callback order: 1 = filled, 0 = not (yet) recorded */
	int (*transition)(void *ctx, unsigned i, struct btc_trans *t);
	unsigned (*now_s)(void *ctx);	/* monotonic seconds (wall time keeps running while the app is suspended) */
	unsigned (*side_gaps)(void *ctx);	/* GAP lines the app side printed itself (review T-2d63 LOW 6); NULL = none */
	/* T-3615 connect step (NULL = none, the T-2d63 sequence): once the app has been in the ACTIVE window at t >= conn_at_s, stop
	 * the scan and make up to conn_tries attempts, each on the next-strongest connectable advertiser, conn_timeout_s each; the
	 * scan is re-issued afterwards by the normal not-scanning resume. Blocking, called on the run thread. */
	int (*connect)(void *ctx, unsigned try_idx, unsigned timeout_s, struct btc_conn *r);
	unsigned conn_at_s, conn_tries, conn_timeout_s;
	/* T-a52f pair step (NULL = none): once ACTIVE at t >= pair_at_s, one blocking attempt of at most pair_timeout_s on the peer
	 * phone; the scan is stopped by the backend and re-issued by the not-scanning resume. Called on the run thread. */
	int (*pair)(void *ctx, unsigned timeout_s, struct btc_pair *r);
	unsigned pair_at_s, pair_timeout_s;
};

/* window label of an app state: active fg, background bg, inactive in, launching pre, mixed mix */
const char *btc_app_label(int state);
/* runs until now_s() - start >= total_s; RESULT pass iff the active windows together discovered >= 1 */
int btc_app_run(const struct btc_app_ops *o, unsigned period_s, unsigned total_s, const char *build);

/* iOS backend (bt_client_ios.c), shared by bt-probe and BTApp: btc_ios_setup resolves the runtime and fills *ops (0, or -1 when a
 * libobjc symbol is missing); btc_ios_counters copies discovered/unique on the delegate queue (0/0 before the central exists). */
int btc_ios_setup(struct btc_ops *ops);
void btc_ios_counters(unsigned *discovered, unsigned *unique);
/* T-3615: one connect attempt on the strongest unused connectable candidate seen by the scan (bt_client_ios.c); fills *r. */
int btc_ios_connect(unsigned try_idx, unsigned timeout_s, struct btc_conn *r);
/* T-a52f: one pairing attempt (bt_client_ios.c, BluetoothManager backend); fills *r. */
int btc_ios_pair(unsigned timeout_s, struct btc_pair *r);

/* bt-client adv (bt-proper): LE advertising through CBPeripheralManager, so one boot exercises bluetoothd's multi-advertising path
 * (Apple VSCs 0xfd4a..0xfd4d, which the kernel translates to Broadcom 0xfd54). The core owns the state machine and every
 * "BT ADV:" line; the backend (bt_adv_ios.c) reaches the system only through struct btadv_ops, so the host test runs it with stubs.
 * Lines carry counts and our own constants only: never the address, identifier or name of a nearby device (rule 4). */
#define BTADV_POWER_WAIT_S 30	/* wait for CBManagerStatePoweredOn */
#define BTADV_STEP_WAIT_S 10	/* wait for didAddService / didStartAdvertising */
#define BTADV_DEFAULT_S 60

struct btadv_sample {
	long state;		/* CBManagerState, -1 = no manager */
	int svc;		/* 0 = didAddService pending, 1 = added, -1 = error */
	int adv;		/* 0 = didStartAdvertising pending, 1 = started, -1 = error */
	int is_adv;		/* CBPeripheralManager.isAdvertising, -1 = no manager */
	long err;		/* NSError code behind svc == -1 / adv == -1 (adv wins), else 0 */
	char err_domain[48];	/* NSError domain (framework constant), "" when err == 0 */
	unsigned reads;		/* didReceiveReadRequest calls */
	unsigned centrals;	/* distinct central objects behind those reads (pointer compare, never printed) */
	unsigned gaps;		/* GAP lines the backend printed itself */
};

struct btadv_ops {
	void *ctx;
	int (*start)(void *ctx);	/* create the CBPeripheralManager + delegate queue; 0 ok, < 0 failed (backend printed a GAP) */
	int (*add_service)(void *ctx);	/* addService: with the readable characteristic; 0 issued, < 0 failed */
	int (*advertise)(void *ctx);	/* startAdvertising: (local name + service UUID); 0 issued, < 0 failed */
	void (*sample)(void *ctx, struct btadv_sample *s);
	void (*stop)(void *ctx);	/* stopAdvertising + removeAllServices; idempotent, called whenever start() succeeded */
	void (*sleep_s)(void *ctx, unsigned s);
	void (*out)(void *ctx, const char *line);
};

/* argv after "adv": [seconds] [--delay N]. 0 ok (*secs default BTADV_DEFAULT_S, *delay_s default 0), -1 bad (why filled). */
int btadv_parse(int argc, char **argv, unsigned *secs, unsigned *delay_s, char *why, unsigned long why_len);
/* 0 = PASS, 1 = FAIL; the last line is "BT ADV: RESULT: PASS|FAIL ..." */
int btadv_run(const struct btadv_ops *ops, unsigned secs, unsigned delay_s, const char *build);
/* iOS backend (bt_adv_ios.c) */
int btadv_ios_main(unsigned secs, unsigned delay_s);

/* bt-client tone (bt-proper): a 440 Hz sine through AVAudioSession + an AudioToolbox AudioQueue, so one boot drives a real A2DP stream
 * (host encode in mediaserverd, docs/research/bt-audio-path.md). The core owns the sine, the state machine and every "BT TONE:" line;
 * the backend (bt_tone_ios.c) reaches the system only through struct bttone_ops. Lines carry portType strings and counts only (rule 4). */
#define BTTONE_RATE 44100u
#define BTTONE_HZ 440u
#define BTTONE_AMP 8230	/* round(32767 * 10^(-12/20)) = -12 dBFS peak */
#define BTTONE_BUF_FRAMES 4096u	/* one AudioQueue buffer: 4096 frames, 16-bit stereo = 16384 bytes, ~92.9 ms */
#define BTTONE_NBUF 3u
#define BTTONE_DEFAULT_S 30
#define BTTONE_STALL_S 3	/* consecutive 1 s samples without a refill = the queue stopped */
#define BTROUTE_LEN 96

struct bttone_sample {
	long qerr;		/* first non-zero AudioQueue OSStatus seen (0 = none) */
	unsigned refills;	/* buffers handed to the queue from the callback (the priming fills are not counted) */
	unsigned gaps;		/* GAP lines the backend printed itself */
};

struct bttone_ops {
	void *ctx;
	int (*session)(void *ctx);	/* setCategory:Playback + setActive:YES; 0 ok, < 0 failed (backend printed a GAP) */
	void (*route)(void *ctx, char *buf, unsigned long len);	/* output portType strings joined by '+', "none" when empty, "?" if unreadable */
	int (*start)(void *ctx);	/* AudioQueueNewOutput + prime 3 buffers + AudioQueueStart; 0 ok, < 0 failed (GAP printed) */
	void (*sample)(void *ctx, struct bttone_sample *s);
	void (*stop)(void *ctx);	/* AudioQueueStop + Dispose + setActive:NO; idempotent, called whenever start() was attempted */
	void (*sleep_s)(void *ctx, unsigned s);
	void (*out)(void *ctx, const char *line);
};

/* argv after "tone": [seconds] [--delay N]. 0 ok (*secs default BTTONE_DEFAULT_S, *delay_s default 0), -1 bad (why filled). */
int bttone_parse(int argc, char **argv, unsigned *secs, unsigned *delay_s, char *why, unsigned long why_len);
/* Fill `frames` stereo 16-bit frames (L == R) of a BTTONE_HZ sine at BTTONE_AMP, continuing from *pos (frame counter, kept mod BTTONE_RATE). */
void bttone_fill(short *out, unsigned frames, unsigned *pos);
/* minimum refills a healthy queue of `secs` seconds has made (half of the nominal rate) */
unsigned bttone_min_refills(unsigned secs);
/* 0 = PASS, 1 = FAIL; the last line is "BT TONE: RESULT: PASS|FAIL ..." */
int bttone_run(const struct bttone_ops *ops, unsigned secs, unsigned delay_s, const char *build);
/* iOS backend (bt_tone_ios.c) */
int bttone_ios_main(unsigned secs, unsigned delay_s);

#endif
