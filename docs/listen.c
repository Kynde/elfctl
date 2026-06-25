/* listen — PASSIVE listener on both MK424BT HID interfaces.
 *
 * Opens BOTH hidraw nodes for the device (interface 0 = keyboard/consumer/mouse,
 * interface 1 = vendor 0xff00 config, which has an IN endpoint ep_84) and prints
 * every report that arrives, timestamped. It NEVER writes — pure read().
 *
 * Purpose: discover what the physical "S button" emits. The user reports S
 * blinks the backlight (a layer switch) but produces no keystroke. A layer-
 * change event may surface as a vendor report on interface 1's IN endpoint and
 * could carry the current layer index.
 *
 * Build: cc -O2 -Wall -Wextra -o listen listen.c
 * Run:   ./listen     then press S, the power button, and the keys.  Ctrl-C to stop.
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
#include <sys/time.h>
#include <unistd.h>

#define VENDOR "3553"
#define PRODUCT "C140"

static int read_file(const char *path, char *buf, size_t n) {
    int fd = open(path, O_RDONLY); if (fd < 0) return -1;
    ssize_t r = read(fd, buf, n - 1); close(fd);
    if (r < 0) return -1;
    buf[r] = '\0';
    return 0;
}

/* Find both hidraw nodes for the device; out[i] gets interface number i's node. */
static void find_nodes(char out[2][256]) {
    out[0][0] = out[1][0] = '\0';
    DIR *d = opendir("/sys/class/hidraw"); if (!d) return;
    struct dirent *e;
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
        int idx = (strcmp(in, "00") == 0) ? 0 : (strcmp(in, "01") == 0) ? 1 : -1;
        if (idx >= 0) snprintf(out[idx], 256, "/dev/%.249s", e->d_name);
    }
    closedir(d);
}

static double now_s(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

int main(void) {
    char nodes[2][256];
    find_nodes(nodes);
    if (!nodes[0][0] && !nodes[1][0]) { fprintf(stderr, "device not found\n"); return 1; }

    struct { int fd; const char *tag; } io[2] = {{-1, "if0(kbd)"}, {-1, "if1(vendor)"}};
    for (int i = 0; i < 2; i++) {
        if (!nodes[i][0]) continue;
        io[i].fd = open(nodes[i], O_RDONLY);
        if (io[i].fd < 0) fprintf(stderr, "warn: cannot open %s (%s) — may need root for if0\n",
                                  nodes[i], io[i].tag);
        else printf("listening on %s = %s\n", io[i].tag, nodes[i]);
    }
    if (io[0].fd < 0 && io[1].fd < 0) { fprintf(stderr, "no interfaces open\n"); return 1; }

    printf("\nPress: the S button, then the power button, then each key.  Ctrl-C to stop.\n");
    printf("(timestamps in seconds since start)\n\n");
    double t0 = now_s();

    for (;;) {
        struct pollfd p[2];
        int np = 0, map[2];
        for (int i = 0; i < 2; i++)
            if (io[i].fd >= 0) { p[np].fd = io[i].fd; p[np].events = POLLIN; map[np] = i; np++; }
        if (poll(p, np, -1) <= 0) continue;
        for (int j = 0; j < np; j++) {
            if (!(p[j].revents & POLLIN)) continue;
            uint8_t b[64];
            ssize_t n = read(io[map[j]].fd, b, sizeof b);
            if (n <= 0) continue;
            printf("[%8.3f] %-11s (%2zd) ", now_s() - t0, io[map[j]].tag, n);
            for (ssize_t i = 0; i < n; i++) printf("%02x ", b[i]);
            printf("\n");
            fflush(stdout);
        }
    }
}
