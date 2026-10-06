#include "hotkey.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    unsigned    vk;
    const char *name;
} KeyName;

/* Keys whose meaning does not depend on the keyboard layout. Letters and
   digits are handled by range below. */
static const KeyName kKeys[] = {
    { 0x08, "Backspace" }, { 0x09, "Tab" },      { 0x0D, "Enter" },
    { 0x13, "Pause" },     { 0x20, "Space" },    { 0x21, "PageUp" },
    { 0x22, "PageDown" },  { 0x23, "End" },      { 0x24, "Home" },
    { 0x25, "Left" },      { 0x26, "Up" },       { 0x27, "Right" },
    { 0x28, "Down" },      { 0x2D, "Insert" },   { 0x2E, "Delete" },
    { 0x6A, "NumMultiply" }, { 0x6B, "NumPlus" }, { 0x6D, "NumMinus" },
    { 0x6E, "NumDecimal" },  { 0x6F, "NumDivide" },
};

static const struct {
    unsigned    flag;
    const char *name;
} kMods[] = {
    { HK_MOD_CONTROL, "Ctrl" },
    { HK_MOD_ALT,     "Alt" },
    { HK_MOD_SHIFT,   "Shift" },
    { HK_MOD_WIN,     "Win" },
};

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* Static buffers for generated names; the table above covers the rest. */
static char g_letters[26][2];
static char g_digits[10][2];
static char g_fkeys[24][4];
static char g_numpad[10][5];

const char *Hotkey_KeyName(unsigned vk)
{
    if (vk >= 'A' && vk <= 'Z') {
        g_letters[vk - 'A'][0] = (char)vk;
        return g_letters[vk - 'A'];
    }
    if (vk >= '0' && vk <= '9') {
        g_digits[vk - '0'][0] = (char)vk;
        return g_digits[vk - '0'];
    }
    if (vk >= 0x70 && vk <= 0x87) {          /* F1..F24 */
        snprintf(g_fkeys[vk - 0x70], sizeof(g_fkeys[0]), "F%u", vk - 0x70 + 1);
        return g_fkeys[vk - 0x70];
    }
    if (vk >= 0x60 && vk <= 0x69) {          /* numpad 0..9 */
        snprintf(g_numpad[vk - 0x60], sizeof(g_numpad[0]), "Num%u", vk - 0x60);
        return g_numpad[vk - 0x60];
    }
    for (int i = 0; i < COUNT(kKeys); i++)
        if (kKeys[i].vk == vk)
            return kKeys[i].name;
    return NULL;
}

static int EqualNoCase(const char *a, int alen, const char *b)
{
    int blen = (int)strlen(b);
    if (alen != blen)
        return 0;
    for (int i = 0; i < alen; i++) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x = (char)(x - 'a' + 'A');
        if (y >= 'a' && y <= 'z') y = (char)(y - 'a' + 'A');
        if (x != y)
            return 0;
    }
    return 1;
}

/* Token -> key code, or 0 when the token names no supported key. */
static unsigned KeyFromToken(const char *tok, int len)
{
    for (unsigned vk = 1; vk < 0x100; vk++) {
        const char *name = Hotkey_KeyName(vk);
        if (name && EqualNoCase(tok, len, name))
            return vk;
    }
    return 0;
}

static unsigned ModFromToken(const char *tok, int len)
{
    for (int i = 0; i < COUNT(kMods); i++)
        if (EqualNoCase(tok, len, kMods[i].name))
            return kMods[i].flag;
    if (EqualNoCase(tok, len, "Control"))
        return HK_MOD_CONTROL;
    return 0;
}

int Hotkey_Parse(const char *text, Hotkey *out)
{
    Hotkey hk = { 0, 0 };
    if (!text || !out)
        return 0;

    while (*text == ' ') text++;
    int total = (int)strlen(text);
    while (total > 0 && text[total - 1] == ' ') total--;
    if (total == 0 || EqualNoCase(text, total, "None")) {
        *out = hk;
        return 1;
    }

    int pos = 0;
    while (pos < total) {
        int start = pos;
        while (pos < total && text[pos] != '+') pos++;
        int len = pos - start;
        while (len > 0 && text[start] == ' ') { start++; len--; }
        while (len > 0 && text[start + len - 1] == ' ') len--;
        if (len == 0)
            return 0;                       /* "Ctrl++Up" or a trailing '+' */

        int last = (pos >= total);
        if (!last) {
            unsigned mod = ModFromToken(text + start, len);
            if (!mod || (hk.mods & mod))
                return 0;                   /* unknown or repeated modifier */
            hk.mods |= mod;
            pos++;                          /* skip '+' */
        } else {
            hk.vk = KeyFromToken(text + start, len);
            if (!hk.vk)
                return 0;
        }
    }

    /* Text other than "None" must end in a key: "Ctrl+" is a typo, not a
       request to disable the hotkey. */
    if (hk.vk == 0 || !Hotkey_IsValid(hk))
        return 0;
    *out = hk;
    return 1;
}

void Hotkey_Format(Hotkey hk, char *buf, int cch)
{
    if (!buf || cch <= 0)
        return;
    const char *key = Hotkey_KeyName(hk.vk);
    if (hk.vk == 0 || !key) {
        snprintf(buf, (size_t)cch, "None");
        return;
    }
    int pos = 0;
    buf[0] = '\0';
    for (int i = 0; i < COUNT(kMods); i++) {
        if ((hk.mods & kMods[i].flag) && pos < cch)
            pos += snprintf(buf + pos, (size_t)(cch - pos), "%s+", kMods[i].name);
    }
    if (pos < cch)
        snprintf(buf + pos, (size_t)(cch - pos), "%s", key);
}

int Hotkey_IsValid(Hotkey hk)
{
    if (hk.vk == 0)
        return 1;
    if (!Hotkey_KeyName(hk.vk))
        return 0;
    return (hk.mods & (HK_MOD_CONTROL | HK_MOD_ALT | HK_MOD_WIN)) != 0;
}

int Hotkey_Equal(Hotkey a, Hotkey b)
{
    if (a.vk == 0 && b.vk == 0)
        return 1;
    return a.vk == b.vk && a.mods == b.mods;
}

Hotkey Hotkey_Default(int action)
{
    Hotkey hk = { HK_MOD_CONTROL | HK_MOD_WIN, 0 };
    switch (action) {
    case HOTKEY_BRIGHTEN: hk.vk = 0x26; break;   /* Up */
    case HOTKEY_DIM:      hk.vk = 0x28; break;   /* Down */
    case HOTKEY_POPUP:    hk.vk = 'B';  break;
    default:              hk.mods = 0;  break;
    }
    return hk;
}

Hotkey Hotkey_LegacyDefault(int action)
{
    Hotkey hk = Hotkey_Default(action);
    if (action == HOTKEY_BRIGHTEN || action == HOTKEY_DIM)
        hk.mods = HK_MOD_CONTROL | HK_MOD_ALT;   /* the fixed hotkeys up to 1.1 */
    return hk;
}
