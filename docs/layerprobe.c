/* layerprobe — READ-ONLY exploration of how the MK424BT addresses layers.
 *
 * The MK424BT has a physical "S button" that switches between configurable
 * LAYERS held in device firmware (the button emits no HID report; it toggles
 * layers internally). elfctl currently only ever touches key indices 1..4 —
 * i.e. presumably "layer 1". This probe tries to discover where the other
 * layers live in the protocol's address space.
 *
 * SAFETY: this program ONLY ever emits the read-key opcode (0x82) and the
 * read-model opcode (0x83). It NEVER writes configuration (no 0x81/0x20/0x60).
 * Reads cannot change device state, so this is safe to run repeatedly.
 *
 * It probes three hypotheses for "where is layer 2":
 *   H1  extended key index:  read-key with keyindex 5..16
 *   H2  layer in byte[4]:     read-key {1,0x82,8,key, LAYER, 0,0,0} for LAYER 0..3
 *   H3  layer in byte[2] high nibble or a distinct field — dumped as a sweep
 *
 * Build: cc -O2 -Wall -Wextra -o layerprobe layerprobe.c
 * Run:   ./layerprobe            (needs the 60-elfctl.rules udev rule, or root)
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
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
static void drain(int fd) {  /* clear stale reports before a fresh read */
    uint8_t b[64]; struct pollfd p = {.fd=fd, .events=POLLIN};
    while (poll(&p, 1, 30) > 0 && read(fd, b, sizeof b) > 0) ;
}
/* Issue one read-key command and print the framed response. */
static void probe(int fd, const char *label, const uint8_t cmd[8]) {
    drain(fd);
    wr(fd, cmd);
    uint8_t r[64];
    struct pollfd p = {.fd=fd, .events=POLLIN};
    printf("  %-22s -> ", label);
    if (poll(&p, 1, 800) <= 0) { printf("<no response>\n"); return; }
    ssize_t n = read(fd, r, sizeof r);
    if (n <= 0) { printf("<read err>\n"); return; }
    for (ssize_t i = 0; i < n; i++) printf("%02x ", r[i]);
    /* annotate a valid key frame [len=04, count, mod, key] */
    if (n >= 4 && r[0] == 0x04) printf("  [valid frame: count=%d mod=%02x key=%02x]", r[1], r[2], r[3]);
    printf("\n");
}

int main(void) {
    char node[256];
    if (find_dev(node, sizeof node)) { fprintf(stderr, "device not found\n"); return 1; }
    int fd = open(node, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    printf("config dev: %s   (READ-ONLY probe — nothing is written)\n", node);

    printf("\n== baseline: keys 1..4 (current/layer-1) ==\n");
    for (int k = 1; k <= 4; k++) {
        char l[32]; snprintf(l, sizeof l, "read-key idx=%d", k);
        uint8_t cmd[8] = {1, OP_READ_KEY, 8, (uint8_t)k, 0,0,0,0};
        probe(fd, l, cmd);
    }

    printf("\n== H1: extended key index 5..16 (layers as more slots?) ==\n");
    for (int k = 5; k <= 16; k++) {
        char l[32]; snprintf(l, sizeof l, "read-key idx=%d", k);
        uint8_t cmd[8] = {1, OP_READ_KEY, 8, (uint8_t)k, 0,0,0,0};
        probe(fd, l, cmd);
    }

    printf("\n== H2: layer selector in byte[4], keys 1..4 x layer 0..3 ==\n");
    for (int layer = 0; layer <= 3; layer++)
        for (int k = 1; k <= 4; k++) {
            char l[32]; snprintf(l, sizeof l, "key=%d layer(b4)=%d", k, layer);
            uint8_t cmd[8] = {1, OP_READ_KEY, 8, (uint8_t)k, (uint8_t)layer, 0,0,0};
            probe(fd, l, cmd);
        }

    printf("\n== H3: layer selector in byte[5], key 1 x layer 0..3 ==\n");
    for (int layer = 0; layer <= 3; layer++) {
        char l[32]; snprintf(l, sizeof l, "key=1 layer(b5)=%d", layer);
        uint8_t cmd[8] = {1, OP_READ_KEY, 8, 1, 0, (uint8_t)layer, 0,0};
        probe(fd, l, cmd);
    }

    close(fd);
    return 0;
}
