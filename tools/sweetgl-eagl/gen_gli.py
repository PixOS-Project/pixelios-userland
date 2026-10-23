#!/usr/bin/env python3
"""gli_slots.txt + Khronos GL headers (+ Mesa symbol list) -> gli_table_gen.c (and, for the host test, gli_fake_gl.c).

usage: gen_gli.py --slots gli_slots.txt --headers MESA/include [--symbols SYMS] --out gli_table_gen.c [--fake-out gli_fake_gl.c]

SYMS: one symbol per line, with or without the Mach-O leading underscore (`nm -g` of the Mesa libs, or tools/machosyms.py).
Without --symbols every name that has a prototype is wired (host test).
Prototypes are only read to learn the argument classes; the generated C declares its own `_mesa_X` externs with plain C types
(no GL headers, no Mesa headers), so it builds anywhere. The wrapper for slot i is `R w_i(void *rctx, args) { return _mesa_X(args); }`."""
import argparse, re, sys

ap = argparse.ArgumentParser()
ap.add_argument('--slots', required=True); ap.add_argument('--headers', required=True)
ap.add_argument('--symbols'); ap.add_argument('--out', required=True); ap.add_argument('--fake-out')
a = ap.parse_args()
NSLOTS = 1036   # GLEngine's _gliInitDispatchTable size [src refs/ios-sailfish gen-dispatch.py:62, R33 s.2.1 {rctx, disp[1036]}]

# ---- prototypes (first header wins: ES headers first, then desktop for names ES lacks)
protos = {}
PROTO = re.compile(r'\b(?:GL_APICALL|GL_API|GLAPI)\s+(.+?)\s*(?:GL_APIENTRY|GLAPIENTRY|APIENTRY)\s+(gl\w+)\s*\(([^;]*?)\)\s*;', re.S)
for h in ('GLES3/gl3.h', 'GLES2/gl2.h', 'GLES2/gl2ext.h', 'GLES/gl.h', 'GLES/glext.h', 'GL/gl.h', 'GL/glext.h'):
    try:
        text = open(a.headers + '/' + h).read()
    except OSError:
        continue
    for m in PROTO.finditer(text):
        ret, name, args = m.group(1).strip(), m.group(2), ' '.join(m.group(3).split())
        params = [] if args in ('', 'void') else [x.strip() for x in args.split(',')]
        protos.setdefault(name, (ret, params))

# ---- type classes -> C types of the wrapper (kind, c type)
SCAL = {'GLenum': 'u32', 'GLuint': 'u32', 'GLbitfield': 'u32', 'GLint': 'i32', 'GLsizei': 'i32', 'GLboolean': 'u8', 'GLubyte': 'u8',
        'GLbyte': 'i8', 'GLshort': 'i16', 'GLushort': 'u16', 'GLchar': 'i8', 'GLcharARB': 'i8', 'GLfixed': 'i32', 'GLclampx': 'i32',
        'GLfloat': 'f32', 'GLclampf': 'f32', 'GLdouble': 'f64', 'GLclampd': 'f64', 'GLhalf': 'u16',
        'GLint64': 'i64', 'GLuint64': 'u64', 'GLint64EXT': 'i64', 'GLuint64EXT': 'u64', 'GLintptr': 'i64', 'GLsizeiptr': 'i64',
        'GLintptrARB': 'i64', 'GLsizeiptrARB': 'i64', 'GLsync': 'ptr', 'GLeglImageOES': 'ptr', 'GLhandleARB': 'u32',
        'GLvoid': 'void', 'void': 'void', 'GLeglClientBufferEXT': 'ptr', 'GLDEBUGPROC': 'ptr', 'GLDEBUGPROCKHR': 'ptr'}
CT = {'u32': 'unsigned int', 'i32': 'int', 'u8': 'unsigned char', 'i8': 'signed char', 'i16': 'short', 'u16': 'unsigned short',
      'f32': 'float', 'f64': 'double', 'i64': 'long', 'u64': 'unsigned long', 'ptr': 'void *', 'void': 'void'}

def cls(t):
    t = re.sub(r'\b(const|volatile|struct)\b', '', t).strip()
    if '*' in t:
        return 'ptr'
    t = t.split()[-1] if t.split() else 'void'
    if t not in SCAL:
        raise KeyError(t)
    return SCAL[t]

def parse(proto):
    ret, params = proto
    ps = []
    for p in params:
        m = re.match(r'(.*?)(\w+)\s*(\[\d*\])?$', p)
        t = m.group(1).strip() if m and m.group(1).strip() else p     # unnamed param: `GLenum`
        if m and m.group(3): t += ' *'
        ps.append(cls(t if t else p))
    return cls(ret), ps

syms = None
if a.symbols:
    raw = [l.strip() for l in open(a.symbols) if l.strip()]
    syms = set(raw) | {r[1:] for r in raw if r.startswith('_')}     # Mach-O prepends one underscore

slots = {}
for ln in open(a.slots):
    ln = ln.split('#')[0].split()
    if not ln: continue
    i, names = int(ln[0]), [t for t in ln[1:] if t[0] not in '-=!']
    slots[i] = dict(names=names, to=next((t[2:] for t in ln[1:] if t.startswith('->')), None),
                    special=next((t[1:] for t in ln[1:] if t[0] == '='), None), trap='!trap' in ln)
    assert 0 <= i < NSLOTS and names, ln

wrap, entries, names_tab, fakes, specials, report = [], [None] * NSLOTS, ['?'] * NSLOTS, {}, [], {'wired': 0, 'special': 0, 'trap': []}
target = [None] * NSLOTS
nint = [0] * NSLOTS

for i in range(NSLOTS):
    s = slots.get(i)
    if not s:
        continue
    names_tab[i] = s['names'][0]
    if s['trap']:
        report['trap'].append((i, s['names'][0], 'forced')); continue
    if s['special']:
        specials.append(s['special']); entries[i] = s['special']; target[i] = s['special']; report['special'] += 1; continue
    found = None
    # variants of each name: as is, vendor suffix stripped, other suffixes added (Mesa spells VertexAttrib1fARB, GetQueryObjectiv, ...)
    def variants(n):
        core = re.sub(r'(ARB|EXT|OES|APPLE|NV|ATI)$', '', n)
        return [n, core] + [core + x for x in ('ARB', 'EXT', 'OES', 'NV')]
    cands = ([s['to'] and 'gl' + s['to']] if s['to'] else []) + [v for n in s['names'] for v in variants(n)]
    for n in cands:
        mesa = '_mesa_' + n[2:]
        pn = next((x for x in [n] + s['names'] + cands if x in protos), None)
        if pn is None or (syms is not None and mesa not in syms):
            continue
        found = (mesa, protos[pn]); break
    if not found:
        report['trap'].append((i, s['names'][0], 'no prototype' if not any(x in protos for x in s['names']) else 'no _mesa_ symbol'))
        continue
    mesa, proto = found
    try:
        r, ps = parse(proto)
    except KeyError as e:
        report['trap'].append((i, s['names'][0], 'unknown type %s' % e)); continue
    # a `->X` override has the arguments of its own prototype (glFlush -> Finish: both void)
    decl = ', '.join(['void *rctx'] + ['%s a%d' % (CT[c], k) for k, c in enumerate(ps)])
    call = ', '.join('a%d' % k for k in range(len(ps)))
    wrap.append('extern %s %s(%s);' % (CT[r], mesa, ', '.join(CT[c] for c in ps) or 'void'))
    body = '%s(%s);' % (mesa, call)
    wrap.append('static %s w_%d(%s) { %s }' % (CT[r], i, decl, body if r == 'void' else 'return ' + body))
    entries[i] = 'w_%d' % i; target[i] = mesa; report['wired'] += 1
    nint[i] = sum(c not in ('f32', 'f64') for c in ps)
    fakes[mesa] = (r, ps)

with open(a.out, 'w') as f:
    f.write('/* GENERATED by gen_gli.py from gli_slots.txt: do not edit. wired %d, special %d, trap %d of %d slots. */\n'
            % (report['wired'], report['special'], len(report['trap']) + sum(1 for i in range(NSLOTS) if i not in slots), NSLOTS))
    f.write('#include <stddef.h>\n#include "gli_table.h"\n\n')
    for sp in sorted(set(specials)):
        f.write('extern void %s(void);\n' % sp)
    f.write('\n' + '\n'.join(wrap) + '\n\n')
    for i in range(NSLOTS):
        if entries[i] is None:
            f.write('static long t_%d(void) { return sweet_gli_trap(%d); }\n' % (i, i))
    f.write('\nconst void *const sweet_gli_defaults[SWEET_GLI_NSLOTS] = {\n')
    for i in range(NSLOTS):
        f.write('\t(void *)%s,\n' % (entries[i] or 't_%d' % i))
    f.write('};\n\nconst char *const sweet_gli_names[SWEET_GLI_NSLOTS] = {\n')
    for i in range(NSLOTS):
        f.write('\t"%s",\n' % names_tab[i])
    f.write('};\n\n/* what each slot is wired to: "_mesa_X", "sgli_x" (hand-written) or NULL (trap) */\n')
    f.write('const char *const sweet_gli_targets[SWEET_GLI_NSLOTS] = {\n')
    for i in range(NSLOTS):
        f.write('\t%s,\n' % ('"%s"' % target[i] if target[i] else 'NULL'))
    f.write('};\n\n/* integer-class (non-float) argument count of the wired GL function, for the host test */\n')
    f.write('const unsigned char sweet_gli_nint[SWEET_GLI_NSLOTS] = {%s};\n' % ','.join(map(str, nint)))

if a.fake_out:
    with open(a.fake_out, 'w') as f:
        f.write('/* GENERATED by gen_gli.py: fake _mesa_* functions for the host test (record the call and the integer args). */\n')
        f.write('#include "gli_fake.h"\n')
        for mesa, (r, ps) in sorted(fakes.items()):
            decl = ', '.join('%s a%d' % (CT[c], k) for k, c in enumerate(ps)) or 'void'
            rec = ''.join('fake_arg(%d, (unsigned long)a%d); ' % (j, k) for j, k in
                          enumerate(k for k, c in enumerate(ps) if c not in ('f32', 'f64')) if j < 5)
            f.write('%s %s(%s) { fake_hit("%s"); %s%s}\n' % (CT[r], mesa, decl, mesa, rec,
                    '' if r == 'void' else 'return (%s)fake_ret; ' % CT[r]))

print('wired %d, special %d, trap %d (unnamed slots %d)' % (report['wired'], report['special'], len(report['trap']),
      sum(1 for i in range(NSLOTS) if i not in slots)), file=sys.stderr)
for i, n, why in report['trap']:
    print('  trap slot %d %s: %s' % (i, n, why), file=sys.stderr)
