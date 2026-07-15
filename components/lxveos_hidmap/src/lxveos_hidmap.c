// lxveos_hidmap — see lxveos_hidmap.h. Dependency-free (libc-only) US-layout HID keystroke map,
// host-tested off-target (tests/host_c/test_hidmap.c). Extracted verbatim from lxveos_ble_hid.c.
#include "lxveos_hidmap.h"

#include <strings.h>  // strcasecmp

#define MOD_LSHIFT 0x02

bool ascii_to_hid(char c, uint8_t *mod, uint8_t *key)
{
    *mod = 0;
    *key = 0;
    if (c >= 'a' && c <= 'z') { *key = 0x04 + (c - 'a'); return true; }
    if (c >= 'A' && c <= 'Z') { *mod = MOD_LSHIFT; *key = 0x04 + (c - 'A'); return true; }
    if (c >= '1' && c <= '9') { *key = 0x1E + (c - '1'); return true; }
    if (c == '0') { *key = 0x27; return true; }
    switch (c) {
    case ' ':  *key = 0x2C; return true;
    case '\n': *key = 0x28; return true;  // Enter
    case '\t': *key = 0x2B; return true;  // Tab
    case '-':  *key = 0x2D; return true;
    case '_':  *mod = MOD_LSHIFT; *key = 0x2D; return true;
    case '=':  *key = 0x2E; return true;
    case '+':  *mod = MOD_LSHIFT; *key = 0x2E; return true;
    case '[':  *key = 0x2F; return true;
    case '{':  *mod = MOD_LSHIFT; *key = 0x2F; return true;
    case ']':  *key = 0x30; return true;
    case '}':  *mod = MOD_LSHIFT; *key = 0x30; return true;
    case '\\': *key = 0x31; return true;
    case '|':  *mod = MOD_LSHIFT; *key = 0x31; return true;
    case ';':  *key = 0x33; return true;
    case ':':  *mod = MOD_LSHIFT; *key = 0x33; return true;
    case '\'': *key = 0x34; return true;
    case '"':  *mod = MOD_LSHIFT; *key = 0x34; return true;
    case '`':  *key = 0x35; return true;
    case '~':  *mod = MOD_LSHIFT; *key = 0x35; return true;
    case ',':  *key = 0x36; return true;
    case '<':  *mod = MOD_LSHIFT; *key = 0x36; return true;
    case '.':  *key = 0x37; return true;
    case '>':  *mod = MOD_LSHIFT; *key = 0x37; return true;
    case '/':  *key = 0x38; return true;
    case '?':  *mod = MOD_LSHIFT; *key = 0x38; return true;
    case '!':  *mod = MOD_LSHIFT; *key = 0x1E; return true;
    case '@':  *mod = MOD_LSHIFT; *key = 0x1F; return true;
    case '#':  *mod = MOD_LSHIFT; *key = 0x20; return true;
    case '$':  *mod = MOD_LSHIFT; *key = 0x21; return true;
    case '%':  *mod = MOD_LSHIFT; *key = 0x22; return true;
    case '^':  *mod = MOD_LSHIFT; *key = 0x23; return true;
    case '&':  *mod = MOD_LSHIFT; *key = 0x24; return true;
    case '*':  *mod = MOD_LSHIFT; *key = 0x25; return true;
    case '(':  *mod = MOD_LSHIFT; *key = 0x26; return true;
    case ')':  *mod = MOD_LSHIFT; *key = 0x27; return true;
    default:   return false;
    }
}

uint8_t named_key(const char *name)
{
    if (name == NULL) return 0;  // defensive: the driver always passes a token, but never deref a NULL
    if (!strcasecmp(name, "ENTER") || !strcasecmp(name, "RETURN")) return 0x28;
    if (!strcasecmp(name, "TAB"))    return 0x2B;
    if (!strcasecmp(name, "ESC") || !strcasecmp(name, "ESCAPE")) return 0x29;
    if (!strcasecmp(name, "SPACE"))  return 0x2C;
    if (!strcasecmp(name, "BACKSPACE") || !strcasecmp(name, "BKSP")) return 0x2A;
    if (!strcasecmp(name, "DELETE") || !strcasecmp(name, "DEL")) return 0x4C;
    if (!strcasecmp(name, "UP"))     return 0x52;
    if (!strcasecmp(name, "DOWN"))   return 0x51;
    if (!strcasecmp(name, "LEFT"))   return 0x50;
    if (!strcasecmp(name, "RIGHT"))  return 0x4F;
    if (!strcasecmp(name, "HOME"))   return 0x4A;
    if (!strcasecmp(name, "END"))    return 0x4D;
    if (name[0] >= 'a' && name[0] <= 'z' && name[1] == '\0') return 0x04 + (name[0] - 'a');
    if (name[0] >= 'A' && name[0] <= 'Z' && name[1] == '\0') return 0x04 + (name[0] - 'A');
    return 0;
}
