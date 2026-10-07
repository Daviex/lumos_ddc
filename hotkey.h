#ifndef HOTKEY_H
#define HOTKEY_H

/* Global hotkey model: parse and format the text stored in config.ini, such as
   "Ctrl+Alt+Up". Win32-free so it can be unit-tested with native gcc
   (test_hotkey.c). The values below are the Win32 MOD_* and VK_* codes, so a
   Hotkey can be passed to RegisterHotKey unchanged. */

#define HK_MOD_ALT      0x0001
#define HK_MOD_CONTROL  0x0002
#define HK_MOD_SHIFT    0x0004
#define HK_MOD_WIN      0x0008

/* The configurable actions, in Settings window order. */
enum {
    HOTKEY_BRIGHTEN = 0,
    HOTKEY_DIM,
    HOTKEY_POPUP,
    HOTKEY_COUNT
};

typedef struct {
    unsigned mods;   /* HK_MOD_* flags */
    unsigned vk;     /* virtual-key code, 0 = disabled */
} Hotkey;

#define HOTKEY_TEXT_MAX 48

/* Parse "Ctrl+Alt+Up" (case-insensitive, any modifier order) or "None".
   Returns 1 and fills *out on success, 0 when the text is not a usable hotkey. */
int Hotkey_Parse(const char *text, Hotkey *out);

/* Format as "Ctrl+Alt+Up", or "None" when disabled. */
void Hotkey_Format(Hotkey hk, char *buf, int cch);

/* Name of a key Lumos accepts as a hotkey, or NULL when the key is not
   supported (modifier keys alone, OEM punctuation that depends on the layout). */
const char *Hotkey_KeyName(unsigned vk);

/* A hotkey needs Ctrl, Alt or Win. Shift alone would take a key away from
   normal typing in every application. A disabled hotkey is also valid. */
int Hotkey_IsValid(Hotkey hk);

int Hotkey_Equal(Hotkey a, Hotkey b);

/* Defaults for a new install: Ctrl+Win+Up, Ctrl+Win+Down, Ctrl+Win+B. */
Hotkey Hotkey_Default(int action);

/* What a config.ini from 1.1 or older gets for a hotkey it has no line for:
   brighten and dim were fixed to Ctrl+Alt+Up and Ctrl+Alt+Down then, and an
   upgrade must not take them away. The popup hotkey is new and uses Default. */
Hotkey Hotkey_LegacyDefault(int action);

#endif /* HOTKEY_H */
