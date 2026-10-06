#include "presets.h"
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

    /* Load deltas */
    s->deltaCount = 0;
    len = GetPrivateProfileStringW(L"Deltas", NULL, L"", buf, 4096, s->iniPath);
    if (len > 0) {
        WCHAR *key = buf;
        while (*key && s->deltaCount < MAX_MONITORS) {
            GetPrivateProfileStringW(L"Deltas", key, L"0", val, 16, s->iniPath);
            int idx = s->deltaCount;
            wcsncpy(s->deltaNames[idx], key, 127);
            s->deltaNames[idx][127] = L'\0';
            s->deltaValues[idx] = _wtoi(val);
            if (s->deltaValues[idx] < -40) s->deltaValues[idx] = -40;
            if (s->deltaValues[idx] > 40) s->deltaValues[idx] = 40;
            s->deltaCount++;
            key += wcslen(key) + 1;
        }
    }

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

    /* Save deltas */
    WritePrivateProfileSectionW(L"Deltas", L"", s->iniPath);
    for (int i = 0; i < s->deltaCount; i++) {
        if (s->deltaValues[i] != 0) {
            wsprintfW(val, L"%d", s->deltaValues[i]);
            WritePrivateProfileStringW(L"Deltas", s->deltaNames[i], val, s->iniPath);
        }
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

void Settings_LoadDeltas(Settings *s, MonitorList *ml)
{
    for (int i = 0; i < ml->count; i++) {
        ml->monitors[i].delta = 0;
        for (int j = 0; j < s->deltaCount; j++) {
            if (wcscmp(ml->monitors[i].name, s->deltaNames[j]) == 0) {
                ml->monitors[i].delta = s->deltaValues[j];
                break;
            }
        }
    }
}

void Settings_SaveDeltas(Settings *s, MonitorList *ml)
{
    s->deltaCount = 0;
    for (int i = 0; i < ml->count && s->deltaCount < MAX_MONITORS; i++) {
        int idx = s->deltaCount;
        wcsncpy(s->deltaNames[idx], ml->monitors[i].name, 127);
        s->deltaNames[idx][127] = L'\0';
        s->deltaValues[idx] = ml->monitors[i].delta;
        s->deltaCount++;
    }
}
