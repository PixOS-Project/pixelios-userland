#!/bin/sh
# Each mutant is a sed edit of gen_gli.py / gli_table.c / gli_special.c; gli-host-test.sh (HEADERS=... SYMBOLS=...) must FAIL on it.
# Baseline first (AP-20). Needs HEADERS (and optionally SYMBOLS) like gli-host-test.sh.
cd "$(dirname "$0")"
[ -n "$HEADERS" ] || { echo "set HEADERS=<mesa>/include (generator mutants need it)"; exit 2; }
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
n=0; bad=0
./gli-host-test.sh >/dev/null 2>&1 || { echo 'baseline gli-host-test FAILS (HEADERS set? stale generated files?), mutation check meaningless'; exit 2; }
mut() { # name file sed-expr envvar
	n=$((n+1)); sed "$3" "$2" > "$D/m"
	if cmp -s "$2" "$D/m"; then echo "mutant $1: sed did not change anything"; bad=$((bad+1)); return; fi
	if env "$4=$D/m" ./gli-host-test.sh >"$D/out" 2>&1; then echo "SURVIVED $1"; bad=$((bad+1)); else echo "killed   $1"; fi
}
mut shift-args       gen_gli.py     "s/\['void \*rctx'\] + \['%s a%d'/['void *rctx', 'long pad'] + ['%s a%d'/" GEN_PY
mut ignore-override  gen_gli.py     "s/for n in (\[s\['to'\] and 'gl' + s\['to'\]\] if s\['to'\] else \[\]) + s\['names'\]:/for n in s['names']:/;s/^    cands = (\[s\['to'\].*\$/    cands = [v for n in s['names'] for v in variants(n)]/" GEN_PY
mut no-ensure-wired  gen_gli.py     "s/{ SWEET_GLI_ENSURE(rctx); %s }/{ %s }/" GEN_PY
mut no-ensure-special gli_special.c "s/unsigned object, unsigned name) { SWEET_GLI_ENSURE(rctx);/unsigned object, unsigned name) {/" SPECIAL_C
mut trap-every-time  gli_table.c    's/ && !seen\[slot\]//' TABLE_C
mut init-short       gli_table.c    's/sizeof(t->disp));/sizeof(t->disp) - sizeof(void *));/' TABLE_C
mut getstring-enum   gli_special.c  's/name == 0x8a1e/name == 0x8a1f/' SPECIAL_C
mut testobject-false gli_special.c  's/return 1; }/return 0; }/' SPECIAL_C
mut delete-branch    gli_special.c  's/if (_mesa_IsProgram(obj))/if (!_mesa_IsProgram(obj))/' SPECIAL_C
echo "$n mutants, $bad survived"
[ "$bad" = 0 ] && echo "all mutants killed"
exit "$bad"
