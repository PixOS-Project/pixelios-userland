/*
 * Fake IOKit for the host test: only the calls sweet_drm_mali.c uses, in memory, deterministic. It plays the SweetMali kernel
 * (selectors 0-5 and BO mapping as in sweet_drm_mali.h), validates argument counts like SweetMali.cpp does, and records calls.
 * kIOReturn values are from memory of IOReturn.h [guess]; only the names matter to the code under test.
 */
#ifndef FAKE_IOKIT_H
#define FAKE_IOKIT_H
#include <stddef.h>
#include <stdint.h>

typedef int kern_return_t;
typedef uint32_t mach_port_t, io_object_t, io_service_t, io_connect_t, IOOptionBits;
typedef uint64_t mach_vm_address_t, mach_vm_size_t;
typedef const char *FakeMatching;

#define kIOReturnSuccess      0
#define kIOReturnNoMemory     ((kern_return_t)0xe00002bd)
#define kIOReturnNoResources  ((kern_return_t)0xe00002be)
#define kIOReturnBadArgument  ((kern_return_t)0xe00002c2)
#define kIOReturnUnsupported  ((kern_return_t)0xe00002c7)
#define kIOReturnIOError      ((kern_return_t)0xe00002ca)
#define kIOReturnBusy         ((kern_return_t)0xe00002d5)
#define kIOReturnTimeout      ((kern_return_t)0xe00002d6)
#define kIOReturnNotReady     ((kern_return_t)0xe00002d8)
#define kIOReturnNotPermitted ((kern_return_t)0xe00002e2)
#define kIOReturnNotFound     ((kern_return_t)0xe00002f0)
#define kIOMainPortDefault    0
#define kIOMapAnywhere        1u
#define kIOMapWriteCombineCache 0x400u
#define mach_task_self()      ((mach_port_t)1)

FakeMatching IOServiceMatching(const char *name);
io_service_t IOServiceGetMatchingService(mach_port_t port, FakeMatching m);
kern_return_t IOServiceOpen(io_service_t svc, mach_port_t task, uint32_t type, io_connect_t *c);
kern_return_t IOServiceClose(io_connect_t c);
kern_return_t IOObjectRelease(io_object_t o);
kern_return_t IOConnectCallMethod(mach_port_t c, uint32_t sel, const uint64_t *in, uint32_t nin, const void *ins, size_t nins,
    uint64_t *out, uint32_t *nout, void *outs, size_t *nouts);
kern_return_t IOConnectMapMemory64(mach_port_t c, uint32_t type, mach_port_t task, mach_vm_address_t *addr, mach_vm_size_t *size, IOOptionBits opt);
kern_return_t IOConnectUnmapMemory64(mach_port_t c, uint32_t type, mach_port_t task, mach_vm_address_t addr);

/* query result word i (index in struct sweet_mali_info order: 1 l2, 2 tiler, 3 mem, 4 mmu, 5 as, 6 js, 7 max_threads, 8 workgroup,
 * 9 barrier, 10 thread_features, 11-14 texture[0-3], 15 afbc, 16 shader_present); word 0 is fake.gpu_id */
#define FAKE_INFO_WORD(i) (0x1000u + (i) * 0x111u)

/* ---- test control ---- */
struct fake_call {
	uint32_t sel;
	uint64_t in[2];
	uint32_t nin;
};
struct fake_surface {
	uint32_t id, pitch, w, h, fmt;
	uint64_t len;                        /* not a multiple of 16 KiB (or 0): selector 5 says kIOReturnUnsupported, as 0105 does */
};
struct fake_state {
	/* inputs */
	int service_present;                 /* 0: IOServiceGetMatchingService fails */
	kern_return_t open_result;
	uint32_t gpu_id;                     /* query result: the rest is derived from it */
	uint64_t completed;                  /* seqnos <= completed are done */
	unsigned complete_after_waits;       /* if > 0: after this many Timeout waits, everything submitted completes */
	kern_return_t fail_sel[8];           /* one-shot failure per selector (0 = none) */
	kern_return_t map_result;
	struct fake_surface surfaces[8];     /* IOSurfaces selector 5 knows; others: kIOReturnNotFound */
	unsigned nsurfaces;
	/* outputs */
	unsigned imports;                    /* successful selector 5 calls */
	struct fake_call calls[512];
	unsigned ncalls;
	uint64_t seq;                        /* last seqno handed out */
	unsigned opens, closes, maps, unmaps, timeouts;
	uint32_t live_bos;
	uint64_t last_map_opt;
	uint64_t last_unmap_addr;
};
extern struct fake_state fake;
void fake_add_surface(uint32_t id, uint64_t len, uint32_t pitch, uint32_t w, uint32_t h, uint32_t fmt);
void fake_reset(void);                   /* zero everything, service present, GPU_ID 0x92020010 */
unsigned fake_count(uint32_t sel);       /* recorded calls with this selector */
const struct fake_call *fake_last(uint32_t sel);
#endif
