/* gli_table.c: table fill and trap logging. The slot tables are generated (gli_table_gen.c). */
#include "gli_table.h"
#include <stdio.h>
#include <string.h>

static void default_log(int slot, const char *name) { fprintf(stderr, "sweetgl: GLI slot %d (%s) is not implemented\n", slot, name); }
static void (*logger)(int, const char *) = default_log;
static unsigned char seen[SWEET_GLI_NSLOTS];   /* ponytail: racy bytes, a duplicate log line is the worst case */

void (*sweet_gli_ensure_current)(void *rctx);

void sweet_gli_set_log(void (*fn)(int, const char *)) { logger = fn; }

long sweet_gli_trap(int slot)
{
	if (slot >= 0 && slot < SWEET_GLI_NSLOTS && !seen[slot]) {
		seen[slot] = 1;
		if (logger) logger(slot, sweet_gli_names[slot]);
	}
	return 0;
}

void sweet_gli_init(struct sweet_gli *t, void *rctx)
{
	t->rctx = rctx;
	memcpy(t->disp, sweet_gli_defaults, sizeof(t->disp));
}
