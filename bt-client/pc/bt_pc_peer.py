#!/usr/bin/env python3
"""PixOS pairing peer on a Linux PC (BlueZ): a NoInputNoOutput agent that accepts pairing ONLY from one address,
makes the adapter discoverable + pairable for WINDOW seconds, then turns discoverable off, unregisters, and reports
whether that device is Paired/Bonded. Usage: bt_pc_peer.py ADDR WINDOW_S"""
import sys as _s
if len(_s.argv) > 1 and _s.argv[1] in ("-h", "--help"):
    print(__doc__)
    _s.exit(0)

import sys, time
import dbus, dbus.service, dbus.mainloop.glib
from gi.repository import GLib

ADDR = sys.argv[1].upper()
WINDOW = int(sys.argv[2])
AGENT_PATH = "/org/pixos/peeragent"
dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SystemBus()


def say(*a):
    print("BT PEER:", *a, flush=True)


def dev_addr(path):
    try:
        return str(dbus.Interface(bus.get_object("org.bluez", path), "org.freedesktop.DBus.Properties").Get("org.bluez.Device1", "Address")).upper()
    except Exception as e:
        return "?(%s)" % e


class Rejected(dbus.DBusException):
    _dbus_error_name = "org.bluez.Error.Rejected"


class Agent(dbus.service.Object):
    def ok(self, dev, what):
        a = dev_addr(dev)
        if a != ADDR:
            say("REFUSED %s from another device" % what)
            raise Rejected("not the expected device")
        say("accepted %s" % what)

    @dbus.service.method("org.bluez.Agent1", in_signature="", out_signature="")
    def Release(self):
        say("agent released")

    @dbus.service.method("org.bluez.Agent1", in_signature="os", out_signature="")
    def AuthorizeService(self, dev, uuid):
        self.ok(dev, "service %s" % uuid)

    @dbus.service.method("org.bluez.Agent1", in_signature="ou", out_signature="")
    def RequestConfirmation(self, dev, passkey):
        self.ok(dev, "confirmation")

    @dbus.service.method("org.bluez.Agent1", in_signature="o", out_signature="")
    def RequestAuthorization(self, dev):
        self.ok(dev, "authorization")

    @dbus.service.method("org.bluez.Agent1", in_signature="o", out_signature="s")
    def RequestPinCode(self, dev):
        say("REFUSED pin request (legacy pairing not expected)")
        raise Rejected("no pin")

    @dbus.service.method("org.bluez.Agent1", in_signature="o", out_signature="u")
    def RequestPasskey(self, dev):
        say("REFUSED passkey request")
        raise Rejected("no passkey")

    @dbus.service.method("org.bluez.Agent1", in_signature="", out_signature="")
    def Cancel(self):
        say("cancelled by bluez")


Agent(bus, AGENT_PATH)
mgr = dbus.Interface(bus.get_object("org.bluez", "/org/bluez"), "org.bluez.AgentManager1")
mgr.RegisterAgent(AGENT_PATH, "NoInputNoOutput")
mgr.RequestDefaultAgent(AGENT_PATH)
ad = dbus.Interface(bus.get_object("org.bluez", "/org/bluez/hci0"), "org.freedesktop.DBus.Properties")
ad.Set("org.bluez.Adapter1", "Powered", dbus.Boolean(True))
ad.Set("org.bluez.Adapter1", "Pairable", dbus.Boolean(True))
ad.Set("org.bluez.Adapter1", "DiscoverableTimeout", dbus.UInt32(WINDOW))
ad.Set("org.bluez.Adapter1", "Discoverable", dbus.Boolean(True))
say("discoverable + pairable for %d s, accepting only the expected device" % WINDOW)


def finish():
    try:
        ad.Set("org.bluez.Adapter1", "Discoverable", dbus.Boolean(False))
    finally:
        try:
            mgr.UnregisterAgent(AGENT_PATH)
        except Exception:
            pass
    om = dbus.Interface(bus.get_object("org.bluez", "/"), "org.freedesktop.DBus.ObjectManager")
    found = False
    for path, ifs in om.GetManagedObjects().items():
        d = ifs.get("org.bluez.Device1")
        if d and str(d.get("Address", "")).upper() == ADDR:
            found = True
            say("RESULT device paired=%d bonded=%d connected=%d" % (bool(d.get("Paired")), bool(d.get("Bonded", False)), bool(d.get("Connected"))))
    if not found:
        say("RESULT device not known to bluez (never connected)")
    say("discoverable off, agent unregistered")
    loop.quit()
    return False


loop = GLib.MainLoop()
GLib.timeout_add_seconds(WINDOW, finish)
loop.run()
