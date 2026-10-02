#include "presets.h"
#include <shlobj.h>
#include <strsafe.h>
#include <stdio.h>

#define REG_RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define APP_NAME    L"Lumos"
#define AUTOSTART_COMMAND_CAPACITY (MAX_PATH + 2 + ARRAYSIZE(L" " LUMOS_STARTUP_ARGUMENT) - 1)
#define MONITOR_SELECTION_SECTION L"MonitorSelection"
#define MONITOR_INPUT_SECTION L"MonitorInputs"

static void EnsureDirectory(const WCHAR *path)
{
    CreateDirectoryW(path, NULL);
}

void Settings_Init(Settings *s)
{
    WCHAR appData[MAX_PATH];
    WCHAR settingsDir[MAX_PATH];

    memset(s, 0, sizeof(*s));
    s->step = 5;   /* brightness step for hotkeys and mouse wheel */
    s->autostart = FALSE;

    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appData)) &&
        SUCCEEDED(StringCchPrintfW(settingsDir, ARRAYSIZE(settingsDir),
                                  L"%s\\" APP_NAME, appData)) &&
        SUCCEEDED(StringCchPrintfW(s->iniPath, ARRAYSIZE(s->iniPath),
                                  L"%s\\config.ini", settingsDir))) {
        EnsureDirectory(settingsDir);
    } else {
        /* Preserve the local fallback if AppData or the complete path does
           not fit; never read or write an INI at a truncated path. */
        StringCchCopyW(s->iniPath, ARRAYSIZE(s->iniPath), L".\\config.ini");
    }

    /* If file doesn't exist, create defaults */
    if (GetFileAttributesW(s->iniPath) == INVALID_FILE_ATTRIBUTES)
        Settings_CreateDefaults(s);

    Settings_Load(s);
}

void Settings_CreateDefaults(Settings *s)
{
    WCHAR dayBrightness[4];
    StringCchPrintfW(dayBrightness, ARRAYSIZE(dayBrightness), L"%d", DEFAULT_DAY_BRIGHTNESS);
    WritePrivateProfileStringW(L"Presets", L"Night", L"30", s->iniPath);
    WritePrivateProfileStringW(L"Presets", L"Day", dayBrightness, s->iniPath);
    WritePrivateProfileStringW(L"Presets", L"Presentation", L"100", s->iniPath);
    WritePrivateProfileStringW(L"Settings", L"Step", L"5", s->iniPath);
    WritePrivateProfileStringW(L"Settings", L"Autostart", L"0", s->iniPath);
    WritePrivateProfileStringW(L"Settings", L"IdleDimEnabled", L"0", s->iniPath);
    WritePrivateProfileStringW(L"Settings", L"IdleDimPercent", L"5", s->iniPath);
    WritePrivateProfileStringW(L"Settings", L"IdleDimMinutes", L"5", s->iniPath);
    WritePrivateProfileStringW(MONITOR_SELECTION_SECTION, L"Mode", L"All", s->iniPath);
    WritePrivateProfileStringW(MONITOR_SELECTION_SECTION, L"Count", L"0", s->iniPath);
    WritePrivateProfileStringW(MONITOR_INPUT_SECTION, L"Count", L"0", s->iniPath);
}

int Settings_DayBrightness(const Settings *s)
{
    int dayBrightness = -1;
    if (s) {
        for (int i = 0; i < s->presetCount && i < MAX_PRESETS; i++) {
            const Preset *preset = &s->presets[i];
            int brightness = preset->brightness > 100 ? 100 : (int)preset->brightness;
            if (_wcsicmp(preset->name, L"Giorno") == 0) return brightness;
            if (dayBrightness < 0 && _wcsicmp(preset->name, L"Day") == 0)
                dayBrightness = brightness;
        }
    }
    return dayBrightness >= 0 ? dayBrightness : DEFAULT_DAY_BRIGHTNESS;
}

static void LoadMonitorSelection(Settings *s)
{
    MonitorSelection *selection = &s->monitorSelection;
    WCHAR mode[16];
    memset(selection, 0, sizeof(*selection));
    GetPrivateProfileStringW(MONITOR_SELECTION_SECTION, L"Mode", L"All",
                             mode, ARRAYSIZE(mode), s->iniPath);
    if (_wcsicmp(mode, L"All") == 0) selection->selectedOnly = FALSE;
    else {
        selection->selectedOnly = TRUE;
        /* An invalid explicit mode fails closed, rather than controlling all. */
        if (_wcsicmp(mode, L"Selected") != 0) return;
    }

    int count = (int)GetPrivateProfileIntW(MONITOR_SELECTION_SECTION, L"Count", 0, s->iniPath);
    if (count < 0) count = 0;
    if (count > MAX_MONITORS) count = MAX_MONITORS;
    for (int i = 0; i < count; i++) {
        WCHAR field[16], key[MONITOR_SELECTION_KEY_LEN + 1];
        StringCchPrintfW(field, ARRAYSIZE(field), L"Key%d", i);
        DWORD length = GetPrivateProfileStringW(MONITOR_SELECTION_SECTION, field, L"",
                                                key, ARRAYSIZE(key), s->iniPath);
        /* A one-character larger read buffer distinguishes a valid maximum
           length key from a truncated identity; never accept a shortened ID. */
        if (length >= MONITOR_SELECTION_KEY_LEN || !Settings_MonitorKeyValid(key)) continue;
        BOOL duplicate = FALSE;
        for (int j = 0; j < selection->count; j++)
            if (_wcsicmp(selection->keys[j], key) == 0) duplicate = TRUE;
        if (duplicate) continue;
        int index = selection->count++;
        StringCchCopyW(selection->keys[index], MONITOR_SELECTION_KEY_LEN, key);
        StringCchPrintfW(field, ARRAYSIZE(field), L"Name%d", i);
        GetPrivateProfileStringW(MONITOR_SELECTION_SECTION, field, L"",
                                 selection->names[index], ARRAYSIZE(selection->names[index]), s->iniPath);
    }
}

static void SaveMonitorSelection(const Settings *s)
{
    const MonitorSelection *selection = &s->monitorSelection;
    int savedIndices[MAX_MONITORS], count = 0;
    WCHAR field[16], value[16];
    WritePrivateProfileSectionW(MONITOR_SELECTION_SECTION, L"", s->iniPath);
    WritePrivateProfileStringW(MONITOR_SELECTION_SECTION, L"Mode",
                               selection->selectedOnly ? L"Selected" : L"All", s->iniPath);
    for (int i = 0; i < selection->count && i < MAX_MONITORS; i++) {
        if (!Settings_MonitorKeyValid(selection->keys[i])) continue;
        BOOL duplicate = FALSE;
        for (int j = 0; j < count; j++)
            if (_wcsicmp(selection->keys[savedIndices[j]], selection->keys[i]) == 0) duplicate = TRUE;
        if (duplicate) continue;
        StringCchPrintfW(field, ARRAYSIZE(field), L"Key%d", count);
        WritePrivateProfileStringW(MONITOR_SELECTION_SECTION, field, selection->keys[i], s->iniPath);
        StringCchPrintfW(field, ARRAYSIZE(field), L"Name%d", count);
        WritePrivateProfileStringW(MONITOR_SELECTION_SECTION, field, selection->names[i], s->iniPath);
        savedIndices[count++] = i;
    }
    StringCchPrintfW(value, ARRAYSIZE(value), L"%d", count);
    WritePrivateProfileStringW(MONITOR_SELECTION_SECTION, L"Count", value, s->iniPath);
}

static BOOL ParseMonitorInput(const WCHAR *value, DWORD *input)
{
    DWORD parsed = 0;
    if (!value[0]) return FALSE;
    for (size_t i = 0; value[i]; i++) {
        if (value[i] < L'0' || value[i] > L'9') return FALSE;
        parsed = parsed * 10 + (DWORD)(value[i] - L'0');
        if (parsed > 255) return FALSE;
    }
    *input = parsed;
    return TRUE;
}

static void AddMonitorInputRule(MonitorInputRule *rules, int *count,
                                const MonitorInputRule *rule)
{
    if (!Settings_MonitorKeyValid(rule->key) || _wcsnicmp(rule->key, L"DDC:", 4) != 0)
        return;
    for (int i = 0; i < *count; i++) {
        if (_wcsicmp(rules[i].key, rule->key) == 0) {
            /* Conflicting or duplicate identities need explicit reconfiguration.
               A later disabled record must not bypass an earlier enabled filter. */
            rules[i].enabled = rules[i].enabled || rule->enabled;
            rules[i].input = 0;
            return;
        }
    }
    if (*count >= MAX_MONITORS) return;
    rules[*count] = *rule;
    rules[*count].enabled = rule->enabled != FALSE;
    if (rules[*count].input > 255) rules[*count].input = 0;
    (*count)++;
}

static void LoadMonitorInputs(Settings *s)
{
    MonitorSelection *selection = &s->monitorSelection;
    selection->inputRuleCount = 0;
    int count = (int)GetPrivateProfileIntW(MONITOR_INPUT_SECTION, L"Count", 0, s->iniPath);
    if (count < 0) count = 0;
    if (count > MAX_MONITORS) count = MAX_MONITORS;
    for (int i = 0; i < count; i++) {
        MonitorInputRule rule = { 0 };
        WCHAR field[16], key[MONITOR_SELECTION_KEY_LEN + 1], value[16];
        StringCchPrintfW(field, ARRAYSIZE(field), L"Key%d", i);
        DWORD length = GetPrivateProfileStringW(MONITOR_INPUT_SECTION, field, L"",
                                                key, ARRAYSIZE(key), s->iniPath);
        if (length >= MONITOR_SELECTION_KEY_LEN || !Settings_MonitorKeyValid(key)) continue;
        StringCchCopyW(rule.key, ARRAYSIZE(rule.key), key);
        StringCchPrintfW(field, ARRAYSIZE(field), L"Name%d", i);
        GetPrivateProfileStringW(MONITOR_INPUT_SECTION, field, L"",
                                 rule.name, ARRAYSIZE(rule.name), s->iniPath);
        StringCchPrintfW(field, ARRAYSIZE(field), L"Enabled%d", i);
        GetPrivateProfileStringW(MONITOR_INPUT_SECTION, field, L"",
                                 value, ARRAYSIZE(value), s->iniPath);
        BOOL validEnabled = wcscmp(value, L"0") == 0 || wcscmp(value, L"1") == 0;
        /* An existing rule with a missing/corrupt flag stays protected. */
        rule.enabled = wcscmp(value, L"0") != 0;
        StringCchPrintfW(field, ARRAYSIZE(field), L"Input%d", i);
        length = GetPrivateProfileStringW(MONITOR_INPUT_SECTION, field, L"",
                                          value, ARRAYSIZE(value), s->iniPath);
        if (!validEnabled || length >= ARRAYSIZE(value) - 1 ||
            !ParseMonitorInput(value, &rule.input)) rule.input = 0;
        AddMonitorInputRule(selection->inputRules, &selection->inputRuleCount, &rule);
    }
}

static void SaveMonitorInputs(const Settings *s)
{
    const MonitorSelection *selection = &s->monitorSelection;
    MonitorInputRule rules[MAX_MONITORS];
    int count = 0;
    WCHAR field[16], value[16];
    for (int i = 0; i < selection->inputRuleCount && i < MAX_MONITORS; i++)
        AddMonitorInputRule(rules, &count, &selection->inputRules[i]);
    WritePrivateProfileSectionW(MONITOR_INPUT_SECTION, L"", s->iniPath);
    for (int i = 0; i < count; i++) {
        StringCchPrintfW(field, ARRAYSIZE(field), L"Key%d", i);
        WritePrivateProfileStringW(MONITOR_INPUT_SECTION, field, rules[i].key, s->iniPath);
        StringCchPrintfW(field, ARRAYSIZE(field), L"Name%d", i);
        WritePrivateProfileStringW(MONITOR_INPUT_SECTION, field, rules[i].name, s->iniPath);
        StringCchPrintfW(field, ARRAYSIZE(field), L"Enabled%d", i);
        WritePrivateProfileStringW(MONITOR_INPUT_SECTION, field,
                                   rules[i].enabled ? L"1" : L"0", s->iniPath);
        StringCchPrintfW(field, ARRAYSIZE(field), L"Input%d", i);
        StringCchPrintfW(value, ARRAYSIZE(value), L"%lu", (unsigned long)rules[i].input);
        WritePrivateProfileStringW(MONITOR_INPUT_SECTION, field, value, s->iniPath);
    }
    StringCchPrintfW(value, ARRAYSIZE(value), L"%d", count);
    WritePrivateProfileStringW(MONITOR_INPUT_SECTION, L"Count", value, s->iniPath);
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
    LoadMonitorSelection(s);
    LoadMonitorInputs(s);
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
    SaveMonitorSelection(s);
    SaveMonitorInputs(s);
}

static BOOL GetAutostartExecutable(WCHAR exePath[MAX_PATH])
{
    DWORD length = GetModuleFileNameW(NULL, exePath, MAX_PATH);
    return length > 0 && length < MAX_PATH;
}

static BOOL FormatAutostartCommand(const WCHAR *exePath,
                                  WCHAR command[AUTOSTART_COMMAND_CAPACITY])
{
    return SUCCEEDED(StringCchPrintfW(command, AUTOSTART_COMMAND_CAPACITY,
                                      L"\"%s\" " LUMOS_STARTUP_ARGUMENT, exePath));
}

BOOL Settings_SetAutostart(BOOL enable)
{
    HKEY hKey;
    LONG status;
    WCHAR command[AUTOSTART_COMMAND_CAPACITY];

    if (enable) {
        WCHAR exePath[MAX_PATH];
        if (!GetAutostartExecutable(exePath) || !FormatAutostartCommand(exePath, command))
            return FALSE;

        status = RegCreateKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, NULL, 0,
                                 KEY_SET_VALUE, NULL, &hKey, NULL);
    } else {
        status = RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0,
                               KEY_SET_VALUE, &hKey);
        if (status == ERROR_FILE_NOT_FOUND) return TRUE;
    }
    if (status != ERROR_SUCCESS) return FALSE;

    if (enable) {
        status = RegSetValueExW(hKey, APP_NAME, 0, REG_SZ,
                               (const BYTE *)command,
                               (DWORD)((wcslen(command) + 1) * sizeof(WCHAR)));
    } else {
        status = RegDeleteValueW(hKey, APP_NAME);
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(hKey);
    return status == ERROR_SUCCESS;
}

BOOL Settings_UpgradeAutostart(void)
{
    HKEY hKey;
    LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0,
                                KEY_QUERY_VALUE | KEY_SET_VALUE, &hKey);
    if (status == ERROR_FILE_NOT_FOUND) return TRUE;
    if (status != ERROR_SUCCESS) return FALSE;

    WCHAR existing[AUTOSTART_COMMAND_CAPACITY];
    DWORD bytes = sizeof(existing), type = 0;
    BOOL success = FALSE;
    status = RegQueryValueExW(hKey, APP_NAME, NULL, &type, (BYTE *)existing, &bytes);
    if (status == ERROR_FILE_NOT_FOUND) {
        success = TRUE;
    } else if (status == ERROR_SUCCESS && type == REG_SZ &&
               bytes >= sizeof(WCHAR) && bytes <= sizeof(existing) &&
               bytes % sizeof(WCHAR) == 0) {
        size_t length;
        size_t characters = bytes / sizeof(WCHAR);
        /* The registry does not guarantee termination. Reject truncated or
           embedded-null data before comparing any command line. */
        if (SUCCEEDED(StringCchLengthW(existing, characters, &length)) &&
            length == characters - 1) {
            WCHAR exePath[MAX_PATH];
            WCHAR quotedPath[MAX_PATH + 2];
            if (GetAutostartExecutable(exePath) &&
                SUCCEEDED(StringCchPrintfW(quotedPath, ARRAYSIZE(quotedPath),
                                           L"\"%s\"", exePath))) {
                success = TRUE;
                if (_wcsicmp(existing, exePath) == 0 || _wcsicmp(existing, quotedPath) == 0) {
                    WCHAR command[AUTOSTART_COMMAND_CAPACITY];
                    success = FormatAutostartCommand(exePath, command) &&
                        RegSetValueExW(hKey, APP_NAME, 0, REG_SZ, (const BYTE *)command,
                                        (DWORD)((wcslen(command) + 1) * sizeof(WCHAR))) == ERROR_SUCCESS;
                }
            }
        }
    }
    RegCloseKey(hKey);
    return success;
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
