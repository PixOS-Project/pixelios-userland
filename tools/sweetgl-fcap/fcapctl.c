/*
 * fcapctl: root helper for libsweetgl's frame capture (sprint GPU 2026-11-09), run by devd (`tools/hot run /usr/local/bin/fcapctl ...`).
 *
 *   fcapctl prep     create /private/var/dev/fcap (mode 0777: backboardd runs as mobile and writes the capture files there; devd can only
 *                    pull below /private/var/dev/) and make the flag files /private/var/dev/fcap-{on,eager,req} writable for everyone
 *   fcapctl ls       list the capture files: "SIZE NAME"
 *   fcapctl clean    delete every *.fcap file in /private/var/dev/fcap (captures of earlier boots; the root is shared)
 *   fcapctl panel F  read kern.sweetpanel (kernel patch 0276: the panel memory the display scans out, after a CPU cache clean) into file F (0644)
 *   fcapctl panels D write every kernel panel snapshot (kern.sweetpanelcount, slots via kern.sweetpanelsel) to D/snap-NN.raw; prints "snaps N"
 *   fcapctl nav N    write N to kern.sweetnavrun (kernel patch 0275): a new nav session on the running system (N = reps | 0x100 capture | 0x200 tap)
 *
 * Exit 0 = done; 1 = a step failed (the failing step and errno are printed: no silent failure).
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define DIR_PATH "/private/var/dev/fcap"

static int prep(void)
{
    int bad = 0;
    if (mkdir(DIR_PATH, 0777) != 0 && errno != EEXIST) { printf("fcapctl: mkdir %s: %s\n", DIR_PATH, strerror(errno)); bad = 1; }
    if (chmod(DIR_PATH, 0777) != 0) { printf("fcapctl: chmod %s: %s\n", DIR_PATH, strerror(errno)); bad = 1; }
    static const char *flags[] = { "/private/var/dev/fcap-on", "/private/var/dev/fcap-eager", "/private/var/dev/fcap-req" };
    for (unsigned i = 0; i < sizeof flags / sizeof flags[0]; i++) {
        struct stat sb;
        if (stat(flags[i], &sb) == 0 && chmod(flags[i], 0666) != 0) { printf("fcapctl: chmod %s: %s\n", flags[i], strerror(errno)); bad = 1; }
    }
    printf("fcapctl: prep %s\n", bad ? "FAILED" : "ok");
    return bad;
}

static int list(int clean)
{
    DIR *d = opendir(DIR_PATH);
    if (!d) { printf("fcapctl: opendir %s: %s\n", DIR_PATH, strerror(errno)); return 1; }
    struct dirent *e;
    int n = 0, bad = 0;
    while ((e = readdir(d))) {
        /* the recorder writes parts (<name>.fcap.NNN, <name>.ftap.NNN) and the kernel panel snapshots (snap-NN.raw); matching only "*.fcap"
         * left every part on userdata: 2.58 GB / 174 files filled /private/var and the c02 capture of gputest-count5 ended in errno 28 */
        const char *nm = e->d_name;
        size_t l = strlen(nm);
        int ours = (strstr(nm, ".fcap") != NULL) || (strstr(nm, ".ftap") != NULL) || (strncmp(nm, "snap-", 5) == 0 && l > 4 && strcmp(nm + l - 4, ".raw") == 0);
        if (!ours) continue;
        char p[512];
        snprintf(p, sizeof p, "%s/%s", DIR_PATH, e->d_name);
        struct stat sb;
        if (stat(p, &sb) != 0) { printf("fcapctl: stat %s: %s\n", p, strerror(errno)); bad = 1; continue; }
        if (clean) {
            if (unlink(p) != 0) { printf("fcapctl: unlink %s: %s\n", p, strerror(errno)); bad = 1; continue; }
            printf("deleted %lld %s\n", (long long)sb.st_size, e->d_name);
        } else {
            printf("%lld %s\n", (long long)sb.st_size, e->d_name);
        }
        n++;
    }
    closedir(d);
    printf("fcapctl: %s %d file(s)%s\n", clean ? "clean" : "ls", n, bad ? " with errors" : "");
    return bad;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "prep") == 0) return prep();
    if (argc == 2 && strcmp(argv[1], "ls") == 0) return list(0);
    if (argc == 2 && strcmp(argv[1], "clean") == 0) return list(1);
    if (argc == 3 && strcmp(argv[1], "panel") == 0) {
        size_t len = 0;
        if (sysctlbyname("kern.sweetpanel", NULL, &len, NULL, 0) != 0 || len == 0) { printf("fcapctl: kern.sweetpanel size: %s\n", strerror(errno)); return 1; }
        void *buf = malloc(len);
        if (!buf) { printf("fcapctl: no memory for %zu bytes\n", len); return 1; }
        if (sysctlbyname("kern.sweetpanel", buf, &len, NULL, 0) != 0) { printf("fcapctl: kern.sweetpanel read: %s\n", strerror(errno)); free(buf); return 1; }
        FILE *f = fopen(argv[2], "wb");
        if (!f || fwrite(buf, 1, len, f) != len || fclose(f) != 0) { printf("fcapctl: write %s: %s\n", argv[2], strerror(errno)); free(buf); return 1; }
        chmod(argv[2], 0644);
        free(buf);
        printf("fcapctl: panel %zu bytes -> %s\n", len, argv[2]);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "panels") == 0) {
        unsigned n = 0; size_t sz = sizeof n;
        if (sysctlbyname("kern.sweetpanelcount", &n, &sz, NULL, 0) != 0) { printf("fcapctl: kern.sweetpanelcount: %s\n", strerror(errno)); return 1; }
        unsigned slots = n < 32 ? n : 32, ok = 0;
        for (unsigned i = 0; i < slots; i++) {
            int sel = (int)i;
            if (sysctlbyname("kern.sweetpanelsel", NULL, NULL, &sel, sizeof sel) != 0) { printf("fcapctl: sweetpanelsel %u: %s\n", i, strerror(errno)); continue; }
            size_t len = 0;
            if (sysctlbyname("kern.sweetpanel", NULL, &len, NULL, 0) != 0 || !len) { printf("fcapctl: slot %u size: %s\n", i, strerror(errno)); continue; }
            void *buf = malloc(len);
            if (!buf || sysctlbyname("kern.sweetpanel", buf, &len, NULL, 0) != 0) { printf("fcapctl: slot %u read: %s\n", i, strerror(errno)); free(buf); continue; }
            char path[600]; snprintf(path, sizeof path, "%s/snap-%02u.raw", argv[2], i);
            FILE *f = fopen(path, "wb");
            if (f && fwrite(buf, 1, len, f) == len && fclose(f) == 0) { chmod(path, 0644); ok++; } else printf("fcapctl: write %s: %s\n", path, strerror(errno));
            free(buf);
        }
        int live = -1;
        sysctlbyname("kern.sweetpanelsel", NULL, NULL, &live, sizeof live);
        printf("snaps %u of %u\n", ok, slots);
        return ok == slots ? 0 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "nav") == 0) {
        int v = (int)strtol(argv[2], NULL, 0);
        if (sysctlbyname("kern.sweetnavrun", NULL, NULL, &v, sizeof v) != 0) { printf("fcapctl: kern.sweetnavrun = 0x%x: %s\n", v, strerror(errno)); return 1; }
        printf("fcapctl: nav session started (kern.sweetnavrun = 0x%x)\n", v);
        return 0;
    }
    printf("usage: fcapctl prep|ls|clean|nav N|panel FILE|panels DIR\n");
    return 1;
}
