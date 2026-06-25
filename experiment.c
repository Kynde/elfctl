/* Experiment harness: observe exactly what the device emits around a key write.
 * Read-only-ish: it DOES write key1, but only to a recoverable HID usage, and
 * key1's original value (a=0x04) is known so we can always restore it.
 *
 * Build: cc -O2 -Wall -Wextra -o experiment experiment.c
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
static void wr(int fd, const uint8_t c[8], const char *label) {
    uint8_t buf[9]; buf[0] = 0; memcpy(buf + 1, c, 8);
    usleep(20000);
    ssize_t w = write(fd, buf, 9);
    printf("  WRITE %-14s [%02x %02x %02x %02x %02x %02x %02x %02x] -> %zd%s\n",
           label, c[0],c[1],c[2],c[3],c[4],c[5],c[6],c[7], w,
           w==9?"":" ERR");
}
/* Drain and print every report available within timeout_ms (per report). */
static void drain(int fd, const char *label, int timeout_ms) {
    uint8_t b[64]; int got = 0;
    for (;;) {
        struct pollfd p = {.fd=fd, .events=POLLIN};
        if (poll(&p, 1, timeout_ms) <= 0) break;
        ssize_t n = read(fd, b, sizeof b);
        if (n <= 0) break;
        printf("  READ  %-14s [", label);
        for (ssize_t i = 0; i < n; i++) printf("%02x%s", b[i], i+1<n?" ":"");
        printf("]\n");
        got++;
    }
    if (!got) printf("  READ  %-14s <none>\n", label);
}

int main(int argc, char **argv) {
    char node[256];
    if (find_dev(node, sizeof node)) { fprintf(stderr, "not found\n"); return 1; }
    int fd = open(node, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    printf("dev: %s\n", node);

    /* baseline read of key1 */
    uint8_t rk[8] = {1, 0x82, 8, 1, 0,0,0,0};
    printf("\n[baseline read key1]\n");
    wr(fd, rk, "read-key1");
    drain(fd, "resp", 800);

    /* Candidate write. argv[1] selects the data-report format to try.
     * target keycode = F13 = 0x68, modifier = 0. */
    int variant = argc > 1 ? atoi(argv[1]) : 1;
    uint8_t key = 0x68, mod = 0x00;

    printf("\n[write key1 -> f13, variant %d]\n", variant);
    if (variant == 1) {
        /* symmetric-with-read: header[len=4], data=[4,1,mod,key] */
        uint8_t h[8] = {1, 0x81, 0x04, 1, 0,0,0,0};
        uint8_t dat[8] = {0x04, 0x01, mod, key, 0,0,0,0};
        wr(fd, h, "set-hdr"); drain(fd, "hdr-resp", 300);
        wr(fd, dat, "set-data"); drain(fd, "data-resp", 300);
    } else if (variant == 2) {
        /* reference-style: header[len=3], data=[3,4,key] (the JS buildBytes shape) */
        uint8_t h[8] = {1, 0x81, 0x03, 1, 0,0,0,0};
        uint8_t dat[8] = {0x03, 0x04, key, 0,0,0,0,0};
        wr(fd, h, "set-hdr"); drain(fd, "hdr-resp", 300);
        wr(fd, dat, "set-data"); drain(fd, "data-resp", 300);
    } else if (variant == 3) {
        /* single combined report, no separate header */
        uint8_t dat[8] = {1, 0x81, 0x04, 1, 0x01, mod, key, 0};
        wr(fd, dat, "set-1shot"); drain(fd, "resp", 300);
    }

    /* fresh read of key1 */
    printf("\n[verify read key1]\n");
    wr(fd, rk, "read-key1");
    drain(fd, "resp", 800);

    /* restore key1 -> a (0x04), using whichever variant we tried */
    printf("\n[restore key1 -> a]\n");
    if (variant == 2) {
        uint8_t h[8] = {1, 0x81, 0x03, 1, 0,0,0,0};
        uint8_t dat[8] = {0x03, 0x04, 0x04, 0,0,0,0,0};
        wr(fd, h, "set-hdr"); drain(fd, "hdr-resp", 300);
        wr(fd, dat, "set-data"); drain(fd, "data-resp", 300);
    } else if (variant == 3) {
        uint8_t dat[8] = {1, 0x81, 0x04, 1, 0x01, 0x00, 0x04, 0};
        wr(fd, dat, "set-1shot"); drain(fd, "resp", 300);
    } else {
        uint8_t h[8] = {1, 0x81, 0x04, 1, 0,0,0,0};
        uint8_t dat[8] = {0x04, 0x01, 0x00, 0x04, 0,0,0,0};
        wr(fd, h, "set-hdr"); drain(fd, "hdr-resp", 300);
        wr(fd, dat, "set-data"); drain(fd, "data-resp", 300);
    }
    printf("\n[final read key1]\n");
    wr(fd, rk, "read-key1");
    drain(fd, "resp", 800);

    close(fd);
    return 0;
}
