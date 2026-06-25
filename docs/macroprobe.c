/* macroprobe — READ-ONLY dump of the MK424BT macro table (opcode 0xC1).
 *
 * Reads all 8 macro slots via readMacroListCmd (0xC1) and prints, per slot:
 *   - the raw 8-byte reports exactly as they arrive, and
 *   - a best-effort decode (name, byte-count, mode, and each action's
 *     mod/key/type/delay) using the framing documented in docs/PROTOCOL.md.
 *
 * The 0xC0-family opcodes are DOCUMENTED — they come from the official PCsensor
 * ElfKey configurator's own source (out/main/index.js: setMacro/readMacroList)
 * and from rgerganov's footswitch driver (same protocol family, 3553:b001).
 * This tool emits ONLY 0xC1, which is a pure read; it never writes a macro
 * (0xC0), deletes one (0xC2), or touches any key binding.
 *
 * Framing (after the kernel strips the report-number byte, so each read is the
 * 8 payload bytes — same as elfctl sees):
 *   report 0..3 : 32 bytes of NAME (UTF-8, NUL-padded)
 *   report 4    : [lenHi lenLo mode a0 a1 a2 a3 a4]   (len = actionBytes + 3)
 *   report 5..  : remaining action bytes, 8 per report
 * Each action is length-prefixed:
 *   keyboard (7): 07 01 <mod> <key> <type> <delayHi> <delayLo>
 *   mouse    (9): 09 02 <btn> <x> <y> <wheel> <type> <delayHi> <delayLo>
 * An empty slot's first report is all 0x00 or all 0xff.
 *
 * The exact `type` (press/release/click) and `mode` (once/repeat/hold/toggle)
 * enum values are decoded from the app but unconfirmed on hardware; this tool
 * exists to confirm them. Decode is best-effort and clearly marked.
 *
 * Build: cc -O2 -Wall -Wextra -o macroprobe macroprobe.c   (or `make macroprobe`)
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
#define NUM_MACROS 8

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

/* Decode one HID usage code to a human key name (same ranges elfctl uses). */
static void keyname(uint8_t code, char *buf, size_t n) {
    if (code >= 0x04 && code <= 0x1d) { snprintf(buf, n, "%c", 'a' + code - 0x04); return; }
    if (code >= 0x1e && code <= 0x26) { snprintf(buf, n, "%c", '1' + code - 0x1e); return; }
    if (code == 0x27) { snprintf(buf, n, "0"); return; }
    if (code >= 0x3a && code <= 0x45) { snprintf(buf, n, "f%d", code - 0x3a + 1); return; }
    if (code >= 0x68 && code <= 0x73) { snprintf(buf, n, "f%d", code - 0x68 + 13); return; }
    switch (code) {
        case 0x00: snprintf(buf, n, "-"); return;   /* no key (modifier-only step) */
        case 0x28: snprintf(buf, n, "enter"); return;
        case 0x29: snprintf(buf, n, "esc"); return;
        case 0x2a: snprintf(buf, n, "backspace"); return;
        case 0x2b: snprintf(buf, n, "tab"); return;
        case 0x2c: snprintf(buf, n, "space"); return;
        default: snprintf(buf, n, "0x%02x", code); return;
    }
}
/* Decode a HID modifier bitmask into "ctrl+shift+..." (elfctl's bit order). */
static void modname(uint8_t m, char *buf, size_t n) {
    static const struct { uint8_t bit; const char *s; } M[] = {
        {0x01,"ctrl"},{0x02,"shift"},{0x04,"alt"},{0x08,"gui"},
        {0x10,"rctrl"},{0x20,"rshift"},{0x40,"ralt"},{0x80,"rgui"},
    };
    buf[0] = '\0'; size_t used = 0;
    for (size_t i = 0; i < sizeof M / sizeof M[0]; i++)
        if (m & M[i].bit) {
            int w = snprintf(buf + used, n - used, "%s%s", used ? "+" : "", M[i].s);
            if (w > 0) used += (size_t)w;
        }
    if (!used) snprintf(buf, n, "-");
}

/* Read all reports the device sends after a 0xC1 query, into `buf`. Returns the
 * total byte count. Stops when a read times out (the device sends its reports
 * back-to-back, then goes quiet). Each report is printed raw as it arrives. */
static int read_slot(int fd, uint8_t *buf, size_t cap) {
    size_t total = 0;
    int first = 1, rep = 0;
    for (;;) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        if (poll(&p, 1, first ? 600 : 150) <= 0) break;
        first = 0;
        uint8_t r[64];
        ssize_t n = read(fd, r, sizeof r);
        if (n <= 0) break;
        printf("    report %2d:", rep++);
        for (ssize_t i = 0; i < n; i++) printf(" %02x", r[i]);
        printf("\n");
        for (ssize_t i = 0; i < n && total < cap; i++) buf[total++] = r[i];
    }
    return (int)total;
}

static void decode_slot(const uint8_t *b, int len) {
    /* Unlike the 0xD layer family, 0xC1 does NOT echo the opcode: the response
     * is the name/action stream directly. An empty slot replies with a single
     * report of all 0xff (or all 0x00) — the app's own empty-slot sentinel. */
    if (len < 8) { printf("    (no data)\n"); return; }
    int all0 = 1, allff = 1;
    for (int i = 0; i < 8; i++) { if (b[i] != 0x00) all0 = 0; if (b[i] != 0xff) allff = 0; }
    if (all0 || allff) { printf("    (empty slot)\n"); return; }
    if (len < 35) { printf("    (truncated: %d bytes, need >=35 for header)\n", len); return; }

    char name[33];
    int k = 0;
    for (int i = 0; i < 32; i++) if (b[i]) name[k++] = (char)b[i];
    name[k] = '\0';
    int byteCount = b[32] * 256 + b[33];
    int mode = b[34];
    printf("    name=\"%s\"  byteCount=%d  mode=%d  (decode best-effort)\n",
           name, byteCount, mode);

    int k2 = 35; /* actions begin here in the concatenated stream */
    int step = 1;
    while (k2 < len) {
        int rl = b[k2];
        if (rl == 0) break;
        if (k2 + rl > len) { printf("    step %d: truncated record (len %d)\n", step, rl); break; }
        const uint8_t *a = b + k2;
        if (a[1] == 1 && rl >= 7) {              /* keyboard action */
            char mn[64], kn[16];
            modname(a[2], mn, sizeof mn);
            keyname(a[3], kn, sizeof kn);
            int delay = a[5] * 256 + a[6];
            printf("    step %d: key   mod=%s key=%s type=%d delay=%dms\n",
                   step, mn, kn, a[4], delay);
        } else if (a[1] == 2 && rl >= 9) {       /* mouse action */
            int x = a[3] < 128 ? a[3] : a[3] - 256;
            int y = a[4] < 128 ? a[4] : a[4] - 256;
            int delay = a[7] * 256 + a[8];
            printf("    step %d: mouse btn=0x%02x x=%d y=%d wheel=%d type=%d delay=%dms\n",
                   step, a[2], x, y, a[5], a[6], delay);
        } else {
            printf("    step %d: unknown record (len=%d kind=%d)\n", step, rl, a[1]);
        }
        k2 += rl;
        step++;
    }
}

int main(void) {
    char node[256];
    if (find_dev(node, sizeof node)) { fprintf(stderr, "macroprobe: device not found\n"); return 1; }
    int fd = open(node, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    printf("macroprobe: %s — reading %d macro slots via 0xC1 (read-only)\n\n",
           node, NUM_MACROS);

    for (int id = 1; id <= NUM_MACROS; id++) {
        printf("== macro %d ==\n", id);
        /* drain any stale reports before each query */
        struct pollfd p = {.fd = fd, .events = POLLIN};
        uint8_t junk[64];
        while (poll(&p, 1, 30) > 0 && read(fd, junk, sizeof junk) > 0) ;

        /* id goes in byte[3] (app: readMacroListCmd[3] = id), not byte[2]. */
        uint8_t cmd[8] = {1, 0xC1, 0, (uint8_t)id, 0, 0, 0, 0};
        if (wr(fd, cmd)) { fprintf(stderr, "macroprobe: write failed\n"); close(fd); return 1; }

        uint8_t buf[1024];
        int n = read_slot(fd, buf, sizeof buf);
        if (n == 0) { printf("    (no response)\n\n"); continue; }
        decode_slot(buf, n);
        printf("\n");
    }
    close(fd);
    return 0;
}
