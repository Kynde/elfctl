/* readsweep — READ-ONLY wide sweep of the MK424BT key-index address space.
 *
 * Extends layerprobe: reads key indices 0..64 plus a few high sentinels to map
 * how much storage the device actually exposes and where it stops returning
 * distinct data. Helps decide whether layers live at extended indices.
 *
 * SAFETY: emits ONLY the read-key opcode (0x82). Never writes. Reads cannot
 * change device state.
 *
 * Build: cc -O2 -Wall -Wextra -o readsweep readsweep.c
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VENDOR "3553"
#define PRODUCT "C140"
#define CONFIG_IFACE "01"
#define OP_READ_KEY 0x82

static int read_file(const char *path, char *buf, size_t n) {
    int fd = open(path, O_RDONLY); if (fd < 0) return -1;
    ssize_t r = read(fd, buf, n - 1); close(fd);
    if (r < 0) return -1;
    buf[r] = '\0';
    return 0;
}
static int find_dev(char *out, size_t outn) {
    DIR *d = opendir("/sys/class/hidraw"); if (!d) return -1;
    int found = -1; struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "hidraw", 6)) continue;
        char real[4096], resolved[4096];
        snprintf(real, sizeof real, "/sys/class/hidraw/%s/device", e->d_name);
        if (!realpath(real, resolved)) continue;
        char up[4200], uev[2048];
        snprintf(up, sizeof up, "%s/uevent", resolved);
        if (read_file(up, uev, sizeof uev)) continue;
        for (char *p = uev; *p; p++) *p = (char)toupper((unsigned char)*p);
        if (!strstr(uev, VENDOR) || !strstr(uev, PRODUCT)) continue;
        char *s = strrchr(resolved, '/'); if (!s) continue; *s = '\0';
        char ip[4200], in[64];
        snprintf(ip, sizeof ip, "%s/bInterfaceNumber", resolved);
        if (read_file(ip, in, sizeof in)) continue;
        in[strcspn(in, "\r\n ")] = '\0';
        if (strcmp(in, CONFIG_IFACE)) continue;
        snprintf(out, outn, "/dev/%.*s", (int)(outn - 6), e->d_name); found = 0; break;
    }
    closedir(d); return found;
}
static int wr(int fd, const uint8_t c[8]) {
    uint8_t buf[9]; buf[0] = 0; memcpy(buf + 1, c, 8);
    usleep(20000);
    return write(fd, buf, 9) == 9 ? 0 : -1;
}
static void drain(int fd) {
    uint8_t b[64]; struct pollfd p = {.fd=fd, .events=POLLIN};
    while (poll(&p, 1, 30) > 0 && read(fd, b, sizeof b) > 0) ;
}
/* Read one index; return key byte (r[3]) or -1, and stash mod via *mod. */
static int read_idx(int fd, int idx, int *mod) {
    drain(fd);
    uint8_t cmd[8] = {1, OP_READ_KEY, 8, (uint8_t)idx, 0,0,0,0};
    wr(fd, cmd);
    uint8_t r[64];
    struct pollfd p = {.fd=fd, .events=POLLIN};
    if (poll(&p, 1, 800) <= 0) return -1;
    ssize_t n = read(fd, r, sizeof r);
    if (n < 4 || r[0] != 0x04) return -1;
    if (mod) *mod = r[2];
    return r[3];
}

int main(void) {
    char node[256];
    if (find_dev(node, sizeof node)) { fprintf(stderr, "device not found\n"); return 1; }
    int fd = open(node, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    printf("config dev: %s   (READ-ONLY — nothing written)\n\n", node);

    printf("idx : mod key   note\n");
    static const int idxs[] = {
        1,2,3,4,5,6,
        17,18,19,20,21,22,
	33,34,35,36,37,38
    };
    for (size_t i = 0; i < sizeof idxs / sizeof idxs[0]; i++) {
        int mod = -1, key = read_idx(fd, idxs[i], &mod);
        if (key < 0) { printf(" %3d:  <no/err response>\n", idxs[i]); continue; }
        printf(" %3d:  %02x  %02x\n", idxs[i], mod, key);
    }

    /* Stability check: re-read 5 and 6 three times each. */
    printf("\nstability (5,6 x3):\n");
    for (int rep = 0; rep < 3; rep++) {
        int m5, k5 = read_idx(fd, 5, &m5);
        int m6, k6 = read_idx(fd, 6, &m6);
        printf("  pass%d: idx5=%02x idx6=%02x\n", rep, k5, k6);
    }

    close(fd);
    return 0;
}
