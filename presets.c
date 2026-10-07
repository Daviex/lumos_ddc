#include "presets.h"
#include "brightmap.h"
#include <shlobj.h>
#include <stdio.h>

#define REG_RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define APP_NAME    L"Lumos"

/* INI key per hotkey action, in HOTKEY_* order. */
static const WCHAR *const kHotkeyKeys[HOTKEY_COUNT] = {
    L"HotkeyBrighten", L"HotkeyDim", L"HotkeyPopup"
};

static void EnsureDirectory(const WCHAR *path)
{
    CreateDirectoryW(path, NULL);
}

void Settings_Init(Settings *s)
{
    WCHAR appData[MAX_PATH];

    memset(s, 0, sizeof(*s));
    s->step = 5;   /* brightness step for hotkeys and mouse wheel */
    s->autostart = FALSE;

    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appData))) {
        wsprintfW(s->iniPath, L"%s\\" APP_NAME, appData);
        EnsureDirectory(s->iniPath);
        wsprintfW(s->iniPath, L"%s\\" APP_NAME L"\\config.ini", appData);
    } else {
        wcscpy(s->iniPath, L".\\config.ini");
    }

    /* If file doesn't exist, create defaults */
    if (GetFileAttributesW(s->iniPath) == INVALID_FILE_ATTRIBUTES)
        Settings_CreateDefaults(s);

    Settings_Load(s);
}

void Settings_CreateDefaults(Settings *s)
{
    WritePrivateProfileStringW(L"Presets", L"Night", L"30", s->iniPath);
    WritePrivateProfileStringW(L"Presets", L"Day", L"80", s->iniPath);
    WritePrivateProfileStringW(L"Presets", L"Presentation", L"100", s->iniPath);
    WritePrivateProfileStringW(L"Settings", L"Step", L"5", s->iniPath);
    WritePrivateProfileStringW(L"Settings", L"Autostart", L"0", s->iniPath);
    WritePrivateProfileStringW(L"Settings", L"IdleDimEnabled", L"0", s->iniPath);
    WritePrivateProfileStringW(L"Settings", L"IdleDimPercent", L"5", s->iniPath);
    WritePrivateProfileStringW(L"Settings", L"IdleDimMinutes", L"5", s->iniPath);

    /* Written out on a new install, so that a config.ini with no hotkey lines
       is recognizably older than 1.2 (see Settings_Load). */
    for (int i = 0; i < HOTKEY_COUNT; i++) {
        char text[HOTKEY_TEXT_MAX];
        WCHAR textW[HOTKEY_TEXT_MAX];
        Hotkey_Format(Hotkey_Default(i), text, HOTKEY_TEXT_MAX);
        int k = 0;
        for (; text[k]; k++) textW[k] = (WCHAR)(unsigned char)text[k];
        textW[k] = L'\0';
        WritePrivateProfileStringW(L"Settings", kHotkeyKeys[i], textW, s->iniPath);
    }
}

/* Read [Ranges] ("Name=lo,hi"). Without it, convert the [Deltas] offsets of
   older versions, so an upgrade keeps the monitors matched as they were. The
   result reaches the file on the next save. */
static void LoadRanges(Settings *s)
{
    WCHAR buf[4096], val[32];
    s->rangeCount = 0;
    DWORD len = GetPrivateProfileStringW(L"Ranges", NULL, L"", buf, 4096, s->iniPath);
    for (WCHAR *key = buf; len > 0 && *key && s->rangeCount < MAX_MONITORS;
         key += wcslen(key) + 1) {
        int lo = 0, hi = 100;
        GetPrivateProfileStringW(L"Ranges", key, L"0,100", val, 32, s->iniPath);
        if (swscanf(val, L"%d,%d", &lo, &hi) != 2)
            continue;
        BrightMap_Normalize(&lo, &hi);
        int i = s->rangeCount++;
        wcsncpy(s->rangeNames[i], key, 127);
        s->rangeNames[i][127] = L'\0';
        s->rangeLo[i] = lo;
        s->rangeHi[i] = hi;
        s->rangeConnected[i] = FALSE;
    }
    if (s->rangeCount > 0)
        return;

    int offsets[MAX_MONITORS];
    len = GetPrivateProfileStringW(L"Deltas", NULL, L"", buf, 4096, s->iniPath);
    for (WCHAR *key = buf; len > 0 && *key && s->rangeCount < MAX_MONITORS;
         key += wcslen(key) + 1) {
        GetPrivateProfileStringW(L"Deltas", key, L"0", val, 32, s->iniPath);
        int i = s->rangeCount++;
        wcsncpy(s->rangeNames[i], key, 127);
        s->rangeNames[i][127] = L'\0';
        offsets[i] = _wtoi(val);
        s->rangeConnected[i] = FALSE;
    }
    BrightMap_FromOffsets(offsets, s->rangeCount, s->rangeLo, s->rangeHi);
}

void Settings_Load(Settings *s)
{
    WCHAR buf[4096];
    WCHAR val[16];

    s->presetCount = 0;

    /* Load presets */
    DWORD len = GetPrivateProfileStringW(L"Presets", NULL, L"", buf, 4096, s->iniPath);
    if (len > 0) {
        WCHAR *key = buf;
        while (*key && s->presetCount < MAX_PRESETS) {
            GetPrivateProfileStringW(L"Presets", key, L"50", val, 16, s->iniPath);
            Preset *p = &s->presets[s->presetCount];
            wcsncpy(p->name, key, MAX_PRESET_NAME - 1);
            p->name[MAX_PRESET_NAME - 1] = L'\0';
            p->brightness = (DWORD)_wtoi(val);
            if (p->brightness > 100) p->brightness = 100;
            s->presetCount++;
            key += wcslen(key) + 1;
        }
    }

    /* Load settings */
    s->step = (int)GetPrivateProfileIntW(L"Settings", L"Step", 5, s->iniPath);
    if (s->step < 1) s->step = 1;
    if (s->step > 50) s->step = 50;

    s->autostart = (BOOL)GetPrivateProfileIntW(L"Settings", L"Autostart", 0, s->iniPath);

    /* Idle auto-dim */
    s->idleDimEnabled = (BOOL)GetPrivateProfileIntW(L"Settings", L"IdleDimEnabled", 0, s->iniPath);
    s->idleDimPercent = (int)GetPrivateProfileIntW(L"Settings", L"IdleDimPercent", 5, s->iniPath);
    if (s->idleDimPercent < 0)   s->idleDimPercent = 0;
    if (s->idleDimPercent > 100) s->idleDimPercent = 100;
    s->idleDimMinutes = (int)GetPrivateProfileIntW(L"Settings", L"IdleDimMinutes", 5, s->iniPath);
    /* One minute floor so the dim cannot fire while the user is still reading,
       one day ceiling because anything longer never triggers in practice. */
    if (s->idleDimMinutes < 1)    s->idleDimMinutes = 1;
    if (s->idleDimMinutes > 1440) s->idleDimMinutes = 1440;

    /* Hotkeys are stored as text ("Ctrl+Win+Up"). A missing line means the
       file predates configurable hotkeys, because a new install writes all of
       them: such a file keeps the Ctrl+Alt combinations it has always had. An
       unreadable value falls back to the default, so a typo cannot leave the
       user without a way to change the brightness from the keyboard. */
    for (int i = 0; i < HOTKEY_COUNT; i++) {
        WCHAR textW[HOTKEY_TEXT_MAX];
        char text[HOTKEY_TEXT_MAX];
        s->hotkeys[i] = Hotkey_Default(i);
        GetPrivateProfileStringW(L"Settings", kHotkeyKeys[i], L"\x01", textW,
                                 HOTKEY_TEXT_MAX, s->iniPath);
        if (textW[0] == 0x01) {                 /* no such line */
            s->hotkeys[i] = Hotkey_LegacyDefault(i);
            continue;
        }
        int k = 0;
        for (; textW[k] && k < HOTKEY_TEXT_MAX - 1; k++)
            text[k] = (textW[k] < 0x80) ? (char)textW[k] : '?';
        text[k] = '\0';
        Hotkey hk;
        if (Hotkey_Parse(text, &hk))
            s->hotkeys[i] = hk;
    }

    LoadRanges(s);

    /* Load schedule enabled flag */
    s->scheduleEnabled = (BOOL)GetPrivateProfileIntW(L"Settings", L"ScheduleEnabled", 0, s->iniPath);

    /* Load schedule points: keys are "HH:MM", values are 0-100 */
    s->scheduleCount = 0;
    len = GetPrivateProfileStringW(L"Schedule", NULL, L"", buf, 4096, s->iniPath);
    if (len > 0) {
        WCHAR *key = buf;
        while (*key && s->scheduleCount < MAX_SCHEDULE) {
            char keyA[8];
            /* keys are ASCII "HH:MM"; narrow-copy safely */
            int k = 0;
            for (; key[k] && k < 7; k++) keyA[k] = (char)key[k];
            keyA[k] = '\0';
            int mins = Schedule_ParseKeyTime(keyA);
            if (mins >= 0) {
                GetPrivateProfileStringW(L"Schedule", key, L"50", val, 16, s->iniPath);
                int b = _wtoi(val);
                if (b < 0) b = 0;
                if (b > 100) b = 100;
                s->schedule[s->scheduleCount].minutes = mins;
                s->schedule[s->scheduleCount].brightness = b;
                s->scheduleCount++;
            }
            key += wcslen(key) + 1;
        }
        Schedule_Sort(s->schedule, s->scheduleCount);
    }
}

void Settings_Save(Settings *s)
{
    WCHAR val[16];

    /* Clear presets section and rewrite */
    WritePrivateProfileSectionW(L"Presets", L"", s->iniPath);
    for (int i = 0; i < s->presetCount; i++) {
        wsprintfW(val, L"%u", s->presets[i].brightness);
        WritePrivateProfileStringW(L"Presets", s->presets[i].name, val, s->iniPath);
    }

    wsprintfW(val, L"%d", s->step);
    WritePrivateProfileStringW(L"Settings", L"Step", val, s->iniPath);

    wsprintfW(val, L"%d", s->autostart ? 1 : 0);
    WritePrivateProfileStringW(L"Settings", L"Autostart", val, s->iniPath);

    WritePrivateProfileStringW(L"Settings", L"IdleDimEnabled",
                               s->idleDimEnabled ? L"1" : L"0", s->iniPath);
    wsprintfW(val, L"%d", s->idleDimPercent);
    WritePrivateProfileStringW(L"Settings", L"IdleDimPercent", val, s->iniPath);
    wsprintfW(val, L"%d", s->idleDimMinutes);
    WritePrivateProfileStringW(L"Settings", L"IdleDimMinutes", val, s->iniPath);

    for (int i = 0; i < HOTKEY_COUNT; i++) {
        char text[HOTKEY_TEXT_MAX];
        WCHAR textW[HOTKEY_TEXT_MAX];
        Hotkey_Format(s->hotkeys[i], text, HOTKEY_TEXT_MAX);
        int k = 0;
        for (; text[k]; k++) textW[k] = (WCHAR)(unsigned char)text[k];
        textW[k] = L'\0';
        WritePrivateProfileStringW(L"Settings", kHotkeyKeys[i], textW, s->iniPath);
    }

    /* Rewrite [Ranges]. [Deltas] from older versions is left as it was, so
       going back to an older build still finds its offsets. */
    WritePrivateProfileSectionW(L"Ranges", L"", s->iniPath);
    for (int i = 0; i < s->rangeCount; i++) {
        wsprintfW(val, L"%d,%d", s->rangeLo[i], s->rangeHi[i]);
        WritePrivateProfileStringW(L"Ranges", s->rangeNames[i], val, s->iniPath);
    }

    /* Save schedule enabled flag */
    WritePrivateProfileStringW(L"Settings", L"ScheduleEnabled",
                               s->scheduleEnabled ? L"1" : L"0", s->iniPath);

    /* Rewrite the entire [Schedule] section (clears removed points).
       Build a double-null-terminated "HH:MM=NN\0...\0\0" buffer. */
    {
        WCHAR section[MAX_SCHEDULE * 16 + 2];
        int pos = 0;
        for (int i = 0; i < s->scheduleCount; i++) {
            int h = s->schedule[i].minutes / 60;
            int m = s->schedule[i].minutes % 60;
            pos += wsprintfW(section + pos, L"%02d:%02d=%d",
                             h, m, s->schedule[i].brightness);
            section[pos++] = L'\0';
        }
        section[pos] = L'\0';  /* final terminator */
        WritePrivateProfileSectionW(L"Schedule", section, s->iniPath);
    }
}

void Settings_SetAutostart(BOOL enable)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            WCHAR exePath[MAX_PATH];
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            RegSetValueExW(hKey, APP_NAME, 0, REG_SZ,
                           (BYTE *)exePath, (DWORD)((wcslen(exePath) + 1) * sizeof(WCHAR)));
        } else {
            RegDeleteValueW(hKey, APP_NAME);
        }
        RegCloseKey(hKey);
    }
}

BOOL Settings_GetAutostart(void)
{
    HKEY hKey;
    BOOL result = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        result = (RegQueryValueExW(hKey, APP_NAME, NULL, NULL, NULL, NULL) == ERROR_SUCCESS);
        RegCloseKey(hKey);
    }
    return result;
}

/* The entry for this monitor name, added with the full range when missing.
   Returns -1 when the table is full. */
static int RangeEntry(Settings *s, const WCHAR *name)
{
    for (int i = 0; i < s->rangeCount; i++)
        if (wcscmp(s->rangeNames[i], name) == 0)
            return i;
    if (s->rangeCount >= MAX_MONITORS)
        return -1;
    int i = s->rangeCount++;
    wcsncpy(s->rangeNames[i], name, 127);
    s->rangeNames[i][127] = L'\0';
    s->rangeLo[i] = 0;
    s->rangeHi[i] = 100;
    s->rangeConnected[i] = FALSE;
    return i;
}

/* Only a monitor that can be set, or is expected to answer again, has a range
   worth keeping; the "No DDC/CI monitors found" stand-in does not. */
static BOOL HasRange(const BrightMonitor *mon)
{
    return mon->controllable || mon->awaitingAnswer;
}

void Settings_ApplyRanges(Settings *s, MonitorList *ml)
{
    for (int i = 0; i < s->rangeCount; i++)
        s->rangeConnected[i] = FALSE;
    for (int i = 0; i < ml->count; i++) {
        BrightMonitor *mon = &ml->monitors[i];
        mon->rangeLo = 0;
        mon->rangeHi = 100;
        int e = HasRange(mon) ? RangeEntry(s, mon->name) : -1;
        if (e < 0)
            continue;
        mon->rangeLo = s->rangeLo[e];
        mon->rangeHi = s->rangeHi[e];
        s->rangeConnected[e] = TRUE;
    }
}

void Settings_StoreRanges(Settings *s, const MonitorList *ml)
{
    for (int i = 0; i < ml->count; i++) {
        const BrightMonitor *mon = &ml->monitors[i];
        int e = HasRange(mon) ? RangeEntry(s, mon->name) : -1;
        if (e < 0)
            continue;
        s->rangeLo[e] = mon->rangeLo;
        s->rangeHi[e] = mon->rangeHi;
    }
}
