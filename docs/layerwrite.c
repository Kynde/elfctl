/* layerwrite — recoverable single-index write harness for layer experiments.
 *
 * Writes one arbitrary key index (NOT capped at 1..4 like elfctl) so we can
 * test the "modifying a layer-2 key enables that layer" hypothesis. Uses ONLY
 * the known-safe set-key (0x81) / read-key (0x82) opcodes — never flash/model.
 *
 * Usage:
 *   ./layerwrite <idx>            read-only: print mod,key at that index
 *   ./layerwrite <idx> <hexkey>   write keycode (mod=0) at idx, verify read-back
 *
 * Layer map (verified): index = layer*16 + key.  layer2 key1 = idx 17.
 * Factory default at idx 17 is 0x0a ('g') — restore with `./layerwrite 17 0a`.
 *
 * It always prints idx 1 (layer-1 key1) before & after so we can confirm a
 * high-index write does NOT bleed into layer 1.
 *
 * Build: cc -O2 -Wall -Wextra -o layerwrite layerwrite.c
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
#define OP_SET_KEY  0x81

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
static int read_idx(int fd, int idx, int *mod) {
    drain(fd);
    uint8_t cmd[8] = {1, OP_READ_KEY, 8, (uint8_t)idx, 0,0,0,0};
    if (wr(fd, cmd)) return -1;
    uint8_t r[64];
    for (int t = 0; t < 8; t++) {
        struct pollfd p = {.fd=fd, .events=POLLIN};
        if (poll(&p, 1, 800) <= 0) return -1;
        ssize_t n = read(fd, r, sizeof r);
        if (n < 4) return -1;
        if (r[0] == 0x04) { if (mod) *mod = r[2]; return r[3]; }
    }
    return -1;
}
/* write idx -> (mod=0, key); mirrors elfctl set_key but with arbitrary idx. */
static int write_idx(int fd, int idx, uint8_t key) {
    uint8_t hdr[8]  = {1, OP_SET_KEY, 0x04, (uint8_t)idx, 0,0,0,0};
    uint8_t data[8] = {0x04, 0x01, 0x00, key, 0,0,0,0};
    if (wr(fd, hdr)) return -1;
    if (wr(fd, data)) return -1;
    drain(fd); /* consume set-key ACK */
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <idx> [hexkey]\n", argv[0]); return 2; }
    int idx = atoi(argv[1]);
    char node[256];
    if (find_dev(node, sizeof node)) { fprintf(stderr, "device not found\n"); return 1; }
    int fd = open(node, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    int m1, k1 = read_idx(fd, 1, &m1);
    int mi, ki = read_idx(fd, idx, &mi);
    printf("before: idx1 = mod %02x key %02x   |   idx%d = mod %02x key %02x\n",
           m1, k1, idx, mi, ki);

    if (argc >= 3) {
        uint8_t key = (uint8_t)strtol(argv[2], NULL, 16);
        printf("writing idx%d <- key %02x ...\n", idx, key);
        if (write_idx(fd, idx, key)) { fprintf(stderr, "write failed\n"); close(fd); return 1; }
        int m1b, k1b = read_idx(fd, 1, &m1b);
        int mib, kib = read_idx(fd, idx, &mib);
        printf("after : idx1 = mod %02x key %02x   |   idx%d = mod %02x key %02x\n",
               m1b, k1b, idx, mib, kib);
        printf("  idx%d %s   |   idx1 %s\n",
               idx, (kib == key) ? "took the write" : "DID NOT change",
               (k1b == k1) ? "unchanged (no bleed)" : "ALSO CHANGED (bleed!)");
    }
    close(fd);
    return 0;
}
