#!/bin/sh
# Each mutant is a sed edit of a copy of eagl_core.c; host-test.sh must FAIL on every one.
cd "$(dirname "$0")"
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
n=0; bad=0
./host-test.sh >/dev/null 2>&1 || { echo 'baseline host-test FAILS, mutation check meaningless'; exit 2; }
mut() { # name sed-expr
	n=$((n+1)); sed "$2" eagl_core.c > "$D/m.c"
	if cmp -s eagl_core.c "$D/m.c"; then echo "mutant $1: sed did not change anything"; bad=$((bad+1)); return; fi
	if EAGL_CORE_SRC="$D/m.c" ./host-test.sh >/dev/null 2>&1; then echo "SURVIVED $1"; bad=$((bad+1)); else echo "killed   $1"; fi
}
mut accept-es2        's/got != SGL_API_ES3/got < SGL_API_ES2/'
mut global-current    's/static _Thread_local sgl_ctx \*tl_cur/static sgl_ctx *tl_cur/'
mut destroy-bound     's/else if (c->bound) r = -EBUSY;/else if (0) r = -EBUSY;/'
mut param-no-update   's/if (!r) c->params\[i\].value = value;/if (!r \&\& i == c->nparams - 1) c->params[i].value = value;/'
mut attach-no-replace 's/c->att\[i\].s = \*surf;/if (i == c->natt - 1 \&\& 0) c->att[i].s = *surf;/'
mut pitch-overflow    's/(uint64_t)s->bytes_per_row \* s->h/(uint32_t)(s->bytes_per_row * s->h)/'
mut skip-current-chk  's/if (c != tl_cur) return -EPERM;/if (0) return -EPERM;/'
mut leak-es2-ctx      's/if (be.destroy_context) be.destroy_context(be.user, b);//'
mut ensure-noop 's/(void)sgl_set_current(rctx); .*$//'
echo "$n mutants, $bad survived"
[ "$bad" = 0 ] && echo "all mutants killed"
exit "$bad"
