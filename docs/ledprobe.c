/* ledprobe — READ-ONLY dump of the MK424BT LED/backlight settings.
 *
 * Emits exactly two opcodes, both pure reads:
 *   0x63  read-led         (app: readLedCmd       = [1, 99, 1, 0, 0, 0, 0, 0])
 *   0xA4  read-border-led  (app: readBorderLedCmd = [1,164, 0, 0, 0, 0, 0, 0])
 *
 * It NEVER emits the writes (0x62 set-led / 0xA3 set-border-led), and never
 * touches the neighbouring 0x60 set-model opcode. See the safety rules in
 * AGENTS.md.
 *
 * Provenance: the opcodes and the response layout come from the official
 * PCsensor ElfKey configurator's own source — the 3.0.0 macOS build ships
 * out/main/index.js as plain JS (3.3.5 compiles it to V8 bytecode). MK424 is
 * explicitly listed in that app's hasLed() predicate, so this is the LED family
 * our device actually uses. Full writeup in docs/PROTOCOL.md ("LEDs").
 *
 * Response framing — VERIFIED on hardware (MK424BT, firmware V1.1). The device
 * replies with exactly the same two-report record that setLed writes:
 *   report 0: [ 0x0D  effect  colorMode  r  g  b  0x01  0xE8 ]
 *   report 1: [ 0x03  0xE8  0x03  mode  flashes  0  0  0 ]
 * i.e. concatenated, a length-prefixed 13-byte record:
 *   0D <effect> <colorMode> <r> <g> <b> 01 E8 03 E8 03 <mode> <flashes>
 *
 * NOTE the off-by-one against the app's source: readLed() takes effect from
 * data1[2], colorMode from data1[3], rgb from data1[4..6], mode from data2[4]
 * and flashes from data2[5] — all one higher than the offsets above. node-hid
 * hands the app a leading report-ID byte that the kernel strips from hidraw
 * reads, so app index N == our index N-1. Translate every app read offset by -1.
 *
 * Enum values (from the configurator's UI, English strings quoted verbatim):
 *   effect     1 "Turn off the light"
 *              2 "Corresponding key pressed blinks"
 *              3 "Breathing light mode"
 *              5 "Flashes"
 *   flashes    1 = "Multicolor (high power consumption)", 0 = single colour
 *   borderLed  0 off / 1 always on / 2 breathing / 3 flash on keypress
 *            130 "Colorful breath"
 *   colorMode / mode are round-tripped by the app but never edited in its UI;
 *   their semantics are UNKNOWN. Defaults are colorMode=3, mode=1.
 *
 * Build: cc -O2 -Wall -Wextra -o ledprobe ledprobe.c   (or `make ledprobe`)
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
    struct pollfd p = {.fd = fd, .events = POLLIN};
    uint8_t junk[64];
    while (poll(&p, 1, 30) > 0 && read(fd, junk, sizeof junk) > 0) ;
}

/* Collect every report the device sends in reply, printing each raw. */
static int read_reply(int fd, uint8_t *buf, size_t cap) {
    size_t total = 0;
    int first = 1, rep = 0;
    for (;;) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        if (poll(&p, 1, first ? 600 : 150) <= 0) break;
        first = 0;
        uint8_t r[64];
        ssize_t n = read(fd, r, sizeof r);
        if (n <= 0) break;
        printf("    report %d:", rep++);
        for (ssize_t i = 0; i < n; i++) printf(" %02x", r[i]);
        printf("\n");
        for (ssize_t i = 0; i < n && total < cap; i++) buf[total++] = r[i];
    }
    return (int)total;
}

static const char *effect_name(uint8_t e) {
    switch (e) {
        case 1: return "off";
        case 2: return "blink on corresponding keypress";
        case 3: return "breathing";
        case 5: return "flashes";
        default: return "UNKNOWN";
    }
}
static const char *border_name(uint8_t b) {
    switch (b) {
        case 0:   return "off";
        case 1:   return "always on";
        case 2:   return "breathing";
        case 3:   return "flash on keypress";
        case 130: return "colorful breath";
        default:  return "UNKNOWN";
    }
}

int main(void) {
    char node[256];
    if (find_dev(node, sizeof node)) { fprintf(stderr, "ledprobe: device not found\n"); return 1; }
    int fd = open(node, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    printf("ledprobe: %s — reading LED settings via 0x63 / 0xA4 (read-only)\n\n", node);

    /* --- key LED: 01 63 01 ... ------------------------------------------ */
    printf("== read-led (0x63, byte[2]=1) ==\n");
    drain(fd);
    uint8_t led[8] = {1, 0x63, 1, 0, 0, 0, 0, 0};
    if (wr(fd, led)) { fprintf(stderr, "ledprobe: write failed\n"); close(fd); return 1; }

    uint8_t b[256];
    int n = read_reply(fd, b, sizeof b);
    if (n < 13) {
        printf("    (need the full 13-byte record; got %d bytes)\n", n);
    } else if (b[0] != 0x0D) {
        printf("    (unexpected record length 0x%02x, want 0x0d — not decoding)\n", b[0]);
    } else {
        /* Offsets are the record's own, i.e. the app's readLed() indices
         * minus one (see the note at the top of this file). */
        uint8_t effect = b[1], colorMode = b[2];
        uint8_t r = b[3], g = b[4], bl = b[5];
        uint8_t mode = b[11], flashes = b[12];
        unsigned t1 = (unsigned)b[7] | ((unsigned)b[8] << 8);
        unsigned t2 = (unsigned)b[9] | ((unsigned)b[10] << 8);
        printf("\n    effect    = %u  (%s)\n", effect, effect_name(effect));
        printf("    colorMode = %u  (semantics unknown; app default 3)\n", colorMode);
        printf("    color     = #%02x%02x%02x\n", r, g, bl);
        printf("    mode      = %u  (semantics unknown; app default 1)\n", mode);
        printf("    flashes   = %u  (%s)\n", flashes,
               flashes == 1 ? "multicolor" : "single colour");
        printf("    unknown   = byte[6]=%u  timings=%u,%u (16-bit LE; app hard-codes 1000)\n",
               b[6], t1, t2);
    }

    /* --- border LED: 01 A4 ... ------------------------------------------ */
    printf("\n== read-border-led (0xA4) ==\n");
    drain(fd);
    uint8_t bled[8] = {1, 0xA4, 0, 0, 0, 0, 0, 0};
    if (wr(fd, bled)) { fprintf(stderr, "ledprobe: write failed\n"); close(fd); return 1; }

    uint8_t c[256];
    int m = read_reply(fd, c, sizeof c);
    if (m < 3) {
        printf("    (no usable reply; got %d bytes)\n", m);
    } else {
        /* Reply observed as: a4 82 03 00 00 00 00 00 — byte[0] echoes the
         * opcode (like the 0xD3 family). By the -1 rule the app's
         * borderLedData[2] is our byte[1]; but byte[2] also holds a valid
         * enum value, so print both and stay honest about which is which. */
        printf("\n    byte[1] = %3u  (%s)   <- app's borderLed by the -1 rule\n",
               c[1], border_name(c[1]));
        printf("    byte[2] = %3u  (%s)   <- also a valid enum value; AMBIGUOUS\n",
               c[2], border_name(c[2]));
    }

    printf("\nNote: the 0x63 record is verified; the 0xA4 border-led reply is not\n"
           "disambiguated yet. Confirm against docs/PROTOCOL.md before acting on it.\n");
    close(fd);
    return 0;
}
