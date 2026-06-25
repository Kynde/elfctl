/* elfctl — configure PCsensor ElfKey devices (MK424BT 4-key macropad, 3553:c140)
 *
 * Talks to the device's config HID interface over raw /dev/hidraw with zero
 * external dependencies (no hidapi, no libusb). Reverse-engineered ElfKey
 * protocol; validated empirically against an MK424BT (firmware V1.1).
 *
 * Commands:
 *   elfctl list                 identify the device (model, firmware)
 *   elfctl get                  read all keys, decode to names
 *   elfctl set <key> <binding>  write one key (e.g. `set 1 f13`, `set 2 ctrl-c`)
 *   elfctl save [file]          dump current config (stdout or file)
 *   elfctl load <file>          apply a config file
 *
 * Bindings are a single key with optional modifier prefixes joined by '-' or
 * '+', e.g.  f13  ctrl-c  shift-tab  gui-l  alt-f4. Names: a-z, 0-9, f1-f24,
 * enter esc backspace tab space and common punctuation. Modifiers: ctrl shift
 * alt gui/super/win, optionally prefixed r for the right-hand variant.
 *
 * Config file format (one key per line; '#' comments, blanks ignored):
 *   key1 = f13
 *   key2 = ctrl-c
 *
 * Build:  cc -O2 -Wall -Wextra -o elfctl elfctl.c
 * Access: needs the 60-elfctl.rules udev rule (or run as root).
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
#define NUM_KEYS 4

/* ---- ElfKey command opcodes (byte[1] of the 8-byte report payload) ----
 * NOTE: only the read/set-key opcodes are used here. The protocol family
 * also contains firmware-flash (0x20) and set-model (0x60) opcodes which
 * this tool deliberately never emits. */
#define OP_READ_MODEL 0x83
#define OP_READ_KEY   0x82
#define OP_SET_KEY    0x81

static int g_debug = 0; /* enabled via ELFCTL_DEBUG env */

/* ----------------------- device discovery & raw I/O ----------------------- */

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

/* Locate /dev/hidrawN for the ElfKey config interface (robust against
 * hidraw renumbering: matches vendor:product + config interface number). */
static int find_config_hidraw(char *out, size_t outn) {
    DIR *d = opendir("/sys/class/hidraw");
    if (!d)
        return -1;

    int found = -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "hidraw", 6) != 0)
            continue;

        char real[4096], resolved[4096];
        snprintf(real, sizeof real, "/sys/class/hidraw/%s/device", e->d_name);
        if (!realpath(real, resolved))
            continue;

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

        /* Bound the name copy to the destination so the compiler can prove no
         * truncation (dirent.d_name is declared char[256]; hidraw names are
         * short in practice, but -Wformat-truncation reasons about the max). */
        snprintf(out, outn, "/dev/%.*s", (int)(outn - 6), e->d_name);
        found = 0;
        break;
    }
    closedir(d);
    return found;
}

static int dev_open(void) {
    char node[256];
    if (find_config_hidraw(node, sizeof node) != 0) {
        fprintf(stderr, "elfctl: ElfKey device not found (plugged in over USB?)\n");
        return -1;
    }
    int fd = open(node, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "elfctl: cannot open %s: %s\n", node, strerror(errno));
        fprintf(stderr, "        (install udev/60-elfctl.rules or run as root)\n");
        return -1;
    }
    return fd;
}

/* Send an 8-byte report payload. The config interface declares an UNNUMBERED
 * 8-byte report, so the hidraw write needs a leading 0x00 report number that
 * the kernel strips; the protocol's own 0x01 is payload byte 0. */
static int write_cmd(int fd, const uint8_t cmd[8]) {
    uint8_t buf[9];
    buf[0] = 0x00;
    memcpy(buf + 1, cmd, 8);
    usleep(20000); /* device is slow to accept back-to-back reports */
    if (g_debug) {
        fprintf(stderr, "  [dbg] write:");
        for (int i = 0; i < 9; i++) fprintf(stderr, " %02x", buf[i]);
        fprintf(stderr, "\n");
    }
    ssize_t w = write(fd, buf, sizeof buf);
    if (w != (ssize_t)sizeof buf) {
        fprintf(stderr, "elfctl: write failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static ssize_t read_report(int fd, uint8_t *buf, size_t n, int timeout_ms) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    if (poll(&pfd, 1, timeout_ms) <= 0)
        return -1;
    ssize_t r = read(fd, buf, n);
    if (g_debug && r > 0) {
        fprintf(stderr, "  [dbg] read :");
        for (ssize_t i = 0; i < r; i++) fprintf(stderr, " %02x", buf[i]);
        fprintf(stderr, "\n");
    }
    return r;
}

/* Discard any reports already queued on the device. The set-key command emits
 * an ACK report (byte[1]=0x55) that must be consumed before a clean read-back,
 * and clearing stale state before any read avoids returning the wrong report. */
static void drain_input(int fd) {
    uint8_t b[64];
    while (read_report(fd, b, sizeof b, 30) > 0)
        ;
}

/* --------------------------- keycode name table --------------------------- */

struct named { const char *name; uint8_t code; };

/* Named (non-alphanumeric, non-F) keys. */
static const struct named KEYS[] = {
    {"enter", 0x28}, {"return", 0x28}, {"esc", 0x29}, {"escape", 0x29},
    {"backspace", 0x2a}, {"bksp", 0x2a}, {"tab", 0x2b}, {"space", 0x2c},
    {"minus", 0x2d}, {"equal", 0x2e}, {"lbracket", 0x2f}, {"rbracket", 0x30},
    {"backslash", 0x31}, {"semicolon", 0x33}, {"quote", 0x34}, {"grave", 0x35},
    {"comma", 0x36}, {"period", 0x37}, {"slash", 0x38}, {"capslock", 0x39},
    {"printscreen", 0x46}, {"scrolllock", 0x47}, {"pause", 0x48},
    {"insert", 0x49}, {"home", 0x4a}, {"pageup", 0x4b}, {"delete", 0x4c},
    {"del", 0x4c}, {"end", 0x4d}, {"pagedown", 0x4e},
    {"right", 0x4f}, {"left", 0x50}, {"down", 0x51}, {"up", 0x52},
};

/* Modifier prefixes -> HID modifier bitmask. */
static const struct named MODS[] = {
    {"ctrl", 0x01}, {"control", 0x01}, {"lctrl", 0x01},
    {"shift", 0x02}, {"lshift", 0x02},
    {"alt", 0x04}, {"lalt", 0x04}, {"opt", 0x04},
    {"gui", 0x08}, {"super", 0x08}, {"win", 0x08}, {"meta", 0x08}, {"lgui", 0x08},
    {"rctrl", 0x10}, {"rshift", 0x20}, {"ralt", 0x40}, {"altgr", 0x40},
    {"rgui", 0x80},
};

/* Resolve a bare key token (no modifiers) to a HID usage code. -1 if unknown. */
static int keyname_to_code(const char *s) {
    size_t len = strlen(s);
    if (len == 1) {
        char c = (char)tolower((unsigned char)s[0]);
        if (c >= 'a' && c <= 'z')
            return 0x04 + (c - 'a');
        if (c >= '1' && c <= '9')
            return 0x1e + (c - '1');
        if (c == '0')
            return 0x27;
    }
    if ((s[0] == 'f' || s[0] == 'F') && isdigit((unsigned char)s[1])) {
        int n = atoi(s + 1);
        if (n >= 1 && n <= 12)
            return 0x3a + (n - 1);
        if (n >= 13 && n <= 24)
            return 0x68 + (n - 13);
    }
    for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++)
        if (strcasecmp(s, KEYS[i].name) == 0)
            return KEYS[i].code;
    return -1;
}

/* Reverse: HID usage code -> human name into buf. */
static void code_to_keyname(uint8_t code, char *buf, size_t n) {
    if (code >= 0x04 && code <= 0x1d) { snprintf(buf, n, "%c", 'a' + code - 0x04); return; }
    if (code >= 0x1e && code <= 0x26) { snprintf(buf, n, "%c", '1' + code - 0x1e); return; }
    if (code == 0x27) { snprintf(buf, n, "0"); return; }
    if (code >= 0x3a && code <= 0x45) { snprintf(buf, n, "f%d", code - 0x3a + 1); return; }
    if (code >= 0x68 && code <= 0x73) { snprintf(buf, n, "f%d", code - 0x68 + 13); return; }
    for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++)
        if (KEYS[i].code == code) { snprintf(buf, n, "%s", KEYS[i].name); return; }
    snprintf(buf, n, "0x%02x", code);
}

/* Parse a binding like "ctrl-shift-c" into modifier mask + key code.
 * Returns 0 on success. Mutates a local copy of the string. */
static int parse_binding(const char *binding, uint8_t *mod_out, uint8_t *key_out) {
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s", binding);

    uint8_t mod = 0;
    int key = -1;

    /* Split on '-' or '+'. The final token is the key; earlier ones modifiers.
     * A lone '-'/'+' (the minus/plus key) is handled by the single-char path. */
    char *tokens[16];
    int ntok = 0;
    if (strcmp(tmp, "-") == 0 || strcmp(tmp, "+") == 0) {
        tokens[ntok++] = tmp;
    } else {
        char *p = tmp, *start = tmp;
        for (; *p; p++) {
            if (*p == '-' || *p == '+') {
                *p = '\0';
                if (ntok < 16) tokens[ntok++] = start;
                start = p + 1;
            }
        }
        if (ntok < 16) tokens[ntok++] = start;
    }
    if (ntok == 0)
        return -1;

    for (int i = 0; i < ntok - 1; i++) {
        uint8_t m = 0;
        for (size_t j = 0; j < sizeof MODS / sizeof MODS[0]; j++)
            if (strcasecmp(tokens[i], MODS[j].name) == 0) { m = MODS[j].code; break; }
        if (!m) {
            fprintf(stderr, "elfctl: unknown modifier '%s'\n", tokens[i]);
            return -1;
        }
        mod |= m;
    }

    key = keyname_to_code(tokens[ntok - 1]);
    if (key < 0) {
        fprintf(stderr, "elfctl: unknown key '%s'\n", tokens[ntok - 1]);
        return -1;
    }
    *mod_out = mod;
    *key_out = (uint8_t)key;
    return 0;
}

/* Render mod+key to a human binding string into buf. */
static void format_binding(uint8_t mod, uint8_t key, char *buf, size_t n) {
    /* Pick a canonical name per modifier bit (first match in MODS table). */
    static const struct { uint8_t bit; const char *name; } M[] = {
        {0x01, "ctrl"}, {0x02, "shift"}, {0x04, "alt"}, {0x08, "gui"},
        {0x10, "rctrl"}, {0x20, "rshift"}, {0x40, "ralt"}, {0x80, "rgui"},
    };
    buf[0] = '\0';
    size_t used = 0;
    for (size_t i = 0; i < sizeof M / sizeof M[0]; i++) {
        if (mod & M[i].bit) {
            int w = snprintf(buf + used, n - used, "%s-", M[i].name);
            if (w > 0) used += (size_t)w;
        }
    }
    char kn[32];
    code_to_keyname(key, kn, sizeof kn);
    snprintf(buf + used, n - used, "%s", kn);
}

/* ------------------------------- operations ------------------------------- */

/* Read raw model/firmware strings. Returns 0 on success. */
static int do_read_model(int fd, char *model, size_t mn, char *fw, size_t fn) {
    uint8_t cmd[8] = {1, OP_READ_MODEL, 8, 0, 0, 0, 0, 0};
    if (write_cmd(fd, cmd) != 0)
        return -1;
    uint8_t a[64], b[64];
    ssize_t na = read_report(fd, a, sizeof a, 1000);
    ssize_t nb = read_report(fd, b, sizeof b, 1000);
    if (na < 0 || nb < 0)
        return -1;
    /* chunk0 = "MK424BT_", chunk1 = "V1.1\0..."; trim the trailing model sep. */
    char raw[32] = {0};
    size_t k = 0;
    for (ssize_t i = 0; i < na && k < sizeof raw - 1; i++)
        if (a[i]) raw[k++] = (char)a[i];
    if (k && raw[k - 1] == '_') raw[--k] = '\0';
    snprintf(model, mn, "%s", raw);
    char fwbuf[32] = {0};
    k = 0;
    for (ssize_t i = 0; i < nb && k < sizeof fwbuf - 1; i++)
        if (b[i]) fwbuf[k++] = (char)b[i];
    snprintf(fw, fn, "%s", fwbuf);
    return 0;
}

/* Read one key slot's (mod,key). Returns 0 on success. */
static int read_key(int fd, int keynum, uint8_t *mod, uint8_t *key) {
    uint8_t cmd[8] = {1, OP_READ_KEY, 8, (uint8_t)keynum, 0, 0, 0, 0};
    drain_input(fd); /* clear any stale/ACK reports so we read THIS response */
    if (write_cmd(fd, cmd) != 0)
        return -1;
    /* A real key-read response is framed [len, count, mod, key, ...] with
     * len=0x04. The device may also emit ACK reports (byte[0]=0x81) from a
     * preceding set; skip those and accept only a genuine read response. */
    uint8_t r[64];
    for (int tries = 0; tries < 8; tries++) {
        ssize_t n = read_report(fd, r, sizeof r, 1000);
        if (n < 4)
            return -1;
        if (r[0] == 0x04) {
            *mod = r[2];
            *key = r[3];
            return 0;
        }
        /* else: ACK or unrelated report — keep reading */
    }
    return -1;
}

/* Write one key slot to a single (mod,key) binding, then read back to verify.
 * Returns 0 on verified success. */
static int set_key(int fd, int keynum, uint8_t mod, uint8_t key) {
    /* header: set-key opcode, byte[2]=payload length (4), byte[3]=key index */
    uint8_t hdr[8] = {1, OP_SET_KEY, 0x04, (uint8_t)keynum, 0, 0, 0, 0};
    /* data report mirrors the read layout: [len=4, count=1, mod, key, ...] */
    uint8_t data[8] = {0x04, 0x01, mod, key, 0, 0, 0, 0};
    if (write_cmd(fd, hdr) != 0)
        return -1;
    if (write_cmd(fd, data) != 0)
        return -1;
    drain_input(fd); /* consume the device's set-key ACK report (byte[1]=0x55) */

    uint8_t rmod, rkey;
    if (read_key(fd, keynum, &rmod, &rkey) != 0) {
        fprintf(stderr, "elfctl: wrote key%d but could not read back to verify\n", keynum);
        return -1;
    }
    if (rmod != mod || rkey != key) {
        fprintf(stderr, "elfctl: verify mismatch on key%d: wrote mod=%02x key=%02x, "
                        "read mod=%02x key=%02x\n", keynum, mod, key, rmod, rkey);
        return -1;
    }
    return 0;
}

/* Print every key/modifier name the binding parser accepts. Generated from the
 * same KEYS[]/MODS[] tables and algorithmic ranges used by keyname_to_code(),
 * so it can never drift out of sync with what `set` actually understands. */
static int cmd_keys(void) {
    printf("Modifiers (prefix with '-' or '+', e.g. ctrl-c):\n ");
    for (size_t i = 0; i < sizeof MODS / sizeof MODS[0]; i++)
        printf(" %s", MODS[i].name);
    printf("\n\n");

    printf("Letters:    a b c ... z\n");
    printf("Digits:     0 1 2 ... 9\n");
    printf("Function:   f1 f2 ... f24\n\n");

    printf("Named keys:\n ");
    /* KEYS[] holds aliases too (enter/return, esc/escape, ...); list every
     * accepted spelling so nothing the parser takes is hidden. */
    int col = 0;
    for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++) {
        printf(" %-12s", KEYS[i].name);
        if (++col % 5 == 0) printf("\n ");
    }
    printf("\n\nExamples: f13   ctrl-c   shift-tab   gui-l   ctrl-shift-esc\n");
    return 0;
}

static int cmd_list(void) {
    int fd = dev_open();
    if (fd < 0) return 1;
    char model[32], fw[32];
    int rc = do_read_model(fd, model, sizeof model, fw, sizeof fw);
    close(fd);
    if (rc != 0) { fprintf(stderr, "elfctl: read failed\n"); return 1; }
    printf("%-10s %s:%s  (%d keys)  firmware %s\n",
           model, "3553", "c140", NUM_KEYS, fw);
    return 0;
}

static int cmd_get(int as_config) {
    int fd = dev_open();
    if (fd < 0) return 1;
    for (int k = 1; k <= NUM_KEYS; k++) {
        uint8_t mod, key;
        if (read_key(fd, k, &mod, &key) != 0) {
            fprintf(stderr, "elfctl: failed reading key%d\n", k);
            close(fd);
            return 1;
        }
        char b[64];
        format_binding(mod, key, b, sizeof b);
        if (as_config)
            printf("key%d = %s\n", k, b);
        else
            printf("key%d: %s\n", k, b);
    }
    close(fd);
    return 0;
}

static int cmd_set(int keynum, const char *binding) {
    if (keynum < 1 || keynum > NUM_KEYS) {
        fprintf(stderr, "elfctl: key must be 1..%d\n", NUM_KEYS);
        return 1;
    }
    uint8_t mod, key;
    if (parse_binding(binding, &mod, &key) != 0)
        return 1;
    int fd = dev_open();
    if (fd < 0) return 1;
    int rc = set_key(fd, keynum, mod, key);
    close(fd);
    if (rc == 0) {
        char b[64];
        format_binding(mod, key, b, sizeof b);
        printf("key%d = %s  (ok)\n", keynum, b);
    }
    return rc == 0 ? 0 : 1;
}

static int cmd_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "elfctl: cannot open %s: %s\n", path, strerror(errno)); return 1; }
    int fd = dev_open();
    if (fd < 0) { fclose(f); return 1; }
    char line[256];
    int rc = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == '\0' || *p == '\n') continue;
        int kn; char bind[128];
        if (sscanf(p, "key%d = %127s", &kn, bind) == 2 ||
            sscanf(p, "key%d=%127s", &kn, bind) == 2) {
            uint8_t mod, key;
            if (parse_binding(bind, &mod, &key) != 0) { rc = 1; continue; }
            if (set_key(fd, kn, mod, key) != 0) { rc = 1; continue; }
            char b[64]; format_binding(mod, key, b, sizeof b);
            printf("key%d = %s  (ok)\n", kn, b);
        } else {
            fprintf(stderr, "elfctl: skipping unparseable line: %s", line);
            rc = 1;
        }
    }
    close(fd);
    fclose(f);
    return rc;
}

static void usage(void) {
    fprintf(stderr,
        "elfctl — configure PCsensor ElfKey (MK424BT, 3553:c140)\n\n"
        "usage:\n"
        "  elfctl list                 identify the device\n"
        "  elfctl get                  show current key bindings\n"
        "  elfctl set <key> <binding>  set one key, e.g. `set 1 f13`, `set 2 ctrl-c`\n"
        "  elfctl save [file]          dump config (stdout if no file)\n"
        "  elfctl load <file>          apply a config file\n"
        "  elfctl keys                 list all supported key/modifier names\n\n"
        "bindings: a-z 0-9 f1-f24 enter esc tab space arrows etc.,\n"
        "          with modifier prefixes: ctrl- shift- alt- gui- (r* for right)\n"
        "          run `elfctl keys` for the full list\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }
    if (getenv("ELFCTL_DEBUG")) g_debug = 1;
    const char *cmd = argv[1];

    if (strcmp(cmd, "list") == 0)
        return cmd_list();
    if (strcmp(cmd, "keys") == 0)
        return cmd_keys();
    if (strcmp(cmd, "get") == 0)
        return cmd_get(0);
    if (strcmp(cmd, "save") == 0) {
        if (argc >= 3) {
            FILE *f = freopen(argv[2], "w", stdout);
            if (!f) { fprintf(stderr, "elfctl: cannot write %s\n", argv[2]); return 1; }
        }
        return cmd_get(1);
    }
    if (strcmp(cmd, "set") == 0) {
        if (argc != 4) { usage(); return 2; }
        return cmd_set(atoi(argv[2]), argv[3]);
    }
    if (strcmp(cmd, "load") == 0) {
        if (argc != 3) { usage(); return 2; }
        return cmd_load(argv[2]);
    }
    usage();
    return 2;
}
