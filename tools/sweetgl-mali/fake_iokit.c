#include "fake_iokit.h"
#include <stdlib.h>
#include <string.h>

#define MAXBO 4096
struct fbo {
	int used;
	uint64_t va, bytes;
	void *mem;
};
static struct fbo bos[MAXBO];
static uint64_t next_va;
struct fake_state fake;

void
fake_reset(void)
{
	for (int i = 0; i < MAXBO; i++) {
		free(bos[i].mem);
	}
	memset(bos, 0, sizeof(bos));
	memset(&fake, 0, sizeof(fake));
	fake.service_present = 1;
	fake.gpu_id = 0x92020010u;
	next_va = 0x2000000;
}

void
fake_add_surface(uint32_t id, uint64_t len, uint32_t pitch, uint32_t w, uint32_t h, uint32_t fmt)
{
	fake.surfaces[fake.nsurfaces++] = (struct fake_surface){ id, pitch, w, h, fmt, len };
}

unsigned
fake_count(uint32_t sel)
{
	unsigned n = 0;
	for (unsigned i = 0; i < fake.ncalls; i++) {
		n += fake.calls[i].sel == sel;
	}
	return n;
}

const struct fake_call *
fake_last(uint32_t sel)
{
	for (unsigned i = fake.ncalls; i-- > 0;) {
		if (fake.calls[i].sel == sel) {
			return &fake.calls[i];
		}
	}
	return NULL;
}

FakeMatching
IOServiceMatching(const char *name)
{
	return name;
}

io_service_t
IOServiceGetMatchingService(mach_port_t port, FakeMatching m)
{
	(void)port;
	return fake.service_present && m && strcmp(m, "SweetMali") == 0 ? 77 : 0;
}

kern_return_t
IOServiceOpen(io_service_t svc, mach_port_t task, uint32_t type, io_connect_t *c)
{
	(void)task;
	(void)type;
	if (svc != 77 || fake.open_result) {
		return fake.open_result ? fake.open_result : kIOReturnBadArgument;
	}
	fake.opens++;
	*c = 5;
	return kIOReturnSuccess;
}

kern_return_t
IOServiceClose(io_connect_t c)
{
	(void)c;
	fake.closes++;
	return kIOReturnSuccess;
}

kern_return_t
IOObjectRelease(io_object_t o)
{
	(void)o;
	return kIOReturnSuccess;
}

static kern_return_t
oneshot(uint32_t sel)
{
	kern_return_t k = sel < 8 ? fake.fail_sel[sel] : 0;
	if (sel < 8) {
		fake.fail_sel[sel] = 0;
	}
	return k;
}

kern_return_t
IOConnectCallMethod(mach_port_t c, uint32_t sel, const uint64_t *in, uint32_t nin, const void *ins, size_t nins, uint64_t *out,
    uint32_t *nout, void *outs, size_t *nouts)
{
	if (c != 5 || ins || nins) {
		return kIOReturnBadArgument;
	}
	if (fake.ncalls < sizeof(fake.calls) / sizeof(fake.calls[0])) {
		struct fake_call *fc = &fake.calls[fake.ncalls++];
		fc->sel = sel;
		fc->nin = nin;
		fc->in[0] = nin > 0 && in ? in[0] : 0;
		fc->in[1] = nin > 1 && in ? in[1] : 0;
	}
	kern_return_t inj = oneshot(sel);
	if (inj) {
		return inj;
	}
	switch (sel) {
	case 0: {
		struct { uint32_t w[17]; uint64_t va_start, va_end; } info = { .va_start = 0x2000000, .va_end = 1ull << 32 };
		for (unsigned i = 1; i < 17; i++) {
			info.w[i] = FAKE_INFO_WORD(i);      /* field order of struct sweet_mali_info */
		}
		info.w[0] = fake.gpu_id;
		if (nin != 0 || !outs || !nouts || *nouts != sizeof(info) || out) {
			return kIOReturnBadArgument;
		}
		memcpy(outs, &info, sizeof(info));
		return kIOReturnSuccess;
	}
	case 1: {                            /* boNew */
		if (nin != 2 || !out || !nout || *nout < 3 || in[0] == 0 || in[0] > (256ull << 20) || (in[1] & ~1ull)) {
			return kIOReturnBadArgument;
		}
		int s = 0;
		while (s < MAXBO && bos[s].used) {
			s++;
		}
		if (s == MAXBO) {
			return kIOReturnNoResources;
		}
		uint64_t len = (in[0] + 16383) & ~16383ull;
		bos[s] = (struct fbo){ .used = 1, .va = next_va, .bytes = len, .mem = calloc(1, len) };
		next_va += len;
		fake.live_bos++;
		out[0] = (uint64_t)s + 1;
		out[1] = bos[s].va;
		out[2] = len;
		*nout = 3;
		return kIOReturnSuccess;
	}
	case 2:
		if (nin != 1 || in[0] == 0 || in[0] > MAXBO || !bos[in[0] - 1].used) {
			return in && nin == 1 ? kIOReturnNotFound : kIOReturnBadArgument;
		}
		free(bos[in[0] - 1].mem);
		bos[in[0] - 1] = (struct fbo){ 0 };
		fake.live_bos--;
		return kIOReturnSuccess;
	case 3:
		if (nin != 2 || !out || !nout || *nout < 1 || (in[1] & ~1ull)) {
			return kIOReturnBadArgument;
		}
		out[0] = ++fake.seq;
		*nout = 1;
		return kIOReturnSuccess;
	case 4:
		if (nin != 2) {
			return kIOReturnBadArgument;
		}
		if (in[0] <= fake.completed) {
			return kIOReturnSuccess;
		}
		if (fake.complete_after_waits && fake.timeouts++ >= fake.complete_after_waits) {   /* n Timeouts, then done */
			fake.completed = fake.seq;
			return kIOReturnSuccess;
		}
		return kIOReturnTimeout;
	case 5: {                            /* importSurface: in id; out handle, va, len, pitch, w, h, format (0105 order) */
		if (nin != 1 || !out || !nout || *nout < 7) {
			return kIOReturnBadArgument;
		}
		const struct fake_surface *fs = NULL;
		for (unsigned i = 0; i < fake.nsurfaces; i++) {
			if (fake.surfaces[i].id == in[0]) {
				fs = &fake.surfaces[i];
			}
		}
		if (!fs) {
			return kIOReturnNotFound;
		}
		if (fs->len == 0 || (fs->len & 16383)) {
			return kIOReturnUnsupported;
		}
		int s = 0;
		while (s < MAXBO && bos[s].used) {
			s++;
		}
		if (s == MAXBO) {
			return kIOReturnNoResources;
		}
		bos[s] = (struct fbo){ .used = 1, .va = next_va, .bytes = fs->len, .mem = calloc(1, fs->len) };
		next_va += fs->len;
		fake.live_bos++;
		fake.imports++;
		out[0] = (uint64_t)s + 1;
		out[1] = bos[s].va;
		out[2] = fs->len;
		out[3] = fs->pitch;
		out[4] = fs->w;
		out[5] = fs->h;
		out[6] = fs->fmt;
		*nout = 7;
		return kIOReturnSuccess;
	}
	default:
		return kIOReturnUnsupported;
	}
}

kern_return_t
IOConnectMapMemory64(mach_port_t c, uint32_t type, mach_port_t task, mach_vm_address_t *addr, mach_vm_size_t *size, IOOptionBits opt)
{
	(void)task;
	if (c != 5 || type == 0 || type > MAXBO || !bos[type - 1].used) {
		return kIOReturnNotFound;
	}
	if (fake.map_result) {
		return fake.map_result;
	}
	fake.maps++;
	fake.last_map_opt = opt;
	*addr = (mach_vm_address_t)(uintptr_t)bos[type - 1].mem;
	*size = bos[type - 1].bytes;
	return kIOReturnSuccess;
}

kern_return_t
IOConnectUnmapMemory64(mach_port_t c, uint32_t type, mach_port_t task, mach_vm_address_t addr)
{
	(void)task;
	if (c != 5 || type == 0 || type > MAXBO || !bos[type - 1].used || addr != (mach_vm_address_t)(uintptr_t)bos[type - 1].mem) {
		return kIOReturnBadArgument;
	}
	fake.unmaps++;
	fake.last_unmap_addr = addr;
	return kIOReturnSuccess;
}
