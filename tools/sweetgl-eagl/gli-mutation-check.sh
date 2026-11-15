#!/bin/sh
# Each mutant is a sed edit of gen_gli.py / gli_table.c / gli_special.c; gli-host-test.sh (HEADERS=... SYMBOLS=...) must FAIL on it.
# Baseline first (AP-20). Needs HEADERS (and optionally SYMBOLS) like gli-host-test.sh.
cd "$(dirname "$0")"
[ -n "$HEADERS" ] || { echo "set HEADERS=<mesa>/include (generator mutants need it)"; exit 2; }
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
n=0; bad=0
./gli-host-test.sh >/dev/null 2>&1 || { echo 'baseline gli-host-test FAILS (HEADERS set? stale generated files?), mutation check meaningless'; exit 2; }
mut() { # name file sed-expr envvar
	n=$((n+1)); sed "$3" "$2" > "$D/m.c"
	if cmp -s "$2" "$D/m.c"; then echo "mutant $1: sed did not change anything"; bad=$((bad+1)); return; fi
	env "$4=$D/m.c" ./gli-host-test.sh >"$D/out" 2>&1; rc=$?
	if [ $rc = 0 ]; then echo "SURVIVED $1"; bad=$((bad+1))
	# last clause: the plain run completed (it compiled) and the sanitizer run died mid-test (the test captures stderr, so no report is visible)
	elif grep -q -E '^FAIL |checks, [1-9][0-9]* failed|runtime error|AddressSanitizer|^STALE' "$D/out" || [ $rc -ge 128 ] || { grep -q 'checks, 0 failed' "$D/out" && ! grep -q 'error:' "$D/out"; }; then echo "killed   $1"
	else echo "NO-KILL-EVIDENCE $1 (non-zero exit but no test failure line / sanitizer report / crash: a build error is not a kill, AP-214)"; bad=$((bad+1)); fi
}
mut shift-args       gen_gli.py     "s/\['void \*rctx'\] + \['%s a%d'/['void *rctx', 'long pad'] + ['%s a%d'/" GEN_PY
mut ignore-override  gen_gli.py     "s/for n in (\[s\['to'\] and 'gl' + s\['to'\]\] if s\['to'\] else \[\]) + s\['names'\]:/for n in s['names']:/;s/^    cands = (\[s\['to'\].*\$/    cands = [v for n in s['names'] for v in variants(n)]/" GEN_PY
mut no-ensure-wired  gen_gli.py     "s/{ SWEET_GLI_ENSURE(rctx, %s); %s }/{ %.0s%s }/" GEN_PY
mut ignore-fail-wired gen_gli.py    "s/{ SWEET_GLI_ENSURE(rctx, %s); %s }/{ sweet_gli_ensure_current(rctx); %.0s%s }/" GEN_PY
mut no-ensure-special gli_special.c "s/unsigned object, unsigned name) { SWEET_GLI_ENSURE(rctx, return 0);/unsigned object, unsigned name) {/" SPECIAL_C
mut ignore-fail-special gli_special.c "s/SWEET_GLI_ENSURE(rctx, return);/sweet_gli_ensure_current(rctx);/" SPECIAL_C
mut trap-every-time  gli_table.c    's/if (!seen\[slot\]) {/if (1) {/' TABLE_C
mut init-short       gli_table.c    's/sizeof(t->disp));/sizeof(t->disp) - sizeof(void *));/' TABLE_C
mut getstring-enum   gli_special.c  's/name == 0x8a1e/name == 0x8a1f/' SPECIAL_C
mut testobject-false gli_special.c  's/return 1; }/return 0; }/' SPECIAL_C
mut delete-branch    gli_special.c  's/if (_mesa_IsProgram(obj))/if (!_mesa_IsProgram(obj))/' SPECIAL_C
mut clamp-off        gli_table.c    's/param > 4) param = 4;/param > 99) param = 4;/' TABLE_C
mut clamp-wrong-pname gli_table.c   's/if (pname == GL_UNPACK_ALIGNMENT \&\& param > 4)/if (pname == GL_UNPACK_ROW_LENGTH \&\& param > 4)/' TABLE_C
mut hook-not-installed gli_table.c  's/t->disp\[GLI_SLOT_PIXELSTOREI\] = (const void \*)hook_pixelstorei;/t->disp[GLI_SLOT_PIXELSTOREI] = orig_ps; (void)hook_pixelstorei;/' TABLE_C
mut shader-no-orig   gli_table.c    's/((void (\*)(void \*, unsigned))orig_lp)(rctx, program);/;/' TABLE_C
mut shader-cap-off   gli_table.c    's/#define GLI_SH_RES_MAX 256/#define GLI_SH_RES_MAX 100000/' TABLE_C
mut shader-always-on gli_table.c    's/if (shaders) {/if (1) {/' TABLE_C
mut shader-logs-blob gli_table.c    's/ (blob not logged)\\n", program, format, length, st);/ %.8s\\n", program, format, length, st, (const char *)binary);/' TABLE_C
mut shader-src-cap-off gli_table.c  's/#define GLI_SH_SRC_MAX 64 /#define GLI_SH_SRC_MAX 100000 /' TABLE_C
mut shader-compile-arg gli_table.c  's/((void (\*)(void \*, unsigned))orig_cs)(rctx, shader);/((void (*)(void *, unsigned))orig_cs)(rctx, shader + 1);/' TABLE_C
mut shader-link-arg  gli_table.c    's/((void (\*)(void \*, unsigned))orig_lp)(rctx, program);/((void (*)(void *, unsigned))orig_lp)(rctx, program + 1);/' TABLE_C
mut shader-bin-cap-off gli_table.c  's/#define GLI_SH_BIN_MAX 64 /#define GLI_SH_BIN_MAX 100000 /' TABLE_C
mut shader-il-cap-off gli_table.c   's/#define GLI_SH_IL_MAX 64 /#define GLI_SH_IL_MAX 100000 /' TABLE_C
mut shader-srcbytes-off gli_table.c 's/#define GLI_SH_SRC_BYTES 1024 /#define GLI_SH_SRC_BYTES 2048 /' TABLE_C
mut shader-logbytes-off gli_table.c 's/#define GLI_SH_LOG_BYTES 4096 /#define GLI_SH_LOG_BYTES 8192 /' TABLE_C
mut capvp-first-last-swapped gli_table.c 's/memcpy(cd->lvp, d->vp, sizeof cd->lvp); cd->lmode = d->mode; cd->lcount = d->count;/memcpy(cd->lvp, cd->fvp, sizeof cd->lvp); cd->lmode = cd->fmode; cd->lcount = cd->fcount;/' TABLE_C
mut capvp-fmode-not-recorded gli_table.c 's/cd->fmode = d->mode; //' TABLE_C
# capv8 (batch 8): formatter and hook. Equivalent mutant NOT listed: GLI_CAPV_PER_BIND 4 -> 5 (the eligible-draw window capv_e <= GLI_CAPV_WINDOW (4) already limits a bind to 4 logged draws, AP-26).
mut capv8-first-zero-is-dash gli_table.c 's/if (v->first >= 0) snprintf(first/if (v->first > 0) snprintf(first/' TABLE_C
mut capv8-client-ptr-printed gli_table.c 's/if (v->at\[i\].buf > 0) snprintf(off/if (v->at[i].buf >= 0) snprintf(off/' TABLE_C
mut capv8-no-tail-reserve gli_table.c 's/body = top - 5;/body = top;/' TABLE_C
mut capv8-cut-mark gli_table.c 's/" ~cut"/" ~cu"/' TABLE_C
mut capv8-umore-always gli_table.c 's/if (v->umore > 0) snprintf(tk/if (v->umore >= 0) snprintf(tk/' TABLE_C
mut capv8-oob-ge gli_table.c 's/if (end < 0 || end > size) v->vstate/if (end < 0 || end >= size) v->vstate/' TABLE_C
mut capv8-no-restore gli_table.c 's/^\t_mesa_BindBuffer(0x8F36, (unsigned)old);/\t;/' TABLE_C
mut capv8-restore-zero gli_table.c 's/_mesa_BindBuffer(0x8F36, (unsigned)old);/_mesa_BindBuffer(0x8F36, 0);/' TABLE_C
mut capv8-bind-wrong-target gli_table.c 's/_mesa_BindBuffer(0x8F36, (unsigned)v->at\[0\].buf);/_mesa_BindBuffer(0x8892, (unsigned)v->at[0].buf);/' TABLE_C
mut capv8-old-binding-pname gli_table.c 's/_mesa_GetIntegerv(0x8F36 \/\* COPY_READ_BUFFER binding \*\/, &old);/_mesa_GetIntegerv(0x8892, \&old);/' TABLE_C
mut capv8-old-unknown-ok gli_table.c 's/if (old < 0) return;/;/' TABLE_C
mut capv8-mapped-ignored gli_table.c 's/if (mapped == 1) v->vstate = SWEET_CAPV_V_MAPPED;/if (0) v->vstate = SWEET_CAPV_V_MAPPED;/' TABLE_C
mut capv8-mapped-unknown-reads gli_table.c 's/else if (mapped == 0 \&\& size >= 0) {/else if (size >= 0) {/' TABLE_C
mut capv8-size-unknown-reads gli_table.c 's/else if (mapped == 0 \&\& size >= 0) {/else if (mapped == 0) {/' TABLE_C
mut capv8-total-33 gli_table.c 's/#define GLI_CAPV_TOTAL 32 /#define GLI_CAPV_TOTAL 33 /' TABLE_C
mut capv8-total-not-counted gli_table.c 's/{ capv_total++; ub_c\[c\].capv_n++;/{ ub_c[c].capv_n++;/' TABLE_C
mut capv8-cap-line-every-time gli_table.c 's/else if (!capv_capped) { capv_capped = 1; capv = 2; }/else { capv = 2; }/' TABLE_C
mut capv8-bind-no-reset gli_table.c 's/ub_c\[c\].capv_n = 0; ub_c/ub_c/' TABLE_C
mut capv8-window-5 gli_table.c 's/++ub_c\[c\].capv_e <= GLI_CAPV_WINDOW/++ub_c[c].capv_e <= 5/' TABLE_C
mut capv8-kind-any gli_table.c "s/if (kc\[ki\] == 'o' \&\& d->cls == 0 \&\&/if (d->cls == 0 \&\&/" TABLE_C
mut capv8-class-any gli_table.c 's/ \&\& d->cls == 0 \&\& d->vp\[3\]/ \&\& d->vp[3]/' TABLE_C
# 'd->cls == 0' -> 'd->cls <= 1' is equivalent since the height gate: class 1 needs h <= 256 (sweet_census_class), the gate needs h >= 1000 (AP-26); the class-2 case (800x1200) is killed by capv7-class-any.
mut capv8-off-guard-removed gli_table.c 's/off > (1UL << 40) ? -1 : (long long)off/0 ? -1 : (long long)off/' TABLE_C
mut capv8-height-gate-removed gli_table.c 's/ \&\& d->vp\[3\] >= GLI_CAPV_MIN_H//' TABLE_C
mut capv8-height-gate-off-by-one gli_table.c 's/d->vp\[3\] >= GLI_CAPV_MIN_H/d->vp[3] > GLI_CAPV_MIN_H/' TABLE_C
mut capv8-height-gate-300 gli_table.c 's/#define GLI_CAPV_MIN_H 1000 /#define GLI_CAPV_MIN_H 100 /' TABLE_C
mut capv8-vp-height-as-width gli_table.c 's/v.vw = d->vp\[2\]; v.vh = d->vp\[3\];/v.vw = d->vp[2]; v.vh = d->vp[2];/' TABLE_C
mut capv8-fbo0-eligible gli_table.c 's/ \&\& ub_c\[c\].fbo != 0 \&\& sweet_px_sched_on()/ \&\& sweet_px_sched_on()/' TABLE_C
mut capv8-no-sched-gate gli_table.c 's/ \&\& sweet_px_sched_on() \&\& ++ub_c/ \&\& ++ub_c/' TABLE_C
mut capv8-elems-idx gli_table.c '/^static void hook_drawelements_census/,/^}/ s/ d.idxtype = type;//' TABLE_C
mut capv8-range-idx gli_table.c '/^static void hook_drawrangeelements_census/,/^}/ s/ d.idxtype = type;//' TABLE_C
mut capv8-elemsinst-idx gli_table.c '/^static void hook_drawelementsinstanced_census/,/^}/ s/ d.idxtype = type;//' TABLE_C
mut capv8-arrays-first-unset gli_table.c 's/d.count = count; d.first = first;/d.count = count;/' TABLE_C
mut capv8-arrays-inst-first-set gli_table.c '/^static void hook_drawarraysinstanced_census/,/^}/ s/d.mode = mode; d.count = count;/d.mode = mode; d.count = count; d.first = first;/' TABLE_C
mut capv8-mat4-floats gli_table.c 's/case 0x8B5C: return 16;/case 0x8B5C: return 12;/' TABLE_C
mut capv8-sampler-2d gli_table.c 's/return t == 0x8B5E || /return /' TABLE_C
mut capv8-stride-zero gli_table.c 's/stride = v->at\[0\].st ? v->at\[0\].st : elem/stride = v->at[0].st/' TABLE_C
mut capv8-size-1 gli_table.c 's/v.at\[0\].sz < 2 ||/v.at[0].sz < 1 ||/' TABLE_C
mut capv8-size-5 gli_table.c 's/v.at\[0\].sz > 4 ||/v.at[0].sz > 5 ||/' TABLE_C
mut capv8-type-any gli_table.c 's/v.at\[0\].ty != 0x1406 ||/v.at[0].ty == 0 ||/' TABLE_C
mut capv8-disabled-reads gli_table.c 's/else if (v.at\[0\].en != 1) v.vstate = SWEET_CAPV_V_DIS;/else if (0) v.vstate = SWEET_CAPV_V_DIS;/' TABLE_C
mut capv8-vbo-branch-takes-client gli_table.c 's/if (ok \&\& v.at\[0\].buf > 0) capv_verts/if (ok \&\& v.at[0].buf >= 0) capv_verts/' TABLE_C
mut capv8-negative-stride gli_table.c 's/ || v.at\[0\].st < 0) v.vstate = SWEET_CAPV_V_TYPE/) v.vstate = SWEET_CAPV_V_TYPE/' TABLE_C
mut capv8-count-zero-reads gli_table.c 's/else if (d->count <= 0) v.vstate = SWEET_CAPV_V_NONE;/else if (d->count < 0) v.vstate = SWEET_CAPV_V_NONE;/' TABLE_C
mut capv8-first-zero-no-read gli_table.c 's/if (d->first < 0 \&\& !d->idxtype)/if (d->first <= 0 \&\& !d->idxtype)/' TABLE_C
mut capv8-four-vertices gli_table.c 's/nv = d->count < 4 ? d->count : 4; for/nv = 4; for/' TABLE_C
mut capv8-attr-offset gli_table.c 's/v.at\[a\].off = (unsigned long)(uintptr_t)ptr;/v.at[a].off = 0;/' TABLE_C
mut capv8-maxlen-uncapped gli_table.c 's/if (maxl > 64) maxl = 64;/;/' TABLE_C
mut capv8-unlinked-queried gli_table.c 's/if (link != 1) return;/;/' TABLE_C
mut capv8-noloc-queried gli_table.c 's/if (loc < 0) u->kind = SWEET_CAPV_U_NOLOC;/if (0) u->kind = SWEET_CAPV_U_NOLOC;/' TABLE_C
mut capv8-umore-lost gli_table.c 's/v->umore = nact > i ? nact - i : 0;/v->umore = 0;/' TABLE_C
mut capv8-uniform-cap-off gli_table.c 's/i < nact \&\& v->nu < SWEET_CAPV_U; i++/i < nact \&\& v->nu < SWEET_CAPV_U + 1; i++/' TABLE_C
mut capv8-prov-missing gli_table.c 's/sweet_prov_line(pl, sizeof pl, 0, fbo, v.tex);/pl[0] = 0;/' TABLE_C
mut capv8-tex-missing gli_table.c 's/sweet_tu_snapshot(sweet_th_ctx_index(rctx), v.tex);/;/' TABLE_C
# capv8 additions (batch 8): skip gate, 230-byte cap, client-array vertex read (capv_cli_verts), client index read (capv_indices).
# 'capv7-tail-name' dropped: the " | err -" tail no longer exists in the capv8 formatter.
mut capv8-skip-gate-removed gli_table.c 's/ \&\& !(d->vp\[2\] == GLI_CAPV_SKIP_W \&\& d->vp\[3\] == GLI_CAPV_SKIP_H)//' TABLE_C
mut capv8-skip-gate-or gli_table.c 's/!(d->vp\[2\] == GLI_CAPV_SKIP_W \&\& d->vp\[3\] == GLI_CAPV_SKIP_H)/!(d->vp[2] == GLI_CAPV_SKIP_W || d->vp[3] == GLI_CAPV_SKIP_H)/' TABLE_C
mut capv8-skip-w gli_table.c 's/#define GLI_CAPV_SKIP_W 1125/#define GLI_CAPV_SKIP_W 1126/' TABLE_C
mut capv8-skip-h gli_table.c 's/#define GLI_CAPV_SKIP_H 2436/#define GLI_CAPV_SKIP_H 2437/' TABLE_C
mut capv8-line-cap-unbounded gli_table.c 's/lim = n < SWEET_CAPV_MAX ? n : SWEET_CAPV_MAX/lim = n/' TABLE_C
mut capv8-prov-cap-off gli_table.c 's/if (strlen(pl) >= SWEET_CAPV_MAX) strcpy/if (strlen(pl) >= 100000) strcpy/' TABLE_C
mut capv8-prov-cut-offset gli_table.c 's/pl + SWEET_CAPV_MAX - 6/pl + SWEET_CAPV_MAX - 4/' TABLE_C
mut capv8-cli-null-ptr-reads gli_table.c 's/v.at\[0\].buf == 0 \&\& !ptr0/v.at[0].buf == 0 \&\& 0/' TABLE_C
mut capv8-cli-buf-any gli_table.c 's/else if (ok \&\& v.at\[0\].buf == 0) capv_cli_verts/else if (ok) capv_cli_verts/' TABLE_C
mut capv8-cli-not-called gli_table.c 's/else if (ok \&\& v.at\[0\].buf == 0) capv_cli_verts/else if (0) capv_cli_verts/' TABLE_C
mut capv8-cli-stride-zero gli_table.c 's/long long stride = v->at\[0\].st ? v->at\[0\].st : (long long)sz \* 4;/long long stride = v->at[0].st;/' TABLE_C
mut capv8-cli-stride-ignored gli_table.c 's/long long stride = v->at\[0\].st ? v->at\[0\].st : (long long)sz \* 4;/long long stride = (long long)sz * 4;/' TABLE_C
mut capv8-cli-copy-size gli_table.c 's/(const char \*)ptr + (long long)vi\[i\] \* stride, (size_t)sz \* 4)/(const char *)ptr + (long long)vi[i] * stride, (size_t)sz * 2)/' TABLE_C
mut capv8-cli-index-ignored gli_table.c 's/(const char \*)ptr + (long long)vi\[i\] \* stride/(const char *)ptr + (long long)i * stride/' TABLE_C
mut capv8-cli-vsz gli_table.c '/^static void capv_cli_verts/,/^}/ s/v->vsz = sz;/v->vsz = 4;/' TABLE_C
mut capv8-cli-nv gli_table.c '/^static void capv_cli_verts/,/^}/ s/v->nv = nv;/v->nv = nv - 1;/' TABLE_C
mut capv8-idx-bound-pname gli_table.c 's/_mesa_GetIntegerv(0x8895 /_mesa_GetIntegerv(0x8894 /' TABLE_C
mut capv8-idx-query-fail-ok gli_table.c 's/if (eb < 0) { v->vstate = SWEET_CAPV_V_IDXQ; return 0; }/;/' TABLE_C
mut capv8-idx-bound-ignored gli_table.c 's/if (eb > 0) { v->vstate = SWEET_CAPV_V_IDXBUF; return 0; }/;/' TABLE_C
mut capv8-idx-bound-gt1 gli_table.c 's/if (eb > 0) { v->vstate = SWEET_CAPV_V_IDXBUF; return 0; }/if (eb > 1) { v->vstate = SWEET_CAPV_V_IDXBUF; return 0; }/' TABLE_C
mut capv8-idx-null-ptr gli_table.c 's/if (!d->indices || !isz)/if (!isz)/' TABLE_C
mut capv8-idx-bad-type-reads gli_table.c 's/if (!d->indices || !isz)/if (!d->indices)/' TABLE_C
mut capv8-idx-u8-type gli_table.c 's/d->idxtype == 0x1401 ? 1/d->idxtype == 0x1402 ? 1/' TABLE_C
mut capv8-idx-u16-type gli_table.c 's/d->idxtype == 0x1403 ? 2/d->idxtype == 0x1404 ? 2/' TABLE_C
mut capv8-idx-u32-type gli_table.c 's/d->idxtype == 0x1405 ? 4/d->idxtype == 0x1406 ? 4/' TABLE_C
mut capv8-idx-u8-off gli_table.c 's/(const char \*)d->indices + i, 1)/(const char *)d->indices + i + 1, 1)/' TABLE_C
mut capv8-idx-u16-stride gli_table.c 's/(const char \*)d->indices + 2 \* i, 2)/(const char *)d->indices + i, 2)/' TABLE_C
mut capv8-idx-u32-stride gli_table.c 's/(const char \*)d->indices + 4 \* i, 4)/(const char *)d->indices + 2 * i, 4)/' TABLE_C
mut capv8-idx-min-not-tracked gli_table.c 's/if (x < mn) mn = x;/;/' TABLE_C
mut capv8-idx-max-not-tracked gli_table.c 's/if (x > mx) mx = x;/;/' TABLE_C
mut capv8-idx-min-flipped gli_table.c 's/if (x < mn) mn = x;/if (x > mn) mn = x;/' TABLE_C
mut capv8-idx-max-flipped gli_table.c 's/if (x > mx) mx = x;/if (x < mx) mx = x;/' TABLE_C
mut capv8-idx-vi-3 gli_table.c 's/if (i < 4) vi\[i\] = x;/if (i < 3) vi[i] = x;/' TABLE_C
mut capv8-idx-n-128 gli_table.c 's/n = d->count < 64 ? d->count : 64,/n = d->count < 128 ? d->count : 128,/' TABLE_C
# capv8-idx-nidx (v->nidx = n instead of min(n,4)) is equivalent: the formatter prints min(nidx,4) entries (sweet_capv_line tag v) and memcpy(v->idx) copies a fixed 4.
mut capv8-idx-nv gli_table.c 's/\*nv = n < 4 ? n : 4;/*nv = n;/' TABLE_C
mut capv8-idx-range-ge gli_table.c 's/if (mx > 65535)/if (mx >= 65535)/' TABLE_C
mut capv8-idx-range-65536 gli_table.c 's/if (mx > 65535)/if (mx > 65536)/' TABLE_C
mut capv8-idx-range-off gli_table.c 's/if (mx > 65535) { v->vstate = SWEET_CAPV_V_IDXRANGE; return 0; }/;/' TABLE_C
mut capv8-arrays-first-offset gli_table.c 's/vi\[k\] = (unsigned)d->first + (unsigned)k;/vi[k] = (unsigned)k;/' TABLE_C
mut capv8-elems-indices-unset gli_table.c '/^static void hook_drawelements_census/,/^}/ s/ d.indices = indices;//' TABLE_C
mut capv8-range-indices-unset gli_table.c '/^static void hook_drawrangeelements_census/,/^}/ s/ d.indices = indices;//' TABLE_C
# capv8-elemsinst-indices-unset dropped: instanced draws never read indices (inst => `v inst`), so the pointer is not stored (equivalent mutant).
# P-G0 slice c (flush timing, SWEETGL_FLUSHT). Not listed: u32 count saturation (needs 4e9 calls, equivalent for the host test); the first-call window never prints empty (equivalent); the __APPLE__ branches of ft_now / sweet_ft_init (mach_timebase_info) are not compiled on the host.
mut ft-parse-zero-bad gli_table.c 's/if (!e || !e\[0\] || (e\[0\] == .0. \&\& !e\[1\])) return 0;/if (!e || !e[0]) return 0;/' TABLE_C
mut ft-parse-prefix gli_table.c "s/if (e\[0\] == '1' \&\& !e\[1\]) return 1;/if (e[0] == '1') return 1;/" TABLE_C
mut ft-parse-bad-silent gli_table.c 's/^\t\*bad = 1;$/\t;/' TABLE_C
mut ft-always-installed gli_table.c 's/if (sweet_ft_init(getenv("SWEETGL_FLUSHT"))) ft_install(t);/sweet_ft_init(getenv("SWEETGL_FLUSHT")); ft_install(t);/' TABLE_C
mut ft-loud-every-time gli_table.c 's/if (bad \&\& !ft_loud_done) {/if (bad) {/' TABLE_C
mut ft-tb-loud-every-time gli_table.c 's/if (!on \&\& !ft_loud_done) {/if (!on) {/' TABLE_C
mut ft-tbfail-ignored gli_table.c 's/if (sweet_ft_tb_fail) on = 0;/if (0) on = 0;/' TABLE_C
# ft-slots-swapped dropped: w_89 and w_90 have the same body (gli_table_gen.c:56,58), swapping the slots is an equivalent mutant (review pg0-r1 F7, AP-33).
mut ft-flush-next-not-called gli_table.c 's/^\t((void (\*)(void \*))ft_next_fl)(rctx);$/\t;/' TABLE_C
mut ft-finish-next-not-called gli_table.c 's/^\t((void (\*)(void \*))ft_next_fi)(rctx);$/\t;/' TABLE_C
mut ft-flush-counted-as-finish gli_table.c 's/ft_record(0, t0, t1);/ft_record(1, t0, t1);/' TABLE_C
mut ft-finish-counted-as-flush gli_table.c 's/ft_record(1, t0, t1);/ft_record(0, t0, t1);/' TABLE_C
mut ft-ticks-no-remainder gli_table.c 's/ + (t % denom) \* numer \/ denom;/;/' TABLE_C
mut ft-ticks-denom0 gli_table.c 's/if (denom == 0) return 0;/;/' TABLE_C
mut ft-us-divisor gli_table.c 's/w->flush_in_ns \/ 1000,/w->flush_in_ns \/ 100,/' TABLE_C
mut ft-max-not-saturated gli_table.c 's/return v > 0xffffffffull ? 0xffffffffu : (unsigned)v;/return (unsigned)v;/' TABLE_C
mut ft-sum-wraps gli_table.c 's/return a + b < a ? ~0ull : a + b;/return a + b;/' TABLE_C
mut ft-window-length gli_table.c 's/(t1 - ft_base) \/ 1000000000ull/(t1 - ft_base) \/ 100000000ull/' TABLE_C
mut ft-base-is-t0 gli_table.c 's/ft_base = t1; ft_cur = 0;/ft_base = t0; ft_cur = 0;/' TABLE_C
mut ft-late-exit-flips gli_table.c 's/^\tif (wid < ft_cur) wid = ft_cur;$/\t;/' TABLE_C
mut ft-dur-backwards-wraps gli_table.c 's/dur = t1 > t0 ? t1 - t0 : 0,/dur = t1 - t0,/' TABLE_C
mut ft-flush-max-untracked gli_table.c 's/if (dur > ft_win.flush_max_ns) ft_win.flush_max_ns = dur;/;/' TABLE_C
mut ft-finish-max-untracked gli_table.c 's/if (dur > ft_win.finish_max_ns) ft_win.finish_max_ns = dur;/;/' TABLE_C
mut ft-flush-max-flipped gli_table.c 's/dur > ft_win.flush_max_ns/dur < ft_win.flush_max_ns/' TABLE_C
mut ft-finish-max-flipped gli_table.c 's/dur > ft_win.finish_max_ns/dur < ft_win.finish_max_ns/' TABLE_C
mut ft-last-exit-is-entry gli_table.c 's/if (t1 > ft_win.last_exit_ns) ft_win.last_exit_ns = t1;/if (t0 > ft_win.last_exit_ns) ft_win.last_exit_ns = t0;/' TABLE_C
mut ft-window-not-reset gli_table.c 's/^\t\tmemset(&ft_win, 0, sizeof ft_win);$/\t\t;/' TABLE_C
mut ft-done-sec-is-new gli_table.c 's/\*done_sec = ft_cur;/*done_sec = wid;/' TABLE_C
mut ft-pid-zero gli_table.c 's/sweet_ft_line(l, sizeof l, (int)getpid(), sec, &d);/sweet_ft_line(l, sizeof l, 0, sec, \&d);/' TABLE_C
mut ft-line-unbounded gli_table.c 's/return snprintf(buf, n, "SWEETGL: FLUSHT/return sprintf(buf, "SWEETGL: FLUSHT/' TABLE_C
mut ft-format-order gli_table.c 's/ flush %u flush_in_us/ flush_n %u flush_in_us/' TABLE_C
# capv8-r1 additions: F1 instanced draws (inst flag), F4 eligible-draw window, F3 index header before the reason word.
mut capv8-inst-ignored gli_table.c 's/if (d->inst) v.vstate = SWEET_CAPV_V_INST;/if (0) v.vstate = SWEET_CAPV_V_INST;/' TABLE_C
mut capv8-dei-inst-unset gli_table.c '/^static void hook_drawelementsinstanced_census/,/^}/ s/ d.inst = 1;//' TABLE_C
mut capv8-dai-inst-unset gli_table.c '/^static void hook_drawarraysinstanced_census/,/^}/ s/ d.inst = 1;//' TABLE_C
mut capv8-window-counts-all gli_table.c 's/++ub_c\[c\].capv_e <= GLI_CAPV_WINDOW/cd->draws <= GLI_CAPV_WINDOW/' TABLE_C
mut capv8-window-no-reset gli_table.c 's/ub_c\[c\].capv_e = 0; memset/memset/' TABLE_C
mut capv8-idx-header-values-only gli_table.c 's/if (k == 0 \&\& v->hasidx) {/if (k == 0 \&\& v->hasidx \&\& v->vstate == SWEET_CAPV_V_VALUES) {/' TABLE_C
# P-G1 slice 2: SWEETGL: BASE line
mut base-image-decimal gli_table.c 's/image 0x%llx slide/image 0x%llu slide/' TABLE_C
mut base-slide-hex-dec gli_table.c 's/slide 0x%llx size/slide 0x%llu size/' TABLE_C
mut base-path-uncut gli_table.c 's/path %.80s", pid/path %.300s", pid/' TABLE_C
mut base-unavail-uncut gli_table.c 's/BASE unavailable (%.80s)/BASE unavailable (%s)/' TABLE_C
mut base-every-init gli_table.c 's/if (__atomic_exchange_n(&base_done, 1, __ATOMIC_ACQ_REL)) return 0;/base_done = 0; if (__atomic_exchange_n(\&base_done, 1, __ATOMIC_ACQ_REL)) return 0;/' TABLE_C
mut base-never-done gli_table.c 's/__atomic_exchange_n(&base_done, 1, __ATOMIC_ACQ_REL)/0/' TABLE_C
mut base-not-called gli_table.c 's/^\tsweet_base_report();   \/\* once per process/\t;   \/* once per process/' TABLE_C
mut base-nopath-ok gli_table.c 's/ || !bi.path) { sweet_base_unavailable_line/) { sweet_base_unavailable_line/' TABLE_C
mut base-resolver-fail-quiet gli_table.c 's/sweet_base_unavailable_line(l, sizeof l, why); fprintf(stderr, "%s\\n", l); return -1;/return -1;/' TABLE_C
mut base-size-zero gli_table.c 's/size = (unsigned long long)st.st_size;/size = 0;/' TABLE_C
mut base-stat-rc gli_table.c 's/else { rc = -2; snprintf/else { rc = 1; snprintf/' TABLE_C
mut base-stat-quiet gli_table.c 's/if (rc == -2) { sweet_base_unavailable_line/if (0) { sweet_base_unavailable_line/' TABLE_C
mut base-pid-zero gli_table.c 's/sweet_base_line(l, sizeof l, (int)getpid(), &bi, size)/sweet_base_line(l, sizeof l, 0, \&bi, size)/' TABLE_C
mut base-host-why gli_table.c 's/no dyld (host build)/no dyld/' TABLE_C
mut base-reset-keeps-done gli_table.c 's/void sweet_base_reset(void) { base_done = 0; /void sweet_base_reset(void) { /' TABLE_C
mut base-reset-keeps-resolver gli_table.c 's/base_done = 0; base_resolver = base_resolve_default; }/base_done = 0; }/' TABLE_C
mut base-setnull-crash gli_table.c 's/base_resolver = fn ? fn : base_resolve_default;/base_resolver = fn;/' TABLE_C
mut base-null-path-q gli_table.c 's/bi->path ? bi->path : "?"/bi->path/' TABLE_C
echo "$n mutants, $bad survived"
[ "$bad" = 0 ] && echo "all mutants killed"
exit "$bad"
