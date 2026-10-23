/* Host test of gli_table: slot wiring (right _mesa_ function, rctx dropped, arguments not shifted), traps, specials. */
#include "gli_fake.h"
#include "gli_table.h"
#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const char *hit; static unsigned long args[5]; unsigned long fake_ret;
void fake_hit(const char *n) { hit = n; }
void fake_arg(int i, unsigned long v) { args[i] = v; }
/* _mesa_ functions the specials call that no slot is wired to directly (weak: the generated fakes win when they exist) */
#define WEAK __attribute__((weak))
WEAK const unsigned char *_mesa_GetString(unsigned n) { (void)n; hit = "_mesa_GetString"; return 0; }
WEAK void _mesa_DeleteProgram(unsigned n) { (void)n; hit = "_mesa_DeleteProgram"; }
WEAK void _mesa_DeleteShader(unsigned n) { (void)n; hit = "_mesa_DeleteShader"; }
WEAK unsigned char _mesa_IsProgram(unsigned n) { (void)n; return (unsigned char)fake_ret; }
WEAK void _mesa_Finish(void) { hit = "_mesa_Finish"; }
WEAK void _mesa_GetIntegerv(unsigned p, int *d) { (void)p; hit = "_mesa_GetIntegerv"; *d = 0; }

typedef unsigned long (*callfn)(void *, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long);
/* ponytail: calls every wrapper as if it took 5 integer args (true for the integer-class registers of both AAPCS64 and SysV);
 * the extra registers are ignored by shorter wrappers. Not a conforming call, fine for a host test. */
static unsigned long call(const struct sweet_gli *t, int slot)
{
	return ((callfn)t->disp[slot])(t->rctx, 1, 2, 3, 4, 5);
}

static int logged_slot, logged_n; static char logged_name[64];
static void logfn(int slot, const char *name) { logged_slot = slot; logged_n++; snprintf(logged_name, sizeof logged_name, "%s", name); }

static int slot_of(const char *name)
{
	for (int i = 0; i < SWEET_GLI_NSLOTS; i++)
		if (!strcmp(sweet_gli_names[i], name)) return i;
	return -1;
}

int main(void)
{
	static struct sweet_gli t;
	int dummy;
	sweet_gli_set_log(logfn);
	sweet_gli_init(&t, &dummy);
	CHECK(t.rctx == &dummy);
	CHECK(sizeof t.disp / sizeof t.disp[0] == 1036);

	int wired = 0, special = 0, traps = 0, named = 0;
	for (int i = 0; i < SWEET_GLI_NSLOTS; i++) {
		CHECK(t.disp[i] != NULL);
		if (strcmp(sweet_gli_names[i], "?")) named++;
		const char *tg = sweet_gli_targets[i];
		if (!tg) {   /* trap: logs once, returns 0, count matches */
			traps++;
			int before = logged_n;
			CHECK(call(&t, i) == 0);
			CHECK(logged_n == before + 1 && logged_slot == i && !strcmp(logged_name, sweet_gli_names[i]));
			CHECK(call(&t, i) == 0 && logged_n == before + 1);   /* once only */
		} else if (!strncmp(tg, "_mesa_", 6)) {   /* wired: the right fake, args after rctx in order */
			wired++;
			hit = NULL; memset(args, 0, sizeof args);
			fake_ret = 0;
			call(&t, i);
			CHECK(hit && !strcmp(hit, tg));
			int n = sweet_gli_nint[i] < 5 ? sweet_gli_nint[i] : 5;
			for (int k = 0; k < n; k++) CHECK(args[k] == (unsigned long)(k + 1));
		} else {
			special++;
			CHECK(!strncmp(tg, "sgli_", 5));
			call(&t, i);   /* must not crash */
		}
	}
	printf("slots: %d named, %d wired, %d special, %d trap\n", named, wired, special, traps);
	CHECK(wired > 300 && special >= 7 && named > 400);
	CHECK(sweet_gli_names[1035] != NULL);

	/* out-of-range trap is safe */
	CHECK(sweet_gli_trap(-1) == 0 && sweet_gli_trap(SWEET_GLI_NSLOTS) == 0);

	/* what CoreAnimation calls (R33 s.2.2, core spelling; the table's first name of a slot) must be wired or special */
	static const char *const need[] = { "glBindTexture", "glBlendEquation", "glBlendFunc", "glClear", "glClearColor", "glClearStencil",
		"glCompressedTexImage2D", "glCopyTexSubImage2D", "glDeleteTextures", "glDisable", "glDrawArrays", "glDrawElements", "glEnable",
		"glEnableClientState", "glFinish", "glFlush", "glGenTextures", "glGetError", "glGetIntegerv", "glGetString", "glPixelStorei",
		"glReadBuffer", "glScissor", "glTexImage2D", "glTexParameterf", "glTexParameteri", "glTexSubImage2D", "glViewport",
		"glActiveTexture", "glVertexAttribPointer", "glEnableVertexAttribArray", "glDisableVertexAttribArray", "glCreateShader",
		"glShaderSource", "glCompileShader", "glCreateProgram", "glAttachShader", "glLinkProgram", "glUseProgram", "glDeleteObjectARB",
		"glBindAttribLocation", "glGetUniformLocation", "glUniform1f", "glUniform1i", "glUniform4f", "glUniform3fv", "glUniform4fv",
		"glUniformMatrix3fv", "glUniformMatrix4fv", "glBindFramebuffer", "glGenFramebuffers", "glDeleteFramebuffers",
		"glFramebufferTexture2D", "glGenerateMipmap", "glFinishObjectAPPLE", "glTestObjectAPPLE", "glLabelObjectEXT",
		"glClientWaitSync", "glFramebufferParameteriAPPLE", "glFenceSync", "glDeleteSync", "glGetShaderiv", "glGetProgramiv",
		"glGetShaderInfoLog", "glGetProgramInfoLog", "glGetShaderPrecisionFormat", "glReleaseShaderCompiler", "glShaderBinary",
		"glInvalidateFramebuffer", "glGetIntegeri_v" };
	for (unsigned k = 0; k < sizeof need / sizeof need[0]; k++) {
		int s = slot_of(need[k]);
		if (s < 0 || !sweet_gli_targets[s]) printf("not wired: %s (slot %d)\n", need[k], s);
		CHECK(s >= 0 && sweet_gli_targets[s]);
	}
	CHECK(!strcmp(sweet_gli_targets[slot_of("glFlush")], "_mesa_Finish"));   /* glFlush == glFinish [src R33 s.3] */
	CHECK(slot_of("glGetString") == 117 && slot_of("glGetIntegerv") == 104);   /* offsets +0x3b0 / +0x348 [src R33 s.2.2] */

	/* specials */
	int gs = slot_of("glGetString");
	hit = NULL;
	CHECK(((const unsigned char *(*)(void *, unsigned))t.disp[gs])(t.rctx, 0x8a1e) == NULL && hit == NULL);   /* Apple-private enum: NULL, no Mesa call */
	((const unsigned char *(*)(void *, unsigned))t.disp[gs])(t.rctx, 0x1f03);
	CHECK(hit && !strcmp(hit, "_mesa_GetString"));
	hit = NULL; call(&t, slot_of("glFinishObjectAPPLE")); CHECK(hit && !strcmp(hit, "_mesa_Finish"));
	CHECK((call(&t, slot_of("glTestObjectAPPLE")) & 0xff) == 1);
	int del = slot_of("glDeleteObjectARB");
	fake_ret = 1; call(&t, del); CHECK(hit && !strcmp(hit, "_mesa_DeleteProgram"));
	fake_ret = 0; call(&t, del); CHECK(hit && !strcmp(hit, "_mesa_DeleteShader"));
	hit = NULL; call(&t, slot_of("glInsertEventMarkerEXT")); CHECK(hit == NULL);
	CHECK(sweet_gli_bound_texture_2d() == 0 && hit && !strcmp(hit, "_mesa_GetIntegerv"));

	printf("%d checks, %d failed\n", checks, fails);
	puts(fails ? "FAIL" : "PASS");
	return fails != 0;
}
