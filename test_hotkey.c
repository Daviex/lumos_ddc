#include "hotkey.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } } while (0)

static int RoundTrip(const char *in, const char *expected)
{
    Hotkey hk;
    char buf[HOTKEY_TEXT_MAX];
    if (!Hotkey_Parse(in, &hk))
        return 0;
    Hotkey_Format(hk, buf, sizeof(buf));
    return strcmp(buf, expected) == 0;
}

int main(void)
{
    Hotkey hk;

    /* Defaults format as documented */
    char buf[HOTKEY_TEXT_MAX];
    Hotkey_Format(Hotkey_Default(HOTKEY_BRIGHTEN), buf, sizeof(buf));
    CHECK(strcmp(buf, "Ctrl+Win+Up") == 0, "default brighten");
    Hotkey_Format(Hotkey_Default(HOTKEY_DIM), buf, sizeof(buf));
    CHECK(strcmp(buf, "Ctrl+Win+Down") == 0, "default dim");
    Hotkey_Format(Hotkey_Default(HOTKEY_POPUP), buf, sizeof(buf));
    CHECK(strcmp(buf, "Ctrl+Win+B") == 0, "default popup");

    /* A config from 1.1 or older keeps the Ctrl+Alt brightness hotkeys */
    Hotkey_Format(Hotkey_LegacyDefault(HOTKEY_BRIGHTEN), buf, sizeof(buf));
    CHECK(strcmp(buf, "Ctrl+Alt+Up") == 0, "legacy brighten");
    Hotkey_Format(Hotkey_LegacyDefault(HOTKEY_DIM), buf, sizeof(buf));
    CHECK(strcmp(buf, "Ctrl+Alt+Down") == 0, "legacy dim");
    Hotkey_Format(Hotkey_LegacyDefault(HOTKEY_POPUP), buf, sizeof(buf));
    CHECK(strcmp(buf, "Ctrl+Win+B") == 0, "legacy popup is the new default");

    /* Parse: values match the Win32 codes */
    CHECK(Hotkey_Parse("Ctrl+Alt+Up", &hk) && hk.vk == 0x26 &&
          hk.mods == (HK_MOD_CONTROL | HK_MOD_ALT), "parse ctrl alt up");
    CHECK(Hotkey_Parse("Win+Shift+F12", &hk) && hk.vk == 0x7B &&
          hk.mods == (HK_MOD_WIN | HK_MOD_SHIFT), "parse win shift f12");

    /* Case, spaces and modifier order are normalized */
    CHECK(RoundTrip("alt + ctrl + b", "Ctrl+Alt+B"), "normalize order and case");
    CHECK(RoundTrip("Control+Num5", "Ctrl+Num5"), "Control alias and numpad");
    CHECK(RoundTrip("Ctrl+PageDown", "Ctrl+PageDown"), "named key");
    CHECK(RoundTrip("Ctrl+Alt+7", "Ctrl+Alt+7"), "digit key");

    /* Disabled */
    CHECK(Hotkey_Parse("None", &hk) && hk.vk == 0, "None is disabled");
    CHECK(Hotkey_Parse("", &hk) && hk.vk == 0, "empty is disabled");
    CHECK(RoundTrip("none", "None"), "None round trip");

    /* Rejected */
    CHECK(!Hotkey_Parse("Up", &hk), "no modifier rejected");
    CHECK(!Hotkey_Parse("Shift+A", &hk), "shift alone rejected");
    CHECK(!Hotkey_Parse("Ctrl+Ctrl+A", &hk), "repeated modifier rejected");
    CHECK(!Hotkey_Parse("Ctrl+", &hk), "missing key rejected");
    CHECK(!Hotkey_Parse("Ctrl++A", &hk), "empty token rejected");
    CHECK(!Hotkey_Parse("Ctrl+Alt", &hk), "modifier as key rejected");
    CHECK(!Hotkey_Parse("Ctrl+Foo", &hk), "unknown key rejected");
    CHECK(!Hotkey_Parse("Hyper+A", &hk), "unknown modifier rejected");

    /* Key names */
    CHECK(Hotkey_KeyName(0x10) == NULL, "Shift key alone has no name");
    CHECK(Hotkey_KeyName(0xBA) == NULL, "OEM key unsupported");
    CHECK(strcmp(Hotkey_KeyName(0x87), "F24") == 0, "F24 name");

    /* Equality treats every disabled hotkey as the same */
    Hotkey a = { HK_MOD_CONTROL, 0 }, b = { 0, 0 };
    CHECK(Hotkey_Equal(a, b), "disabled hotkeys equal");
    CHECK(!Hotkey_Equal(Hotkey_Default(HOTKEY_DIM), Hotkey_Default(HOTKEY_BRIGHTEN)),
          "different hotkeys differ");

    if (failures == 0)
        printf("All hotkey tests passed\n");
    return failures ? 1 : 0;
}
