/* bt-romget (sprint-bt 2026-11-09): copy the BCM4389 ROM bytes the kernel read at boot (patch 0277 mode 2, sysctl debug.sweetbtrom) to
 * /private/var/dev/btrom.bin (devd PULL allowlist), so the host pulls them losslessly with `tools/hot pull`. Prints the length and an
 * FNV-1a hash to compare with the kernel's `BT ROM: buffer done` row. The bytes stay in gitignored artifacts (rule 4). Run as root via devd. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	const char *dst = argc > 1 ? argv[1] : "/private/var/dev/btrom.bin";
	size_t len = 0;
	uint8_t *b;
	uint32_t fnv = 2166136261u;
	int fd;

	setvbuf(stdout, 0, _IONBF, 0);
	if (sysctlbyname("debug.sweetbtrom", NULL, &len, NULL, 0) != 0 || len == 0) {
		printf("BT ROMGET GAP: sysctl debug.sweetbtrom unavailable (len %zu)\n", len);
		return 1;
	}
	b = malloc(len);
	if (!b || sysctlbyname("debug.sweetbtrom", b, &len, NULL, 0) != 0) {
		printf("BT ROMGET GAP: read of %zu bytes failed\n", len);
		return 1;
	}
	for (size_t i = 0; i < len; i++)
		fnv = (fnv ^ b[i]) * 16777619u;
	fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0 || write(fd, b, len) != (ssize_t)len) {
		printf("BT ROMGET GAP: write %s failed\n", dst);
		return 1;
	}
	close(fd);
	printf("BT ROMGET: RESULT: PASS bytes=%zu fnv1a=0x%08x file=%s\n", len, fnv, dst);
	return 0;
}
