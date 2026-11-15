#!/usr/bin/env python3
"""T-f9ab: check that every multi-argument objc_msgSend call in the bt-probe binary passes its arguments in registers.

usage: callsite-check.py BINARY       (needs python3 + capstone; thin arm64 Mach-O)

Why: on Apple arm64 a call through a *variadic* prototype puts the arguments on the stack, while objc_msgSend's callee reads
x2..x7. a61d41c did that: initWithDelegate:queue:options: got a stale x2 (bt-up16 boot 2, SIGBUS far 0x103).

Method (linear scan of __text, no CFG): track for every register where its value came from -
  adr/adrp(+add)          -> a literal address (C strings are read from the file)
  ldr from a global slot  -> the function that dlsym() stored there (slot learned from `str x0,[slot]` after a call whose
                             x1 was the C string "objc_msgSend" / "sel_registerName")
  call through that slot  -> sel_registerName(literal) returns ('sel', name); objc_msgSend is a *site*.
At a site, x1 must carry a selector with a known name; its arity is the number of ':' in it; x2..x(1+arity) must each have
been written since the previous call (registers are caller-saved), and no store to [sp, #0..#0x40) may have happened since
the selector was registered (that is where a variadic caller spills its arguments). One output line per site, then
RESULT: PASS bt-probe-callsites n=N   or   RESULT: FAIL bt-probe-callsites spilled=N [unresolved=M]
"""
import struct
import sys

try:
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_MEM, ARM64_OP_REG
except ImportError:
    print("RESULT: FAIL bt-probe-callsites capstone missing (pip install capstone)")
    sys.exit(2)

TRACKED = ("objc_msgSend", "sel_registerName")


def load(path):
    d = open(path, "rb").read()
    if d[:4] != b"\xcf\xfa\xed\xfe" or struct.unpack_from("<I", d, 4)[0] != 0x0100000C:
        sys.exit("RESULT: FAIL bt-probe-callsites not a thin arm64 Mach-O")
    n = struct.unpack_from("<I", d, 16)[0]
    off, text, segs = 32, None, []
    for _ in range(n):
        cmd, size = struct.unpack_from("<II", d, off)
        if cmd == 0x19:  # LC_SEGMENT_64
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", d, off + 24)
            segs.append((vmaddr, vmsize, fileoff, filesize))
            for i in range(struct.unpack_from("<I", d, off + 64)[0]):
                so = off + 72 + 80 * i
                sect = d[so:so + 16].rstrip(b"\0")
                addr, sz, foff = struct.unpack_from("<QQI", d, so + 32)
                if sect == b"__text":
                    text = (addr, d[foff:foff + sz])
        off += size
    if text is None:
        sys.exit("RESULT: FAIL bt-probe-callsites no __text section")
    return d, text, segs


def cstring(d, segs, addr):
    for vmaddr, vmsize, fileoff, filesize in segs:
        if vmaddr <= addr < vmaddr + min(vmsize, filesize):
            s = fileoff + addr - vmaddr
            e = d.find(b"\0", s)
            if 0 < e - s < 200:
                try:
                    return d[s:e].decode("ascii")
                except UnicodeDecodeError:
                    return None
    return None


def scan(d, text, segs, slots, report):
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    tag = {}      # reg name -> ('lit', addr) | ('fn', name) | ('sel', name) | ('ret', str|None) | ('val',)
    fresh = set()  # registers written (not by a call) since the last call
    stack_stores = []  # sp-relative low stores since the last call
    sel_mark = {}  # reg name -> number of low-sp stores recorded when its selector appeared
    sites = []
    for ins in md.disasm(text[1], text[0]):
        m, ops = ins.mnemonic, ins.operands
        rn = ins.reg_name
        if m == "adr" or m == "adrp":
            tag[rn(ops[0].reg)] = ("lit", ops[1].imm)
            fresh.add(rn(ops[0].reg))
        elif m == "add" and len(ops) == 3 and ops[1].type == ARM64_OP_REG and ops[2].type == ARM64_OP_IMM \
                and tag.get(rn(ops[1].reg), ("",))[0] == "lit":
            tag[rn(ops[0].reg)] = ("lit", tag[rn(ops[1].reg)][1] + ops[2].imm)
            fresh.add(rn(ops[0].reg))
        elif m == "ldr" and ops[1].type == ARM64_OP_MEM and ops[1].mem.base and \
                tag.get(rn(ops[1].mem.base), ("",))[0] == "lit":
            slot = tag[rn(ops[1].mem.base)][1] + ops[1].mem.disp
            name = slots.get(slot)
            tag[rn(ops[0].reg)] = ("fn", name) if name else ("val",)
            fresh.add(rn(ops[0].reg))
        elif m == "str" and ops[1].type == ARM64_OP_MEM and ops[1].mem.base and \
                tag.get(rn(ops[1].mem.base), ("",))[0] == "lit":
            slot = tag[rn(ops[1].mem.base)][1] + ops[1].mem.disp
            t = tag.get(rn(ops[0].reg), ("",))
            if t[0] == "ret" and t[1] in TRACKED:
                slots[slot] = t[1]
        elif m in ("blr", "bl"):
            callee = ("fn", None)
            if m == "blr":
                callee = tag.get(rn(ops[0].reg), ("val",))
            name = callee[1] if callee[0] == "fn" else None
            if name == "objc_msgSend":
                sites.append(check_site(ins, tag, fresh, stack_stores, sel_mark))
                result = ("val",)
            elif name == "sel_registerName":
                t = tag.get("x0", ("",))
                result = ("sel", cstring(d, segs, t[1]) if t[0] == "lit" else None)
            else:
                t = tag.get("x1", ("",))
                result = ("ret", cstring(d, segs, t[1]) if t[0] == "lit" else None)
            for i in range(1, 19):
                tag.pop("x%d" % i, None)
            tag["x0"] = result
            fresh.clear()
            stack_stores = []
            sel_mark = {}
            if result[0] == "sel":
                sel_mark["x0"] = 0
        elif m == "ret":
            pass
        else:
            if m in ("mov",) and len(ops) == 2 and ops[1].type == ARM64_OP_REG:
                src = rn(ops[1].reg)
                tag[rn(ops[0].reg)] = tag.get(src, ("val",))
                fresh.add(rn(ops[0].reg))
                if src in sel_mark:
                    sel_mark[rn(ops[0].reg)] = sel_mark[src]
                elif rn(ops[0].reg) in sel_mark:
                    del sel_mark[rn(ops[0].reg)]
            elif m in ("str", "stp", "stur", "stnp", "strb", "strh") and ops[-1].type == ARM64_OP_MEM:
                mem = ops[-1].mem
                if mem.base and rn(mem.base) == "sp" and 0 <= mem.disp < 0x40:
                    stack_stores.append(ins.address)
            else:
                _, written = ins.regs_access()
                for r in written:
                    nm = rn(r)
                    if nm.startswith(("x", "w")) and nm[1:].isdigit():
                        nm = "x" + nm[1:]
                        tag[nm] = ("val",)
                        fresh.add(nm)
                        sel_mark.pop(nm, None)
    report.extend(sites)
    return sites


def check_site(ins, tag, fresh, stack_stores, sel_mark):
    t = tag.get("x1", ("",))
    if t[0] != "sel" or t[1] is None:
        return (ins.address, "?", -1, [], list(stack_stores), "unresolved selector")
    arity = t[1].count(":")
    missing = ["x%d" % (2 + i) for i in range(arity) if "x%d" % (2 + i) not in fresh]
    return (ins.address, t[1], arity, missing, list(stack_stores), "")


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: callsite-check.py BINARY")
    d, text, segs = load(sys.argv[1])
    slots = {}
    scan(d, text, segs, slots, [])  # pass 1 learns the dlsym slots (stored in main, used in other functions)
    sites = []
    scan(d, text, segs, slots, sites)
    spilled = unresolved = 0
    for addr, sel, arity, missing, stores, why in sites:
        if why:
            unresolved += 1
            print("0x%x %s: UNRESOLVED (%s)" % (addr, sel, why))
            continue
        bad = bool(missing) or (arity > 0 and bool(stores))
        spilled += 1 if bad else 0
        print("0x%x %s arity=%d %s%s%s" % (
            addr, sel, arity, "SPILLED" if bad else "ok",
            " unset=%s" % ",".join(missing) if missing else "",
            " sp-stores=%s" % ",".join("0x%x" % s for s in stores) if stores and arity > 0 else ""))
    multi = sum(1 for s in sites if s[2] > 0)
    if not sites:
        print("RESULT: FAIL bt-probe-callsites no objc_msgSend call site found (slot not recognised)")
        return 1
    if spilled or unresolved:
        print("RESULT: FAIL bt-probe-callsites spilled=%d%s of multi=%d sites=%d" % (
            spilled, " unresolved=%d" % unresolved if unresolved else "", multi, len(sites)))
        return 1
    print("RESULT: PASS bt-probe-callsites n=%d multi=%d" % (len(sites), multi))
    return 0


if __name__ == "__main__":
    sys.exit(main())
