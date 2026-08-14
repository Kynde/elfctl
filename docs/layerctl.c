/* layerctl — drive the MK424BT layer commands (opcodes 0xD1-0xD4).
 *
 * These opcodes are DOCUMENTED, extracted from the official PCsensor ElfKey
 * configurator's own source (Electron app, out/main/index.js):
 *   0xD1 readFuncLayerCmd        [1,209,0,..]  read ACTIVE layer
 *   0xD2 enableFuncLayerCmd      [1,210,m,..]  set ENABLED-layers bitmask (m)
 *   0xD3 readEnabledFuncLayerCmd [1,211,0,..]  read enabled-layers bitmask
 *   0xD4 switchFuncLayerCmd      [1,212,l,..]  switch ACTIVE layer (= S button),
 *                                              l is 1-based; refused if disabled
 * Bitmask: bit0=layer1(always), bit1=layer2, bit2=layer3.
 *   0x01 = L1 only (factory default), 0x03 = L1+L2, 0x07 = all three.
 *
 * Usage:
 *   ./layerctl                 READ-ONLY: print active layer (0xD1) + enabled mask (0xD3)
 *   ./layerctl enable <mask>   write 0xD2 <mask>   (e.g. enable 7 = all layers)
 *   ./layerctl switch <layer>  write 0xD4 <layer>  (software S-button press)
 *
 * Reads (0xD1/0xD3) cannot change state. Writes (0xD2/0xD4) change device state
 * but are documented and reversible (enable 1 restores factory single-layer).
 *
 * Build: cc -O2 -Wall -Wextra -o layerctl layerctl.c
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
/* Send a read command and return the data byte from its response. The response
 * echoes the opcode in byte[0] and carries BOTH fields, in mirrored order, so
 * the caller passes the offset of the one it wants:
 *   0xD1 -> "d1 <active-1> <mask>"   active at byte[1], mask at byte[2]
 *   0xD3 -> "d3 <mask> <active-1>"   mask at byte[1], active at byte[2]
 * The active layer is 0-BASED on the wire (layer 1 = 0), even though 0xD4 takes
 * a 1-based layer. Verified on firmware V1.1 across all three layers.
 */
static int cmd_read(int fd, uint8_t opcode, int data_off) {
    drain(fd);
    uint8_t cmd[8] = {1, opcode, 0, 0, 0, 0, 0, 0};
    if (wr(fd, cmd)) return -1;
    uint8_t r[64];
    for (int t = 0; t < 8; t++) {
        struct pollfd p = {.fd=fd, .events=POLLIN};
        if (poll(&p, 1, 800) <= 0) return -1;
        ssize_t n = read(fd, r, sizeof r);
        if (n <= data_off) continue;
        if (r[0] == opcode) return r[data_off];
    }
    return -1;
}

int main(int argc, char **argv) {
    char node[256];
    if (find_dev(node, sizeof node)) { fprintf(stderr, "device not found\n"); return 1; }
    int fd = open(node, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    if (argc == 1) {
        int active = cmd_read(fd, 0xD1, 1);
        int mask   = cmd_read(fd, 0xD3, 1);
        if (active < 0) printf("active layer (0xD1) : <no/invalid response>\n");
        else            printf("active layer (0xD1) : %d\n", active + 1);
        if (mask < 0) printf("enabled mask (0xD3) : <no/invalid response>\n");
        else printf("enabled mask (0xD3) : 0x%02x  (L1%s%s)\n", mask,
                    (mask & 2) ? "+L2" : "", (mask & 4) ? "+L3" : "");
        close(fd); return 0;
    }
    if (argc == 3 && strcmp(argv[1], "enable") == 0) {
        uint8_t m = (uint8_t)strtol(argv[2], NULL, 0);
        uint8_t cmd[8] = {1, 0xD2, m, 0, 0, 0, 0, 0};
        printf("writing 0xD2 mask=0x%02x ...\n", m);
        if (wr(fd, cmd)) { fprintf(stderr, "write failed\n"); close(fd); return 1; }
        drain(fd);
        int rb = cmd_read(fd, 0xD3, 1);
        printf("read-back enabled mask: 0x%02x %s\n", rb, rb == m ? "(ok)" : "(MISMATCH)");
        close(fd); return rb == m ? 0 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "switch") == 0) {
        uint8_t l = (uint8_t)strtol(argv[2], NULL, 0);
        uint8_t cmd[8] = {1, 0xD4, l, 0, 0, 0, 0, 0};
        printf("writing 0xD4 layer=%d ...\n", l);
        if (wr(fd, cmd)) { fprintf(stderr, "write failed\n"); close(fd); return 1; }
        drain(fd);
        int active = cmd_read(fd, 0xD1, 1);
        if (active < 0) printf("active layer now: <no/invalid response>\n");
        else printf("active layer now: %d%s\n", active + 1,
                    active + 1 == l ? "" : "  (refused — is that layer enabled?)");
        close(fd); return 0;
    }
    fprintf(stderr, "usage: %s [enable <mask> | switch <layer>]\n", argv[0]);
    close(fd); return 2;
}
