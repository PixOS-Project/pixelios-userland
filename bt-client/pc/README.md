# PC peer tools (Linux, BlueZ + PipeWire/Pulse), for the PixOS Bluetooth tests and demo

Run on the PC that acts as the phone's peer. Neither needs root. The phone address is an argument, never stored here.

- `bt_pc_peer.py ADDR WINDOW_S`: pairing peer. Registers a NoInputNoOutput BlueZ agent that accepts pairing ONLY from ADDR, makes the adapter discoverable + pairable for WINDOW_S seconds, then turns discoverable off and reports Paired/Bonded for ADDR. Needs python3-dbus + python3-gi. Before a fresh pairing, drop a stale bond: `bluetoothctl remove ADDR` (a reflashed phone has lost its keys).
- `bt_pc_record.py ADDR_WITH_UNDERSCORES WAIT_S SECS`: A2DP oracle. Waits for the phone's bluez source, records up to SECS s, scores each 1 s window; PASS = >= 5 consecutive s with RMS > -40 dBFS at 400-480 Hz (the bt-client 440 Hz tone). Needs pactl/parecord.

Used by the BT stream holds (docs/sprint/2026-11-11-bt.md in the super repo): bt-hotpair (PAIRING PASS), bt-va2, bt-tr1.
