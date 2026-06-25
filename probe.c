/* Read-only probe for PCsensor ElfKey devices (e.g. MK424BT, 3553:c140).
 *
 * Talks to the device's *config* HID interface (the one carrying an OUT
 * endpoint) over raw /dev/hidraw, with zero external dependencies --
 * no hidapi, no libusb. Just open()/write()/read()/poll() and a sysfs
 * walk to locate the device.
 *
 * This program ONLY issues read commands (read-model, read-key). It
 * never writes configuration. Its job is to validate the
 * reverse-engineered protocol and dump the raw bytes so we can decode
 * the current key mapping before building the write path.
 *
 * Build:  cc -O2 -Wall -Wextra -o probe probe.c
 * Run:    sudo ./probe          (hidraw node is root-only until a udev rule exists)
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

#define VENDOR  "3553"
#define PRODUCT "C140"
#define CONFIG_IFACE "01"   /* interface carrying the OUT endpoint (ep_05) */

#define REPORT_ID 1

/* 8 command bytes (report id is prepended on the wire => 9 bytes total). */
static const uint8_t READ_MODEL_CMD[8] = {1, 131, 8, 0, 0, 0, 0, 0}; /* op 0x83 */
static const uint8_t READ_KEY_CMD[8]   = {1, 130, 8, 0, 0, 0, 0, 0}; /* op 0x82; [3]=key */

/* Read a small sysfs text file into buf (NUL-terminated). Returns 0 on success. */
static int read_file(const char *path, char *buf, size_t n) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r < 0)
        return -1;
    buf[r] = '\0';
    return 0;
}

/* Locate /dev/hidrawN for the ElfKey config interface.
 *
 * Robust against hidraw renumbering: we don't trust a fixed number, we
 * walk /sys/class/hidraw, match vendor:product in the hid uevent, and
 * require bInterfaceNumber == CONFIG_IFACE on the parent USB interface.
 * Writes the device node path into out. Returns 0 on success. */
static int find_config_hidraw(char *out, size_t outn) {
    DIR *d = opendir("/sys/class/hidraw");
    if (!d)
        return -1;

    int found = -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "hidraw", 6) != 0)
            continue;

        char link[512], target[1024];
        snprintf(link, sizeof link, "/sys/class/hidraw/%s/device", e->d_name);
        ssize_t len = readlink(link, target, sizeof target - 1);
        if (len < 0)
            continue;
        target[len] = '\0';
        /* target is relative, e.g. ../../0003:3553:C140.0029 -- resolve it */
        char real[2048];
        snprintf(real, sizeof real, "/sys/class/hidraw/%s/device", e->d_name);
        char resolved[4096];
        if (!realpath(real, resolved))
            continue;

        /* hid uevent carries HID_ID=0003:00003553:0000C140 */
        char uevent_path[4200], uevent[2048];
        snprintf(uevent_path, sizeof uevent_path, "%s/uevent", resolved);
        if (read_file(uevent_path, uevent, sizeof uevent) != 0)
            continue;
        for (char *p = uevent; *p; p++)
            *p = (char)toupper((unsigned char)*p);
        if (!strstr(uevent, VENDOR) || !strstr(uevent, PRODUCT))
            continue;

        /* parent dir of the hid device is the USB interface (.../3-3:1.1) */
        char *slash = strrchr(resolved, '/');
        if (!slash)
            continue;
        *slash = '\0';
        char ifnum_path[4200], ifnum[64];
        snprintf(ifnum_path, sizeof ifnum_path, "%s/bInterfaceNumber", resolved);
        if (read_file(ifnum_path, ifnum, sizeof ifnum) != 0)
            continue;
        ifnum[strcspn(ifnum, "\r\n ")] = '\0';
        if (strcmp(ifnum, CONFIG_IFACE) != 0)
            continue;

        snprintf(out, outn, "/dev/%s", e->d_name);
        found = 0;
        break;
    }
    closedir(d);
    return found;
}

static int write_cmd(int fd, const uint8_t cmd[8]) {
    /* The config interface declares an 8-byte OUTPUT report with NO report
     * ID. For an unnumbered hidraw device the kernel requires buf[0] to be
     * the report-number 0x00 (which it strips and does not transmit); the
     * actual 8-byte report payload follows. The protocol's own leading 0x01
     * is therefore part of cmd[0], not the hidraw report number. */
    uint8_t buf[9];
    buf[0] = 0x00;            /* report number: 0 = unnumbered (stripped) */
    memcpy(buf + 1, cmd, 8);  /* 8-byte report payload, e.g. {1,130,8,...} */
    usleep(20000);            /* 20ms; device is slow to accept back-to-back reports */
    ssize_t w = write(fd, buf, sizeof buf);
    if (w != (ssize_t)sizeof buf) {
        fprintf(stderr, "  write() returned %zd (errno=%d: %s)\n",
                w, errno, strerror(errno));
        return -1;
    }
    return 0;
}

/* Read one input report, waiting up to timeout_ms. Returns #bytes or -1. */
static ssize_t read_report(int fd, uint8_t *buf, size_t n, int timeout_ms) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0)
        return -1;
    return read(fd, buf, n);
}

static void hexdump(const uint8_t *b, ssize_t n) {
    if (n < 0) {
        printf("<none>");
        return;
    }
    for (ssize_t i = 0; i < n; i++)
        printf("%02x ", b[i]);
}

int main(void) {
    char node[256];
    if (find_config_hidraw(node, sizeof node) != 0) {
        fprintf(stderr, "ERROR: ElfKey config interface not found.\n");
        fprintf(stderr, "Is the device plugged in over USB?\n");
        return 1;
    }
    printf("Config interface: %s\n", node);

    int fd = open(node, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "ERROR: cannot open %s (need root or a udev rule).\n", node);
        return 1;
    }

    uint8_t buf[64];

    printf("\n== read-model ==\n");
    if (write_cmd(fd, READ_MODEL_CMD) == 0) {
        for (int i = 0; i < 2; i++) {
            ssize_t n = read_report(fd, buf, sizeof buf, 1000);
            printf("  chunk%d: ", i);
            hexdump(buf, n);
            printf("\n");
        }
    }

    printf("\n== read-key (1..4) ==\n");
    for (int k = 1; k <= 4; k++) {
        uint8_t cmd[8];
        memcpy(cmd, READ_KEY_CMD, 8);
        cmd[3] = (uint8_t)k;
        write_cmd(fd, cmd);
        ssize_t n = read_report(fd, buf, sizeof buf, 1000);
        printf("  key%d: ", k);
        hexdump(buf, n);
        printf("\n");
    }

    close(fd);
    return 0;
}
