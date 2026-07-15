// lxveos_evt — see lxveos_evt.h. Bounded builder for "LXVEOS/1 <type> k=v ..." serial-bridge event lines.
// Pure libc (stdio/stdarg) so it host-tests off-target with no ESP-IDF dependency.
#include "lxveos_evt.h"

#include <stdarg.h>
#include <stdio.h>

// Emission gate (see the header). Plain process-global; set by the `bridge` command, read at every emit site.
static bool s_evt_enabled = false;

void lxveos_evt_set_enabled(bool on) { s_evt_enabled = on; }
bool lxveos_evt_enabled(void) { return s_evt_enabled; }

// Bounded printf-append: writes at buf[off..] within cap (which includes the NUL), advances off. On overflow it
// clamps off to cap-1 (buf stays NUL-terminated at the boundary) so every later append is a no-op — the line is
// truncated, never overrun. The format attribute makes the compiler check each call's format vs its args.
__attribute__((format(printf, 4, 5)))
static size_t evt_appendf(char *buf, size_t cap, size_t off, const char *fmt, ...)
{
    if (buf == NULL || cap == 0 || off >= cap) {
        return (cap == 0) ? 0 : (off > cap - 1 ? cap - 1 : off);
    }
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + off, cap - off, fmt, ap);
    va_end(ap);
    if (w < 0) {
        return off;
    }
    off += (size_t)w;
    if (off >= cap) {
        off = cap - 1;  // truncated by vsnprintf; keep off at the NUL position
    }
    return off;
}

size_t lxveos_evt_begin(char *buf, size_t cap, const char *type)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    buf[0] = '\0';
    if (type == NULL || type[0] == '\0') {
        return 0;
    }
    return evt_appendf(buf, cap, 0, "LXVEOS/1 %s", type);
}

size_t lxveos_evt_kv(char *buf, size_t cap, size_t off, const char *key, const char *value)
{
    if (key == NULL || value == NULL) {
        return off;
    }
    return evt_appendf(buf, cap, off, " %s=%s", key, value);
}

size_t lxveos_evt_kv_int(char *buf, size_t cap, size_t off, const char *key, long value)
{
    if (key == NULL) {
        return off;
    }
    return evt_appendf(buf, cap, off, " %s=%ld", key, value);
}

size_t lxveos_evt_kv_uint(char *buf, size_t cap, size_t off, const char *key, unsigned long value)
{
    if (key == NULL) {
        return off;
    }
    return evt_appendf(buf, cap, off, " %s=%lu", key, value);
}

size_t lxveos_evt_kv_hex(char *buf, size_t cap, size_t off, const char *key, const uint8_t *bytes, size_t len)
{
    if (key == NULL) {
        return off;
    }
    off = evt_appendf(buf, cap, off, " %s=", key);
    for (size_t i = 0; i < len && bytes != NULL; i++) {
        off = evt_appendf(buf, cap, off, "%02x", bytes[i]);
    }
    return off;
}

size_t lxveos_evt_kv_mac(char *buf, size_t cap, size_t off, const char *key, const uint8_t mac[6])
{
    if (key == NULL || mac == NULL) {
        return off;
    }
    return evt_appendf(buf, cap, off, " %s=%02x:%02x:%02x:%02x:%02x:%02x",
                       key, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
