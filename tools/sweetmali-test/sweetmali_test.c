/*
 * sweetmali-test: exercise the SweetMali user client (kernel patches 0104/0105, boot-arg sweetmali=1) from an iOS process:
 * query the GPU, allocate a BO, map it, run one Valhall WRITE_VALUE job through submit/wait, check the result, close the BO.
 * Output goes to stdout and to /dev/null (the sweetnull tee puts it into the kernel/USB log as "|sweetmali-test| ...", as sailfish's sweetgpu-test did).
 * Selectors and structs mirror iokit/Kernel/arm/SweetMali.cpp and bsd/dev/sweet_mali.h in the kernel repo. Build: tools/sweetmali-test/build.sh (macOS + Xcode).
 * The job is the one proven on the phone in run gpujob1 (docs/research/30 s.8): header word 4 = 0x10004, payload at +0x20 (target VA, type 7, magic).
 */
#include <IOKit/IOKitLib.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct sweet_mali_info {
	uint32_t gpu_id, l2_features, tiler_features, mem_features, mmu_features, as_present, js_present;
	uint32_t thread_max_threads, thread_max_workgroup, thread_max_barrier, thread_features, texture_features[4], afbc_features, shader_present;
	uint64_t va_start, va_end;
};

enum { SEL_QUERY, SEL_BO_NEW, SEL_BO_CLOSE, SEL_SUBMIT, SEL_WAIT };
#define MAGIC 0x504958454C494F53ull

static int g_null = -1;

static void
say(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fputs(buf, stdout);
	if (g_null >= 0 && n > 0) {
		(void)!write(g_null, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
	}
}

int
main(int argc, char **argv)
{
	/* argv[1]: nomap (stop after boNew), map (CPU map, default cache mode), wc (default: write-combine map, then run the job) */
	const char *mode = argc > 1 ? argv[1] : "wc";
	g_null = open("/dev/null", O_WRONLY);
	io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("SweetMali"));
	io_connect_t c = 0;
	if (!svc || IOServiceOpen(svc, mach_task_self(), 0, &c) != KERN_SUCCESS) {
		say("sweetmali-test: FAIL no SweetMali service or open refused (boot-arg sweetmali=1? second client?)\n");
		return 2;
	}
	struct sweet_mali_info info;
	size_t isz = sizeof(info);
	kern_return_t kr = IOConnectCallMethod(c, SEL_QUERY, NULL, 0, NULL, 0, NULL, NULL, &info, &isz);
	if (kr != KERN_SUCCESS) {
		say("sweetmali-test: FAIL query kr=%#x\n", kr);
		return 2;
	}
	say("sweetmali-test: GPU_ID=%#x shaders=%#x js=%#x as=%#x VA %#llx-%#llx\n", info.gpu_id, info.shader_present, info.js_present, info.as_present,
	    (unsigned long long)info.va_start, (unsigned long long)info.va_end);

	uint64_t in[2] = { 0x4000, 1 }, out[3] = { 0 };
	uint32_t outn = 3;
	kr = IOConnectCallMethod(c, SEL_BO_NEW, in, 2, NULL, 0, out, &outn, NULL, NULL);
	if (kr != KERN_SUCCESS || outn != 3) {
		say("sweetmali-test: FAIL boNew kr=%#x\n", kr);
		return 2;
	}
	uint64_t handle = out[0], gva = out[1], bytes = out[2];
	if (strcmp(mode, "nomap") == 0) {
		say("sweetmali-test: PASS nomap handle=%llu va=%#llx\n", (unsigned long long)handle, (unsigned long long)gva);
		IOConnectCallMethod(c, SEL_BO_CLOSE, &handle, 1, NULL, 0, NULL, NULL, NULL, NULL);
		IOServiceClose(c);
		return 0;
	}
	say("sweetmali-test: step boNew ok handle=%llu va=%#llx bytes=%llu\n", (unsigned long long)handle, (unsigned long long)gva, (unsigned long long)bytes);
	mach_vm_address_t addr = 0;
	mach_vm_size_t sz = 0;
	kr = IOConnectMapMemory64(c, (uint32_t)handle, mach_task_self(), &addr, &sz, kIOMapAnywhere | (strcmp(mode, "map") == 0 ? kIOMapDefaultCache : kIOMapWriteCombineCache));
	if (kr != KERN_SUCCESS) {
		say("sweetmali-test: FAIL map kr=%#x\n", kr);
		return 2;
	}
	say("sweetmali-test: step map ok addr=%#llx size=%llu\n", (unsigned long long)addr, (unsigned long long)sz);
	volatile uint32_t *d = (volatile uint32_t *)(uintptr_t)addr;
	d[0x10 / 4] = 0x10004u;
	*(volatile uint64_t *)(addr + 0x20) = gva + 0x100;
	d[0x28 / 4] = 7u;
	*(volatile uint64_t *)(addr + 0x30) = MAGIC;
	*(volatile uint64_t *)(addr + 0x100) = 0xa5a5a5a5a5a5a5a5ull;
	__sync_synchronize();
	say("sweetmali-test: step job written, submitting\n");
	uint64_t sub[2] = { gva, 0 }, seq[1] = { 0 };
	uint32_t seqn = 1;
	kr = IOConnectCallMethod(c, SEL_SUBMIT, sub, 2, NULL, 0, seq, &seqn, NULL, NULL);
	if (kr != KERN_SUCCESS) {
		say("sweetmali-test: FAIL submit kr=%#x\n", kr);
		return 2;
	}
	say("sweetmali-test: step submitted seq=%llu, waiting\n", (unsigned long long)seq[0]);
	uint64_t w[2] = { seq[0], 1000000 };
	kr = IOConnectCallMethod(c, SEL_WAIT, w, 2, NULL, 0, NULL, NULL, NULL, NULL);
	__sync_synchronize();
	uint64_t target = *(volatile uint64_t *)(addr + 0x100);
	int ok = kr == KERN_SUCCESS && target == MAGIC;
	say("sweetmali-test: %s bo=%llu va=%#llx bytes=%llu seq=%llu wait kr=%#x target=%#llx\n", ok ? "PASS" : "FAIL", (unsigned long long)handle,
	    (unsigned long long)gva, (unsigned long long)bytes, (unsigned long long)seq[0], kr, (unsigned long long)target);
	IOConnectUnmapMemory64(c, (uint32_t)handle, mach_task_self(), addr);
	IOConnectCallMethod(c, SEL_BO_CLOSE, &handle, 1, NULL, 0, NULL, NULL, NULL, NULL);
	IOServiceClose(c);
	return ok ? 0 : 1;
}
