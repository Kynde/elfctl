/* elfctl — configure PCsensor ElfKey devices (MK424BT 4-key macropad, 3553:c140)
 *
 * Talks to the device's config HID interface over raw /dev/hidraw with zero
 * external dependencies (no hidapi, no libusb). Reverse-engineered ElfKey
 * protocol; validated empirically against an MK424BT (firmware V1.1).
 *
 * Commands:
 *   elfctl list                 identify the device (model, firmware)
 *   elfctl get                  read all keys, decode to names
 *   elfctl set <key> <binding>  write one key (e.g. `set 1 f13`, `set 2 ctrl-c`,
 *                               or `set 1 macro:3` to run a macro)
 *   elfctl macro ...            list/set/delete the 8 macro slots
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

#define ELFCTL_VERSION "0.3.1"  /* dragged along by /release to match the git tag */

#define VENDOR  "3553"
#define PRODUCT "C140"
#define CONFIG_IFACE "01"   /* interface carrying the OUT endpoint (ep_05) */
#define NUM_KEYS 4
#define NUM_LAYERS 3        /* MK424BT stores 3 layers, switched by the S button */
#define NUM_MACROS 8        /* macro table slots (MK424 is not "short macro" = 8) */
#define MACRO_MAX_STEPS 32  /* our self-imposed cap; device limit is far higher */

/* ---- ElfKey command opcodes (byte[1] of the 8-byte report payload) ----
 * NOTE: only read/set-key and the layer opcodes are used here. The protocol
 * family also contains firmware-flash (0x20) and set-model (0x60) opcodes
 * which this tool deliberately never emits. */
#define OP_READ_MODEL 0x83
#define OP_READ_KEY   0x82
#define OP_SET_KEY    0x81

/* Macro-table opcodes (0xC0 family). Documented in the official ElfKey
 * configurator's source (setMacro/readMacroList/deleteMacro) and corroborated by
 * rgerganov's footswitch driver (same PCsensor family, 3553:b001). See
 * docs/PROTOCOL.md "Macros". A macro is a named, timed list of actions stored in
 * its own table; a key references one via a normal set-key write tagged with the
 * macro key-type. */
#define OP_SET_MACRO    0xC0
#define OP_READ_MACRO   0xC1
#define OP_DELETE_MACRO 0xC2

/* set-key byte[1] is the key TYPE, not a count: 0x01=single key, 0x0A=macro link
 * (the firmware also defines mouse/media/etc., which elfctl does not emit). */
#define KEYTYPE_KEY   0x01
#define KEYTYPE_MACRO 0x0A

/* Per-action byte in a macro step: press / release / click. Decoded from the
 * app; the live-record flow uses the parallel 0xC3 enter/pressed/released set.
 * elfctl emits CLICK (atomic down+up) for ordinary chord steps. */
#define ACT_CLICK 0x01

/* Layer (a.k.a. "func layer") opcodes. Verified against the MK424BT and the
 * official ElfKey configurator's own source (readFuncLayerCmd etc.). A key on
 * layer L (1-based) lives at device index (L-1)*16 + key, so the S button just
 * switches which 16-index block the keys read from. byte[2] of OP_ENABLE_LAYERS
 * is a bitmask: bit0=L1 (always on), bit1=L2, bit2=L3 -> 0x01/0x03/0x07. */
#define OP_READ_ACTIVE_LAYER  0xD1  /* resp: [d1 00 <active>] */
#define OP_ENABLE_LAYERS      0xD2  /* [01 d2 <mask> ..] set enabled-layers mask */
#define OP_READ_ENABLED_MASK  0xD3  /* resp: [d3 <mask> 00] */
#define OP_SWITCH_LAYER       0xD4  /* [01 d4 <layer> ..] software S-button */

/* Device key index for a 1-based (layer, key). */
static int key_index(int layer, int key) { return (layer - 1) * 16 + key; }

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

/* Read one slot at device index `idx`. Returns 0 on success and reports the slot
 * type via *type (KEYTYPE_KEY or KEYTYPE_MACRO). For a key binding (*type==KEY)
 * (mod,key) is the chord; for a macro link (*type==MACRO) `key` carries the
 * macro id (byte[3]) and `mod` is 0. idx is (layer-1)*16 + key. */
static int read_key_typed(int fd, int idx, uint8_t *type, uint8_t *mod, uint8_t *key) {
    uint8_t cmd[8] = {1, OP_READ_KEY, 8, (uint8_t)idx, 0, 0, 0, 0};
    drain_input(fd); /* clear any stale/ACK reports so we read THIS response */
    if (write_cmd(fd, cmd) != 0)
        return -1;
    /* A real key-read response is framed [len, type, b2, b3, ...]. The device
     * may also emit ACK reports (byte[0]=0x81) from a preceding set; skip those.
     * A single-key binding has len=0x04 type=0x01 (mod,key). A macro link has
     * type=0x0A with the macro id in byte[3]. Accept either; skip the rest. */
    uint8_t r[64];
    for (int tries = 0; tries < 8; tries++) {
        ssize_t n = read_report(fd, r, sizeof r, 1000);
        if (n < 4)
            return -1;
        if (r[1] == KEYTYPE_MACRO) {
            *type = KEYTYPE_MACRO;
            *mod = 0;
            *key = r[2];   /* macro id (app: case 10 -> macroId at this byte) */
            return 0;
        }
        if (r[0] == 0x04) {
            *type = KEYTYPE_KEY;
            *mod = r[2];
            *key = r[3];
            return 0;
        }
        /* else: ACK or unrelated report — keep reading */
    }
    return -1;
}

/* Write the slot at device index `idx`, then read back to verify. `type` selects
 * a single-key binding (KEYTYPE_KEY: mod+key) or a macro link (KEYTYPE_MACRO:
 * arg2 is the macro id). Returns 0 on verified success. */
static int set_key_typed(int fd, int idx, uint8_t type, uint8_t mod, uint8_t key) {
    /* A single-key binding uses payload length 4: header byte[2]=0x04, data
     * [0x04, 0x01, mod, key]. A macro link uses length 8: header byte[2]=0x08,
     * data [0x08, 0x0A, id, ..] (app's saveMacro: setKeyValueCmd[2]=8,
     * saveMacroData=[8,10,id,..]). */
    uint8_t plen = (type == KEYTYPE_MACRO) ? 0x08 : 0x04;
    uint8_t hdr[8] = {1, OP_SET_KEY, plen, (uint8_t)idx, 0, 0, 0, 0};
    uint8_t data[8] = {plen, type, mod, key, 0, 0, 0, 0};
    if (type == KEYTYPE_MACRO) { data[2] = key; data[3] = 0; }  /* id at byte[2] */
    if (write_cmd(fd, hdr) != 0)
        return -1;
    if (write_cmd(fd, data) != 0)
        return -1;
    drain_input(fd); /* consume the device's set-key ACK report (byte[1]=0x55) */

    uint8_t rtype, rmod, rkey;
    if (read_key_typed(fd, idx, &rtype, &rmod, &rkey) != 0) {
        fprintf(stderr, "elfctl: wrote index %d but could not read back to verify\n", idx);
        return -1;
    }
    if (type == KEYTYPE_MACRO) {
        if (rtype != KEYTYPE_MACRO || rkey != key) {
            fprintf(stderr, "elfctl: verify mismatch at index %d: wrote macro id %d, "
                            "read type=%02x id=%d\n", idx, key, rtype, rkey);
            return -1;
        }
        return 0;
    }
    if (rtype != KEYTYPE_KEY || rmod != mod || rkey != key) {
        fprintf(stderr, "elfctl: verify mismatch at index %d: wrote mod=%02x key=%02x, "
                        "read type=%02x mod=%02x key=%02x\n", idx, mod, key, rtype, rmod, rkey);
        return -1;
    }
    return 0;
}

/* Write a single-key binding to slot `idx` (back-compat wrapper). */
static int set_key(int fd, int idx, uint8_t mod, uint8_t key) {
    return set_key_typed(fd, idx, KEYTYPE_KEY, mod, key);
}

/* ------------------------------- layers ----------------------------------- */

/* Send a layer read opcode and return the data byte at response offset `off`,
 * or -1. Responses echo the opcode in byte[0]: 0xD1 -> [d1 00 <active>] (off 2),
 * 0xD3 -> [d3 <mask> 00] (off 1). */
static int read_layer_byte(int fd, uint8_t opcode, int off) {
    drain_input(fd);
    uint8_t cmd[8] = {1, opcode, 0, 0, 0, 0, 0, 0};
    if (write_cmd(fd, cmd) != 0)
        return -1;
    uint8_t r[64];
    for (int tries = 0; tries < 8; tries++) {
        ssize_t n = read_report(fd, r, sizeof r, 1000);
        if (n <= off)
            continue;
        if (r[0] == opcode)
            return r[off];
    }
    return -1;
}

static int read_active_layer(int fd) { return read_layer_byte(fd, OP_READ_ACTIVE_LAYER, 2); }
static int read_enabled_mask(int fd) { return read_layer_byte(fd, OP_READ_ENABLED_MASK, 1); }

/* Write the enabled-layers bitmask, then read it back. Returns 0 if verified. */
static int set_enabled_mask(int fd, uint8_t mask) {
    uint8_t cmd[8] = {1, OP_ENABLE_LAYERS, mask, 0, 0, 0, 0, 0};
    if (write_cmd(fd, cmd) != 0)
        return -1;
    drain_input(fd);
    int rb = read_enabled_mask(fd);
    if (rb < 0 || (uint8_t)rb != mask) {
        fprintf(stderr, "elfctl: enable-layers verify failed (wrote 0x%02x, read 0x%02x)\n",
                mask, rb);
        return -1;
    }
    return 0;
}

/* ------------------------------- macros ----------------------------------- */

/* One macro step: a chord (mod+key) emitted as an atomic click, plus the delay
 * (ms) the firmware waits AFTER it. The device's action model is finer (separate
 * press/release events), but a click per chord covers ordinary keystroke macros
 * and round-trips cleanly. See docs/PROTOCOL.md "Macros". */
struct macro_step { uint8_t mod, key; uint16_t delay_ms; };

struct macro {
    char name[33];
    uint8_t mode;        /* playback mode byte (0 = once/default) */
    int nsteps;
    struct macro_step steps[MACRO_MAX_STEPS];
};

#define MACRO_DEFAULT_DELAY 10  /* ms between steps; matches the app's default */

/* Parse a macro sequence like "ctrl-c, 50ms, ctrl-v" into a step list. Steps are
 * separated by ','; a bare "<n>ms" or "<n>" token sets the delay AFTER the
 * preceding step instead of adding a step. Returns 0 on success. */
static int parse_macro_seq(const char *seq, struct macro *m) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", seq);
    m->nsteps = 0;

    char *save = NULL;
    for (char *tok = strtok_r(tmp, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (isspace((unsigned char)*tok)) tok++;
        char *end = tok + strlen(tok);
        while (end > tok && isspace((unsigned char)end[-1])) *--end = '\0';
        if (*tok == '\0')
            continue;

        /* A pure-number (optionally "ms"-suffixed) token is a delay for the
         * previous step, not a keystroke. */
        char *p = tok;
        int is_delay = isdigit((unsigned char)*p);
        for (; *p; p++)
            if (!isdigit((unsigned char)*p)) {
                if ((p[0]=='m' && p[1]=='s' && p[2]=='\0')) break;
                is_delay = 0; break;
            }
        if (is_delay) {
            int d = atoi(tok);
            if (m->nsteps == 0) {
                fprintf(stderr, "elfctl: delay '%s' has no preceding step\n", tok);
                return -1;
            }
            if (d < 0 || d > 65535) {
                fprintf(stderr, "elfctl: delay out of range: %s\n", tok);
                return -1;
            }
            m->steps[m->nsteps - 1].delay_ms = (uint16_t)d;
            continue;
        }

        if (m->nsteps >= MACRO_MAX_STEPS) {
            fprintf(stderr, "elfctl: too many macro steps (max %d)\n", MACRO_MAX_STEPS);
            return -1;
        }
        uint8_t mod, key;
        if (parse_binding(tok, &mod, &key) != 0)
            return -1;  /* parse_binding already reported the bad token */
        m->steps[m->nsteps].mod = mod;
        m->steps[m->nsteps].key = key;
        m->steps[m->nsteps].delay_ms = MACRO_DEFAULT_DELAY;
        m->nsteps++;
    }
    if (m->nsteps == 0) {
        fprintf(stderr, "elfctl: empty macro sequence\n");
        return -1;
    }
    return 0;
}

/* Render a macro's steps to "ctrl-c, 50ms, ctrl-v" into buf. A non-default delay
 * after a step is emitted as its own ", <n>ms" token so the result re-parses. */
static void format_macro_seq(const struct macro *m, char *buf, size_t n) {
    buf[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < m->nsteps; i++) {
        char b[64];
        format_binding(m->steps[i].mod, m->steps[i].key, b, sizeof b);
        int w = snprintf(buf + used, n - used, "%s%s", used ? ", " : "", b);
        if (w > 0) used += (size_t)w;
        if (m->steps[i].delay_ms != MACRO_DEFAULT_DELAY) {
            w = snprintf(buf + used, n - used, ", %ums", m->steps[i].delay_ms);
            if (w > 0) used += (size_t)w;
        }
    }
}

/* Build the action-bytes blob for a macro (everything after the mode byte). Each
 * chord becomes one keyboard action: [07 01 mod key CLICK delayHi delayLo].
 * Returns the byte count, or -1 if it would overflow `cap`. */
static int macro_action_bytes(const struct macro *m, uint8_t *out, size_t cap) {
    size_t k = 0;
    for (int i = 0; i < m->nsteps; i++) {
        if (k + 7 > cap)
            return -1;
        out[k++] = 0x07;                 /* record length */
        out[k++] = 0x01;                 /* keyboard action */
        out[k++] = m->steps[i].mod;
        out[k++] = m->steps[i].key;
        out[k++] = ACT_CLICK;            /* press+release */
        out[k++] = (uint8_t)(m->steps[i].delay_ms >> 8);
        out[k++] = (uint8_t)(m->steps[i].delay_ms & 0xff);
    }
    return (int)k;
}

/* Write a macro to slot `id` (1..NUM_MACROS) via 0xC0, mirroring the app's
 * setMacro framing: header, 4 name reports (32 bytes), then a report carrying
 * [lenHi lenLo mode + first 5 action bytes], then the rest 8 per report. */
static int set_macro(int fd, int id, const struct macro *m) {
    uint8_t act[MACRO_MAX_STEPS * 7];
    int nact = macro_action_bytes(m, act, sizeof act);
    if (nact < 0) { fprintf(stderr, "elfctl: macro too large\n"); return -1; }

    int length = nact + 3;               /* app: actionBytes + 3 (len2,mode incl.) */
    uint8_t lenHi = (uint8_t)(length >> 8), lenLo = (uint8_t)(length & 0xff);

    uint8_t hdr[8] = {1, OP_SET_MACRO, lenHi, lenLo, (uint8_t)id, 0, 0, 0};
    if (write_cmd(fd, hdr) != 0)
        return -1;
    drain_input(fd);

    /* 32 bytes of NUL-padded name across 4 reports. */
    uint8_t nm[32] = {0};
    for (size_t i = 0; i < sizeof nm && m->name[i]; i++) nm[i] = (uint8_t)m->name[i];
    for (int i = 0; i < 4; i++)
        if (write_cmd(fd, nm + i * 8) != 0)
            return -1;

    /* First payload report: [lenHi lenLo mode a0..a4]. */
    uint8_t first[8] = {lenHi, lenLo, m->mode, 0, 0, 0, 0, 0};
    for (int j = 0; j < 5 && j < nact; j++) first[3 + j] = act[j];
    if (write_cmd(fd, first) != 0)
        return -1;

    /* Remaining action bytes, 8 per report. */
    for (int off = 5; off < nact; off += 8) {
        uint8_t rep[8] = {0};
        for (int j = 0; j < 8 && off + j < nact; j++) rep[j] = act[off + j];
        if (write_cmd(fd, rep) != 0)
            return -1;
    }
    drain_input(fd);
    return 0;
}

/* Read exactly one 8-byte report into `dst`. Returns 0 on success, -1 on
 * timeout/error. Used where the protocol prescribes a fixed report count. */
static int read_one(int fd, uint8_t dst[8]) {
    uint8_t r[64];
    ssize_t n = read_report(fd, r, sizeof r, 1000);
    if (n < 8)
        return -1;
    memcpy(dst, r, 8);
    return 0;
}

/* Read macro slot `id` into *m. Returns 1 if defined, 0 if empty, -1 on error.
 * Empty slots reply with a single all-0xff (or all-0x00) report (verified).
 *
 * Follows the app's deterministic framing rather than reading-until-quiet: 4
 * name reports (32 bytes), one header report [lenHi lenLo mode + 5 action
 * bytes], then ceil(byteCount/8)-1 further action reports. Counting reports up
 * front avoids a race where the firmware pauses between the name and action
 * blocks longer than an inter-report timeout. */
static int read_macro(int fd, int id, struct macro *m) {
    drain_input(fd);
    uint8_t cmd[8] = {1, OP_READ_MACRO, 0, (uint8_t)id, 0, 0, 0, 0};
    if (write_cmd(fd, cmd) != 0)
        return -1;

    uint8_t rep[8];
    if (read_one(fd, rep) != 0)
        return -1;

    int all0 = 1, allff = 1;
    for (int i = 0; i < 8; i++) { if (rep[i] != 0x00) all0 = 0; if (rep[i] != 0xff) allff = 0; }
    if (all0 || allff)
        return 0;                        /* empty slot */

    memset(m, 0, sizeof *m);

    /* 32-byte name: this first report plus 3 more. */
    uint8_t name[32];
    memcpy(name, rep, 8);
    for (int i = 1; i < 4; i++)
        if (read_one(fd, name + i * 8) != 0)
            return -1;
    int k = 0;
    for (int i = 0; i < 32; i++) if (name[i]) m->name[k++] = (char)name[i];
    m->name[k] = '\0';

    /* Header report: [lenHi lenLo mode a0 a1 a2 a3 a4]. */
    uint8_t hdr[8];
    if (read_one(fd, hdr) != 0)
        return -1;
    int byteCount = hdr[0] * 256 + hdr[1];   /* = actionBytes + 3 */
    m->mode = hdr[2];

    /* Assemble the action-byte stream: 5 from the header, then the rest. */
    uint8_t act[8 + MACRO_MAX_STEPS * 7];
    int nact = 0;
    for (int j = 3; j < 8; j++) act[nact++] = hdr[j];
    int reports = (byteCount + 7) / 8;       /* ceil(byteCount/8) */
    for (int j = 0; j < reports - 1; j++) {
        uint8_t a[8];
        if (read_one(fd, a) != 0)
            break;                           /* short read: decode what we have */
        for (int b = 0; b < 8 && nact < (int)sizeof act; b++) act[nact++] = a[b];
    }

    /* Walk length-prefixed action records. */
    int p = 0;
    while (p < nact && m->nsteps < MACRO_MAX_STEPS) {
        int rl = act[p];
        if (rl == 0 || p + rl > nact)
            break;
        if (act[p + 1] == 0x01 && rl >= 7) {   /* keyboard action */
            m->steps[m->nsteps].mod = act[p + 2];
            m->steps[m->nsteps].key = act[p + 3];
            m->steps[m->nsteps].delay_ms = (uint16_t)(act[p + 5] * 256 + act[p + 6]);
            m->nsteps++;
        }
        /* mouse/other records are skipped — elfctl only emits keyboard steps */
        p += rl;
    }
    return 1;
}

/* Delete macro slot `id` via 0xC2. */
static int delete_macro(int fd, int id) {
    uint8_t cmd[8] = {1, OP_DELETE_MACRO, 0, (uint8_t)id, 0, 0, 0, 0};
    if (write_cmd(fd, cmd) != 0)
        return -1;
    drain_input(fd);
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

/* The enabled-layers mask is always a contiguous run from L1, so it maps 1:1 to
 * a count: 0x01->1, 0x03->2, 0x07->3. (Matches the official app's only values.) */
static int mask_to_count(int mask) {
    if (mask < 0) return -1;
    return (mask & 4) ? 3 : (mask & 2) ? 2 : 1;
}
static uint8_t count_to_mask(int count) {
    return (uint8_t)((1u << count) - 1);  /* 1->0x01, 2->0x03, 3->0x07 */
}

static int cmd_list(void) {
    int fd = dev_open();
    if (fd < 0) return 1;
    char model[32], fw[32];
    int rc = do_read_model(fd, model, sizeof model, fw, sizeof fw);
    int mask = read_enabled_mask(fd);
    close(fd);
    if (rc != 0) { fprintf(stderr, "elfctl: read failed\n"); return 1; }
    printf("%-10s %s:%s  (%d keys, %d/%d layers enabled)  firmware %s\n",
           model, "3553", "c140", NUM_KEYS, mask_to_count(mask), NUM_LAYERS, fw);
    return 0;
}

/* Print one layer's bindings. `as_config` selects file vs human format; for
 * layer 1 the file form stays bare `keyN` (backward compatible), higher layers
 * use `layerL.keyN`. Returns 0 on success. */
static int print_layer(int fd, int layer, int as_config) {
    for (int k = 1; k <= NUM_KEYS; k++) {
        uint8_t type, mod, key;
        if (read_key_typed(fd, key_index(layer, k), &type, &mod, &key) != 0) {
            fprintf(stderr, "elfctl: failed reading layer%d key%d\n", layer, k);
            return -1;
        }
        char b[64];
        if (type == KEYTYPE_MACRO) snprintf(b, sizeof b, "macro:%d", key);
        else                       format_binding(mod, key, b, sizeof b);
        if (as_config && layer == 1)      printf("key%d = %s\n", k, b);
        else if (as_config)               printf("layer%d.key%d = %s\n", layer, k, b);
        else                              printf("  key%d: %s\n", k, b);
    }
    return 0;
}

/* Show bindings. `which` selects a single layer (1..NUM_LAYERS) or 0 for all. */
static int cmd_get(int as_config, int which) {
    int fd = dev_open();
    if (fd < 0) return 1;
    int mask = read_enabled_mask(fd);
    int lo = which ? which : 1, hi = which ? which : NUM_LAYERS;
    if (as_config && !which)
        printf("layers = %d\n\n", mask_to_count(mask));
    for (int layer = lo; layer <= hi; layer++) {
        if (!as_config && (which || NUM_LAYERS > 1)) {
            int on = (layer == 1) || (mask >= 0 && (mask & (1 << (layer - 1))));
            printf("layer %d%s:\n", layer, on ? "" : " (disabled)");
        }
        if (print_layer(fd, layer, as_config) != 0) { close(fd); return 1; }
        if (as_config && layer < hi) printf("\n");
    }
    close(fd);
    return 0;
}

static int cmd_set(int layer, int keynum, const char *binding) {
    if (layer < 1 || layer > NUM_LAYERS) {
        fprintf(stderr, "elfctl: layer must be 1..%d\n", NUM_LAYERS);
        return 1;
    }
    if (keynum < 1 || keynum > NUM_KEYS) {
        fprintf(stderr, "elfctl: key must be 1..%d\n", NUM_KEYS);
        return 1;
    }

    /* `macro:N` links the key to macro slot N instead of a single chord. */
    int macro_id = 0;
    if (sscanf(binding, "macro:%d", &macro_id) == 1) {
        if (macro_id < 1 || macro_id > NUM_MACROS) {
            fprintf(stderr, "elfctl: macro id must be 1..%d\n", NUM_MACROS);
            return 1;
        }
        int fd = dev_open();
        if (fd < 0) return 1;
        int rc = set_key_typed(fd, key_index(layer, keynum), KEYTYPE_MACRO,
                               0, (uint8_t)macro_id);
        close(fd);
        if (rc == 0)
            printf("layer%d key%d = macro:%d  (ok)\n", layer, keynum, macro_id);
        return rc == 0 ? 0 : 1;
    }

    uint8_t mod, key;
    if (parse_binding(binding, &mod, &key) != 0)
        return 1;
    int fd = dev_open();
    if (fd < 0) return 1;
    int rc = set_key(fd, key_index(layer, keynum), mod, key);
    close(fd);
    if (rc == 0) {
        char b[64];
        format_binding(mod, key, b, sizeof b);
        printf("layer%d key%d = %s  (ok)\n", layer, keynum, b);
    }
    return rc == 0 ? 0 : 1;
}

/* `macro` subcommands: list / set / delete. */
static int cmd_macro(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
            "usage:\n"
            "  elfctl macro list                     show all %d macro slots\n"
            "  elfctl macro set <id> <name> <seq>    define macro (seq: 'ctrl-c, 50ms, ctrl-v')\n"
            "  elfctl macro delete <id>              clear a macro slot\n"
            "then link a key with:  elfctl set <key> macro:<id>\n", NUM_MACROS);
        return 2;
    }
    const char *sub = argv[2];

    if (strcmp(sub, "list") == 0) {
        int fd = dev_open();
        if (fd < 0) return 1;
        for (int id = 1; id <= NUM_MACROS; id++) {
            struct macro m;
            int r = read_macro(fd, id, &m);
            if (r < 0)      { printf("macro %d: <read error>\n", id); continue; }
            if (r == 0)     { printf("macro %d: (empty)\n", id);      continue; }
            char seq[512];
            format_macro_seq(&m, seq, sizeof seq);
            printf("macro %d: \"%s\"  [%d step%s]  %s\n", id, m.name,
                   m.nsteps, m.nsteps == 1 ? "" : "s", seq);
        }
        close(fd);
        return 0;
    }

    if (strcmp(sub, "set") == 0) {
        if (argc != 6) {
            fprintf(stderr, "usage: elfctl macro set <id> <name> <sequence>\n");
            return 2;
        }
        int id = atoi(argv[3]);
        if (id < 1 || id > NUM_MACROS) {
            fprintf(stderr, "elfctl: macro id must be 1..%d\n", NUM_MACROS);
            return 1;
        }
        struct macro m;
        memset(&m, 0, sizeof m);
        snprintf(m.name, sizeof m.name, "%s", argv[4]);
        m.mode = 0;
        if (parse_macro_seq(argv[5], &m) != 0)
            return 1;
        int fd = dev_open();
        if (fd < 0) return 1;
        if (set_macro(fd, id, &m) != 0) { close(fd); return 1; }
        /* verify by reading the slot back */
        struct macro rb;
        int r = read_macro(fd, id, &rb);
        close(fd);
        if (r != 1 || rb.nsteps != m.nsteps) {
            fprintf(stderr, "elfctl: macro %d written but read-back verify failed\n", id);
            return 1;
        }
        char seq[512];
        format_macro_seq(&rb, seq, sizeof seq);
        printf("macro %d = \"%s\"  %s  (ok)\n", id, rb.name, seq);
        return 0;
    }

    if (strcmp(sub, "delete") == 0) {
        if (argc != 4) {
            fprintf(stderr, "usage: elfctl macro delete <id>\n");
            return 2;
        }
        int id = atoi(argv[3]);
        if (id < 1 || id > NUM_MACROS) {
            fprintf(stderr, "elfctl: macro id must be 1..%d\n", NUM_MACROS);
            return 1;
        }
        int fd = dev_open();
        if (fd < 0) return 1;
        int rc = delete_macro(fd, id);
        close(fd);
        if (rc == 0) printf("macro %d deleted  (ok)\n", id);
        return rc == 0 ? 0 : 1;
    }

    fprintf(stderr, "elfctl: unknown macro subcommand '%s'\n", sub);
    return 2;
}

/* Parse a key spec "K" (layer 1) or "L:K" into (layer, key). Returns 0 on ok. */
static int parse_keyspec(const char *s, int *layer, int *key) {
    int a, b;
    if (sscanf(s, "%d:%d", &a, &b) == 2) { *layer = a; *key = b; return 0; }
    if (sscanf(s, "%d", &a) == 1)        { *layer = 1; *key = a; return 0; }
    return -1;
}

/* `layers` with no arg shows status; with an arg sets the enabled count (1..3). */
static int cmd_layers(int argc, char **argv) {
    int fd = dev_open();
    if (fd < 0) return 1;
    if (argc < 3) {
        int mask = read_enabled_mask(fd);
        int active = read_active_layer(fd);
        close(fd);
        if (mask < 0) { fprintf(stderr, "elfctl: failed reading layer state\n"); return 1; }
        printf("enabled: %d of %d layers (mask 0x%02x); active-layer byte %d\n",
               mask_to_count(mask), NUM_LAYERS, mask, active);
        printf("(the S button cycles among enabled layers; LED red=1 green=2 blue=3)\n");
        return 0;
    }
    int n = atoi(argv[2]);
    if (n < 1 || n > NUM_LAYERS) {
        fprintf(stderr, "elfctl: layer count must be 1..%d\n", NUM_LAYERS);
        close(fd);
        return 1;
    }
    int rc = set_enabled_mask(fd, count_to_mask(n));
    close(fd);
    if (rc == 0)
        printf("enabled %d layer%s  (ok)\n", n, n == 1 ? "" : "s");
    return rc == 0 ? 0 : 1;
}

/* Software equivalent of the physical S button: switch the active layer. */
static int cmd_switch(const char *arg) {
    int layer = atoi(arg);
    if (layer < 1 || layer > NUM_LAYERS) {
        fprintf(stderr, "elfctl: layer must be 1..%d\n", NUM_LAYERS);
        return 1;
    }
    int fd = dev_open();
    if (fd < 0) return 1;
    uint8_t cmd[8] = {1, OP_SWITCH_LAYER, (uint8_t)layer, 0, 0, 0, 0, 0};
    int rc = write_cmd(fd, cmd);
    drain_input(fd);
    close(fd);
    if (rc == 0) printf("switched to layer %d  (ok)\n", layer);
    return rc == 0 ? 0 : 1;
}

/* Apply one binding token to slot `idx`: either `macro:N` (a macro link) or a
 * single chord. Writes a human form to `out`. Returns 0 on success. */
static int apply_binding(int fd, int idx, const char *bind, char *out, size_t outn) {
    int macro_id = 0;
    if (sscanf(bind, "macro:%d", &macro_id) == 1) {
        if (macro_id < 1 || macro_id > NUM_MACROS) {
            fprintf(stderr, "elfctl: macro id must be 1..%d\n", NUM_MACROS);
            return -1;
        }
        if (set_key_typed(fd, idx, KEYTYPE_MACRO, 0, (uint8_t)macro_id) != 0)
            return -1;
        snprintf(out, outn, "macro:%d", macro_id);
        return 0;
    }
    uint8_t mod, key;
    if (parse_binding(bind, &mod, &key) != 0)
        return -1;
    if (set_key(fd, idx, mod, key) != 0)
        return -1;
    format_binding(mod, key, out, outn);
    return 0;
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
        int ln, kn, count; char bind[128];
        if (sscanf(p, "layers = %d", &count) == 1 ||
            sscanf(p, "layers=%d", &count) == 1) {
            if (count < 1 || count > NUM_LAYERS) {
                fprintf(stderr, "elfctl: bad layer count %d\n", count); rc = 1; continue;
            }
            if (set_enabled_mask(fd, count_to_mask(count)) != 0) { rc = 1; continue; }
            printf("layers = %d  (ok)\n", count);
        } else if (sscanf(p, "layer%d.key%d = %127s", &ln, &kn, bind) == 3 ||
                   sscanf(p, "layer%d.key%d=%127s", &ln, &kn, bind) == 3) {
            if (ln < 1 || ln > NUM_LAYERS || kn < 1 || kn > NUM_KEYS) {
                fprintf(stderr, "elfctl: bad layer/key in: %s", line); rc = 1; continue;
            }
            char b[64];
            if (apply_binding(fd, key_index(ln, kn), bind, b, sizeof b) != 0) { rc = 1; continue; }
            printf("layer%d.key%d = %s  (ok)\n", ln, kn, b);
        } else if (sscanf(p, "key%d = %127s", &kn, bind) == 2 ||
                   sscanf(p, "key%d=%127s", &kn, bind) == 2) {
            if (kn < 1 || kn > NUM_KEYS) {
                fprintf(stderr, "elfctl: bad key in: %s", line); rc = 1; continue;
            }
            char b[64];
            if (apply_binding(fd, key_index(1, kn), bind, b, sizeof b) != 0) { rc = 1; continue; }
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
        "  elfctl get [layer]          show key bindings (all layers, or one)\n"
        "  elfctl set <key> <binding>  set one key, e.g. `set 1 f13`, `set 2 ctrl-c`\n"
        "                              key may be `L:K` to target a layer, e.g. `set 2:1 g`\n"
        "                              binding may be `macro:N` to run macro slot N\n"
        "  elfctl layers [N]           show enabled layers, or enable N (1..3)\n"
        "  elfctl switch <layer>       switch active layer (software S button)\n"
        "  elfctl macro list           show the macro table\n"
        "  elfctl macro set <id> <name> <seq>   define macro (e.g. 'ctrl-c, 50ms, ctrl-v')\n"
        "  elfctl macro delete <id>    clear a macro slot\n"
        "  elfctl save [file]          dump config incl. layers (stdout if no file)\n"
        "  elfctl load <file>          apply a config file\n"
        "  elfctl keys                 list all supported key/modifier names\n"
        "  elfctl --version            print the elfctl version\n\n"
        "layers: the MK424BT stores 3 layers cycled by the physical S button (LED\n"
        "        red=1 green=2 blue=3). Only layer 1 is enabled at the factory;\n"
        "        `layers 3` enables all three. Keys are addressed K (layer 1) or L:K.\n\n"
        "macros: 8 named slots, each a sequence of chords with optional per-step\n"
        "        delays. Define with `macro set`, then point a key at it with\n"
        "        `set <key> macro:<id>`. Sequence steps are comma-separated; a bare\n"
        "        `<n>ms` token sets the delay after the previous step.\n\n"
        "bindings: a-z 0-9 f1-f24 enter esc tab space arrows etc.,\n"
        "          with modifier prefixes: ctrl- shift- alt- gui- (r* for right)\n"
        "          run `elfctl keys` for the full list\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }
    if (getenv("ELFCTL_DEBUG")) g_debug = 1;
    const char *cmd = argv[1];

    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "version") == 0) {
        printf("elfctl %s\n", ELFCTL_VERSION);
        return 0;
    }
    if (strcmp(cmd, "list") == 0)
        return cmd_list();
    if (strcmp(cmd, "keys") == 0)
        return cmd_keys();
    if (strcmp(cmd, "get") == 0)
        return cmd_get(0, argc >= 3 ? atoi(argv[2]) : 0);
    if (strcmp(cmd, "layers") == 0)
        return cmd_layers(argc, argv);
    if (strcmp(cmd, "switch") == 0) {
        if (argc != 3) { usage(); return 2; }
        return cmd_switch(argv[2]);
    }
    if (strcmp(cmd, "macro") == 0)
        return cmd_macro(argc, argv);
    if (strcmp(cmd, "save") == 0) {
        if (argc >= 3) {
            FILE *f = freopen(argv[2], "w", stdout);
            if (!f) { fprintf(stderr, "elfctl: cannot write %s\n", argv[2]); return 1; }
        }
        return cmd_get(1, 0);
    }
    if (strcmp(cmd, "set") == 0) {
        if (argc != 4) { usage(); return 2; }
        int layer, key;
        if (parse_keyspec(argv[2], &layer, &key) != 0) {
            fprintf(stderr, "elfctl: bad key spec '%s' (use K or L:K)\n", argv[2]);
            return 2;
        }
        return cmd_set(layer, key, argv[3]);
    }
    if (strcmp(cmd, "load") == 0) {
        if (argc != 3) { usage(); return 2; }
        return cmd_load(argv[2]);
    }
    usage();
    return 2;
}
