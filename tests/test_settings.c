/* Compile the real settings implementation with all external I/O replaced.
   This executable never touches the registry or writes a configuration file. */
#include <windows.h>
#include <shlobj.h>
#include <strsafe.h>
#include <stdio.h>
#include <string.h>
#include "../presets.h"

static int failures;
#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("FAIL line %d: %s\n", __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static struct {
    WCHAR appData[MAX_PATH];
    HRESULT folderStatus;
    WCHAR exePath[MAX_PATH + 1];
    DWORD exeLength;
    LONG createStatus, openStatus, setStatus, deleteStatus, queryStatus;
    int directoryCalls, createCalls, openCalls, setCalls, deleteCalls, closeCalls, queryCalls;
    WCHAR directory[MAX_PATH];
    WCHAR iniWritePath[MAX_PATH];
    WCHAR defaultDayValue[4];
    WCHAR command[MAX_PATH + 12];
    DWORD commandBytes;
    WCHAR queryValue[MAX_PATH + 64];
    DWORD queryType, queryBytes;
    struct {
        WCHAR field[24];
        WCHAR value[MONITOR_SELECTION_KEY_LEN + 32];
    } selectionValues[MAX_MONITORS * 2 + 8];
    int selectionValueCount;
} mock;

static void ResetMocks(void)
{
    memset(&mock, 0, sizeof(mock));
    StringCchCopyW(mock.appData, ARRAYSIZE(mock.appData), L"C:\\Users\\Lumos Test\\AppData\\Roaming");
    StringCchCopyW(mock.exePath, ARRAYSIZE(mock.exePath), L"C:\\Program Files\\Lumos\\lumos.exe");
    mock.exeLength = (DWORD)wcslen(mock.exePath);
    StringCchPrintfW(mock.queryValue, ARRAYSIZE(mock.queryValue),
                     L"\"%s\" " LUMOS_STARTUP_ARGUMENT, mock.exePath);
    mock.queryType = REG_SZ;
    mock.queryBytes = (DWORD)((wcslen(mock.queryValue) + 1) * sizeof(WCHAR));
}

static HRESULT WINAPI MockSHGetFolderPathW(HWND window, int folder, HANDLE token,
                                          DWORD flags, LPWSTR path)
{
    (void)window; (void)folder; (void)token; (void)flags;
    if (SUCCEEDED(mock.folderStatus))
        StringCchCopyW(path, MAX_PATH, mock.appData);
    return mock.folderStatus;
}

static BOOL WINAPI MockCreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES security)
{
    (void)security;
    mock.directoryCalls++;
    CHECK(SUCCEEDED(StringCchCopyW(mock.directory, ARRAYSIZE(mock.directory), path)));
    return TRUE;
}

static DWORD WINAPI MockGetFileAttributesW(LPCWSTR path)
{
    (void)path;
    return INVALID_FILE_ATTRIBUTES;  /* Exercise default INI creation too. */
}

static const WCHAR *SelectionIniValue(const WCHAR *field)
{
    for (int i = 0; i < mock.selectionValueCount; i++)
        if (_wcsicmp(mock.selectionValues[i].field, field) == 0)
            return mock.selectionValues[i].value;
    return NULL;
}

static void PutSelectionIni(const WCHAR *field, const WCHAR *value)
{
    int index = 0;
    for (; index < mock.selectionValueCount; index++)
        if (_wcsicmp(mock.selectionValues[index].field, field) == 0) break;
    CHECK(index < (int)ARRAYSIZE(mock.selectionValues));
    if (index >= (int)ARRAYSIZE(mock.selectionValues)) return;
    if (index == mock.selectionValueCount) mock.selectionValueCount++;
    CHECK(SUCCEEDED(StringCchCopyW(mock.selectionValues[index].field,
                                   ARRAYSIZE(mock.selectionValues[index].field), field)));
    CHECK(SUCCEEDED(StringCchCopyW(mock.selectionValues[index].value,
                                   ARRAYSIZE(mock.selectionValues[index].value), value)));
}

static BOOL WINAPI MockWritePrivateProfileStringW(LPCWSTR section, LPCWSTR key,
                                                 LPCWSTR value, LPCWSTR path)
{
    if (section && key && value && wcscmp(section, L"Presets") == 0 &&
        wcscmp(key, L"Day") == 0)
        CHECK(SUCCEEDED(StringCchCopyW(mock.defaultDayValue, ARRAYSIZE(mock.defaultDayValue), value)));
    if (section && key && value && wcscmp(section, L"MonitorSelection") == 0)
        PutSelectionIni(key, value);
    CHECK(SUCCEEDED(StringCchCopyW(mock.iniWritePath, ARRAYSIZE(mock.iniWritePath), path)));
    return TRUE;
}

static BOOL WINAPI MockWritePrivateProfileSectionW(LPCWSTR section, LPCWSTR values,
                                                  LPCWSTR path)
{
    (void)path;
    if (wcscmp(section, L"MonitorSelection") == 0) {
        CHECK(values[0] == L'\0');
        mock.selectionValueCount = 0;
    }
    return TRUE;
}

static DWORD WINAPI MockGetPrivateProfileStringW(LPCWSTR section, LPCWSTR key,
                                                LPCWSTR fallback, LPWSTR buffer,
                                                DWORD capacity, LPCWSTR path)
{
    (void)path;
    if (!capacity) return 0;
    if (!key) { buffer[0] = L'\0'; return 0; }
    const WCHAR *value = wcscmp(section, L"MonitorSelection") == 0 ? SelectionIniValue(key) : NULL;
    if (!value) value = fallback;
    size_t length = wcslen(value);
    if (length >= capacity) length = capacity - 1;
    memcpy(buffer, value, length * sizeof(WCHAR));
    buffer[length] = L'\0';
    return (DWORD)length;
}

static UINT WINAPI MockGetPrivateProfileIntW(LPCWSTR section, LPCWSTR key,
                                            INT fallback, LPCWSTR path)
{
    (void)path;
    const WCHAR *value = wcscmp(section, L"MonitorSelection") == 0 ? SelectionIniValue(key) : NULL;
    if (value) return (UINT)_wtoi(value);
    return (UINT)fallback;
}

static DWORD WINAPI MockGetModuleFileNameW(HMODULE module, LPWSTR path, DWORD capacity)
{
    DWORD copied = mock.exeLength < capacity ? mock.exeLength : capacity;
    (void)module;
    CHECK(capacity == MAX_PATH);
    memcpy(path, mock.exePath, copied * sizeof(WCHAR));
    if (copied < capacity) path[copied] = L'\0';
    /* Deliberately leave a truncated output unterminated: it must not be read. */
    return copied;
}

static LONG WINAPI MockRegCreateKeyExW(HKEY root, LPCWSTR key, DWORD reserved,
                                      LPWSTR className, DWORD options, REGSAM access,
                                      const LPSECURITY_ATTRIBUTES security,
                                      PHKEY handle, LPDWORD disposition)
{
    (void)reserved; (void)className; (void)options; (void)security; (void)disposition;
    CHECK(root == HKEY_CURRENT_USER);
    CHECK(wcscmp(key, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run") == 0);
    CHECK(access == KEY_SET_VALUE);
    mock.createCalls++;
    *handle = (HKEY)(UINT_PTR)1;
    return mock.createStatus;
}

static LONG WINAPI MockRegOpenKeyExW(HKEY root, LPCWSTR key, DWORD options,
                                    REGSAM access, PHKEY handle)
{
    (void)root; (void)key; (void)options; (void)access;
    mock.openCalls++;
    *handle = (HKEY)(UINT_PTR)1;
    return mock.openStatus;
}

static LONG WINAPI MockRegSetValueExW(HKEY handle, LPCWSTR name, DWORD reserved,
                                     DWORD type, const BYTE *value, DWORD bytes)
{
    const WCHAR *command = (const WCHAR *)value;
    (void)handle; (void)reserved;
    mock.setCalls++;
    CHECK(wcscmp(name, L"Lumos") == 0);
    CHECK(type == REG_SZ);
    CHECK(bytes == (wcslen(command) + 1) * sizeof(WCHAR));
    mock.commandBytes = bytes;
    CHECK(SUCCEEDED(StringCchCopyW(mock.command, ARRAYSIZE(mock.command), command)));
    return mock.setStatus;
}

static LONG WINAPI MockRegDeleteValueW(HKEY handle, LPCWSTR name)
{
    (void)handle;
    CHECK(wcscmp(name, L"Lumos") == 0);
    mock.deleteCalls++;
    return mock.deleteStatus;
}

static LONG WINAPI MockRegCloseKey(HKEY handle)
{
    (void)handle;
    mock.closeCalls++;
    return ERROR_SUCCESS;
}

static LONG WINAPI MockRegQueryValueExW(HKEY handle, LPCWSTR name, LPDWORD reserved,
                                       LPDWORD type, LPBYTE value, LPDWORD bytes)
{
    (void)handle; (void)reserved;
    CHECK(wcscmp(name, L"Lumos") == 0);
    mock.queryCalls++;
    if (mock.queryStatus != ERROR_SUCCESS) return mock.queryStatus;
    if (type) *type = mock.queryType;
    if (value) {
        CHECK(bytes != NULL);
        if (!bytes) return ERROR_INVALID_PARAMETER;
        if (*bytes < mock.queryBytes) {
            *bytes = mock.queryBytes;
            return ERROR_MORE_DATA;
        }
        CHECK(mock.queryBytes <= sizeof(mock.queryValue));
        if (mock.queryBytes > sizeof(mock.queryValue)) return ERROR_MORE_DATA;
        memcpy(value, mock.queryValue, mock.queryBytes);
    }
    if (bytes) *bytes = mock.queryBytes;
    return ERROR_SUCCESS;
}

#define SHGetFolderPathW MockSHGetFolderPathW
#define CreateDirectoryW MockCreateDirectoryW
#define GetFileAttributesW MockGetFileAttributesW
#define WritePrivateProfileStringW MockWritePrivateProfileStringW
#define WritePrivateProfileSectionW MockWritePrivateProfileSectionW
#define GetPrivateProfileStringW MockGetPrivateProfileStringW
#define GetPrivateProfileIntW MockGetPrivateProfileIntW
#define GetModuleFileNameW MockGetModuleFileNameW
#define RegCreateKeyExW MockRegCreateKeyExW
#define RegOpenKeyExW MockRegOpenKeyExW
#define RegSetValueExW MockRegSetValueExW
#define RegDeleteValueW MockRegDeleteValueW
#define RegCloseKey MockRegCloseKey
#define RegQueryValueExW MockRegQueryValueExW
#include "../presets.c"

static void FillMockPath(WCHAR *path, size_t length)
{
    for (size_t i = 0; i < length; i++) path[i] = L'a';
    path[length] = L'\0';
}

static void CheckSettingsPath(size_t appDataLength, BOOL fallback)
{
    struct {
        unsigned char before[16];
        Settings settings;
        unsigned char after[16];
    } guarded;
    WCHAR expected[MAX_PATH];
    ResetMocks();
    FillMockPath(mock.appData, appDataLength);
    memset(&guarded, 0xa5, sizeof(guarded));
    Settings_Init(&guarded.settings);
    if (fallback) {
        StringCchCopyW(expected, ARRAYSIZE(expected), L".\\config.ini");
        CHECK(mock.directoryCalls == 0);
    } else {
        CHECK(SUCCEEDED(StringCchPrintfW(expected, ARRAYSIZE(expected),
                                        L"%s\\Lumos\\config.ini", mock.appData)));
        CHECK(mock.directoryCalls == 1);
        CHECK(wcslen(mock.directory) == appDataLength + 6);
    }
    CHECK(wcscmp(guarded.settings.iniPath, expected) == 0);
    CHECK(wcscmp(mock.iniWritePath, expected) == 0);
    CHECK(guarded.settings.step == 5);
    for (size_t i = 0; i < sizeof(guarded.before); i++) {
        CHECK(guarded.before[i] == 0xa5);
        CHECK(guarded.after[i] == 0xa5);
    }
}

static void TestSettingsPaths(void)
{
    Settings settings;
    CheckSettingsPath(242, FALSE);  /* 259 chars plus terminator fits MAX_PATH. */
    CheckSettingsPath(243, TRUE);   /* Complete path would need 261 WCHARs. */
    CheckSettingsPath(259, TRUE);   /* Even the directory suffix cannot fit. */
    ResetMocks();
    mock.folderStatus = E_FAIL;
    Settings_Init(&settings);
    CHECK(wcscmp(settings.iniPath, L".\\config.ini") == 0);
    CHECK(mock.directoryCalls == 0);
    CHECK(wcscmp(mock.iniWritePath, settings.iniPath) == 0);
}

static void TestAutostartPaths(void)
{
    ResetMocks();
    CHECK(Settings_SetAutostart(TRUE));
    CHECK(wcscmp(mock.command, L"\"C:\\Program Files\\Lumos\\lumos.exe\" " LUMOS_STARTUP_ARGUMENT) == 0);
    CHECK(mock.createCalls == 1 && mock.setCalls == 1 && mock.closeCalls == 1);

    ResetMocks();
    FillMockPath(mock.exePath, 259);
    mock.exeLength = 259;
    CHECK(Settings_SetAutostart(TRUE));
    CHECK(wcslen(mock.command) == 271);
    CHECK(mock.command[0] == L'"' && mock.command[260] == L'"');
    CHECK(wcscmp(mock.command + 262, LUMOS_STARTUP_ARGUMENT) == 0);
    CHECK(mock.commandBytes == 272 * sizeof(WCHAR));

    ResetMocks();
    FillMockPath(mock.exePath, 260);
    mock.exeLength = 260;
    CHECK(!Settings_SetAutostart(TRUE));
    CHECK(mock.createCalls == 0 && mock.setCalls == 0 && mock.closeCalls == 0);

    ResetMocks();
    mock.exeLength = 0;
    CHECK(!Settings_SetAutostart(TRUE));
    CHECK(mock.createCalls == 0 && mock.setCalls == 0);
}

static void TestAutostartFailures(void)
{
    ResetMocks();
    mock.createStatus = ERROR_ACCESS_DENIED;
    CHECK(!Settings_SetAutostart(TRUE));
    CHECK(mock.setCalls == 0 && mock.closeCalls == 0);

    ResetMocks();
    mock.setStatus = ERROR_ACCESS_DENIED;
    CHECK(!Settings_SetAutostart(TRUE));
    CHECK(mock.setCalls == 1 && mock.closeCalls == 1);

    ResetMocks();
    CHECK(Settings_SetAutostart(FALSE));
    CHECK(mock.createCalls == 0 && mock.deleteCalls == 1 && mock.closeCalls == 1);

    ResetMocks();
    mock.openStatus = ERROR_ACCESS_DENIED;
    CHECK(!Settings_SetAutostart(FALSE));
    CHECK(mock.deleteCalls == 0 && mock.closeCalls == 0);

    ResetMocks();
    mock.deleteStatus = ERROR_ACCESS_DENIED;
    CHECK(!Settings_SetAutostart(FALSE));
    CHECK(mock.deleteCalls == 1 && mock.closeCalls == 1);

    ResetMocks();
    mock.openStatus = ERROR_FILE_NOT_FOUND;
    CHECK(Settings_SetAutostart(FALSE));
    CHECK(mock.deleteCalls == 0 && mock.closeCalls == 0);

    ResetMocks();
    mock.deleteStatus = ERROR_FILE_NOT_FOUND;
    CHECK(Settings_SetAutostart(FALSE));
    CHECK(mock.deleteCalls == 1 && mock.closeCalls == 1);

    ResetMocks();
    CHECK(Settings_GetAutostart());
    CHECK(mock.closeCalls == 1);
    ResetMocks();
    mock.queryStatus = ERROR_FILE_NOT_FOUND;
    CHECK(!Settings_GetAutostart());
    CHECK(mock.closeCalls == 1);
}

static void TestDayBrightness(void)
{
    Settings settings = { 0 };
    CHECK(Settings_DayBrightness(&settings) == DEFAULT_DAY_BRIGHTNESS);
    CHECK(Settings_DayBrightness(NULL) == DEFAULT_DAY_BRIGHTNESS);
    settings.presetCount = 1;
    wcscpy(settings.presets[0].name, L"Night");
    settings.presets[0].brightness = 30;
    CHECK(Settings_DayBrightness(&settings) == DEFAULT_DAY_BRIGHTNESS);

    wcscpy(settings.presets[0].name, L"dAy");
    settings.presets[0].brightness = 64;
    CHECK(Settings_DayBrightness(&settings) == 64);
    settings.presets[0].brightness = 0;
    CHECK(Settings_DayBrightness(&settings) == 0);
    settings.presets[0].brightness = 100;
    CHECK(Settings_DayBrightness(&settings) == 100);

    settings.presetCount = 2;
    wcscpy(settings.presets[1].name, L"GiOrNo");
    settings.presets[1].brightness = 27;
    CHECK(Settings_DayBrightness(&settings) == 27); /* Giorno wins after Day. */
    Preset swap = settings.presets[0];
    settings.presets[0] = settings.presets[1];
    settings.presets[1] = swap;
    CHECK(Settings_DayBrightness(&settings) == 27); /* Giorno wins before Day. */
    settings.presets[0].brightness = 0;
    CHECK(Settings_DayBrightness(&settings) == 0);
    settings.presets[0].brightness = 100;
    settings.presets[1].brightness = 0;
    CHECK(Settings_DayBrightness(&settings) == 100);
    settings.presetCount = 1;
    CHECK(Settings_DayBrightness(&settings) == 100); /* Giorno alone is supported. */
    settings.presets[0].brightness = MAXDWORD;
    CHECK(Settings_DayBrightness(&settings) == 100);

    ResetMocks();
    Settings_CreateDefaults(&settings); /* Mocked INI writes only. */
    CHECK(_wtoi(mock.defaultDayValue) == DEFAULT_DAY_BRIGHTNESS);
    CHECK(mock.createCalls == 0 && mock.openCalls == 0 && mock.setCalls == 0);
}

static void SetQueryCommand(const WCHAR *command, DWORD type)
{
    CHECK(SUCCEEDED(StringCchCopyW(mock.queryValue, ARRAYSIZE(mock.queryValue), command)));
    mock.queryType = type;
    mock.queryBytes = (DWORD)((wcslen(mock.queryValue) + 1) * sizeof(WCHAR));
}

static void CheckNoAutostartWrite(void)
{
    CHECK(mock.createCalls == 0 && mock.setCalls == 0 && mock.deleteCalls == 0);
}

static void TestAutostartUpgrade(void)
{
    ResetMocks();
    SetQueryCommand(mock.exePath, REG_SZ);
    CHECK(Settings_UpgradeAutostart());
    CHECK(mock.createCalls == 0 && mock.setCalls == 1 && mock.closeCalls == 1);
    CHECK(wcscmp(mock.command, L"\"C:\\Program Files\\Lumos\\lumos.exe\" " LUMOS_STARTUP_ARGUMENT) == 0);

    ResetMocks();
    SetQueryCommand(L"\"C:\\PROGRAM FILES\\LUMOS\\LUMOS.EXE\"", REG_SZ);
    CHECK(Settings_UpgradeAutostart());
    CHECK(mock.setCalls == 1 && mock.closeCalls == 1);  /* Case-insensitive quoted legacy. */

    ResetMocks();
    FillMockPath(mock.exePath, 259);
    mock.exeLength = 259;
    StringCchPrintfW(mock.queryValue, ARRAYSIZE(mock.queryValue), L"\"%s\"", mock.exePath);
    mock.queryBytes = (DWORD)((wcslen(mock.queryValue) + 1) * sizeof(WCHAR));
    CHECK(Settings_UpgradeAutostart());
    CHECK(wcslen(mock.command) == 271 && mock.commandBytes == 272 * sizeof(WCHAR));

    ResetMocks();  /* Already has the exact quoted startup command. */
    CHECK(Settings_UpgradeAutostart());
    CheckNoAutostartWrite();
    CHECK(mock.closeCalls == 1);

    ResetMocks();
    SetQueryCommand(L"C:\\Other\\lumos.exe", REG_SZ);
    CHECK(Settings_UpgradeAutostart());
    CheckNoAutostartWrite();

    ResetMocks();
    SetQueryCommand(L"\"C:\\Program Files\\Lumos\\lumos.exe\" --other", REG_SZ);
    CHECK(Settings_UpgradeAutostart());
    CheckNoAutostartWrite();

    ResetMocks();
    mock.openStatus = ERROR_FILE_NOT_FOUND;
    CHECK(Settings_UpgradeAutostart());
    CheckNoAutostartWrite();
    CHECK(mock.queryCalls == 0 && mock.closeCalls == 0);

    ResetMocks();
    mock.queryStatus = ERROR_FILE_NOT_FOUND;
    CHECK(Settings_UpgradeAutostart());
    CheckNoAutostartWrite();
    CHECK(mock.closeCalls == 1);
}

static void TestAutostartUpgradeInvalidData(void)
{
    ResetMocks();
    SetQueryCommand(mock.exePath, REG_EXPAND_SZ);
    CHECK(!Settings_UpgradeAutostart());
    CheckNoAutostartWrite();
    CHECK(mock.closeCalls == 1);

    ResetMocks();
    mock.queryBytes = sizeof(mock.queryValue);  /* Larger than the read buffer. */
    CHECK(!Settings_UpgradeAutostart());
    CheckNoAutostartWrite();

    ResetMocks();
    SetQueryCommand(mock.exePath, REG_SZ);
    mock.queryBytes -= sizeof(WCHAR);  /* Missing registry terminator. */
    CHECK(!Settings_UpgradeAutostart());
    CheckNoAutostartWrite();

    ResetMocks();
    mock.queryBytes = 3;  /* Partial WCHAR data. */
    CHECK(!Settings_UpgradeAutostart());
    CheckNoAutostartWrite();

    ResetMocks();
    mock.queryBytes = 0;
    CHECK(!Settings_UpgradeAutostart());
    CheckNoAutostartWrite();

    ResetMocks();
    SetQueryCommand(mock.exePath, REG_SZ);
    size_t originalCharacters = mock.queryBytes / sizeof(WCHAR);
    mock.queryValue[originalCharacters] = L'x'; /* Payload after an embedded NUL. */
    mock.queryValue[originalCharacters + 1] = L'\0';
    mock.queryBytes = (DWORD)((originalCharacters + 2) * sizeof(WCHAR));
    CHECK(!Settings_UpgradeAutostart());
    CheckNoAutostartWrite();
}

static void TestAutostartUpgradeErrors(void)
{
    ResetMocks();
    mock.openStatus = ERROR_ACCESS_DENIED;
    CHECK(!Settings_UpgradeAutostart());
    CheckNoAutostartWrite();
    CHECK(mock.queryCalls == 0 && mock.closeCalls == 0);

    ResetMocks();
    mock.queryStatus = ERROR_ACCESS_DENIED;
    CHECK(!Settings_UpgradeAutostart());
    CheckNoAutostartWrite();
    CHECK(mock.closeCalls == 1);

    ResetMocks();
    SetQueryCommand(mock.exePath, REG_SZ);
    mock.setStatus = ERROR_ACCESS_DENIED;
    CHECK(!Settings_UpgradeAutostart());
    CHECK(mock.setCalls == 1 && mock.closeCalls == 1 && mock.createCalls == 0);

    ResetMocks();
    SetQueryCommand(mock.exePath, REG_SZ);
    mock.exeLength = 0;
    CHECK(!Settings_UpgradeAutostart());
    CheckNoAutostartWrite();

    ResetMocks();
    FillMockPath(mock.exePath, 260);
    mock.exeLength = 260;
    SetQueryCommand(mock.exePath, REG_SZ);
    CHECK(!Settings_UpgradeAutostart());
    CheckNoAutostartWrite();
}

static BrightMonitor SelectionMonitor(MonitorBackend backend, const WCHAR *identity)
{
    BrightMonitor monitor = { 0 };
    monitor.backend = backend;
    monitor.controllable = TRUE;
    monitor.brightnessMax = 100;
    wcscpy(monitor.name, L"Same display name");
    if (backend == BACKEND_DDC)
        StringCchCopyW(monitor.deviceInstance, ARRAYSIZE(monitor.deviceInstance), identity);
    if (backend == BACKEND_WMI)
        StringCchCopyW(monitor.wmiInstance, ARRAYSIZE(monitor.wmiInstance), identity);
    return monitor;
}

static void TestMonitorSelectionKeys(void)
{
    BrightMonitor monitor = SelectionMonitor(BACKEND_DDC, L"DISPLAY\\DDC1");
    MonitorSelection selection = { 0 };
    WCHAR key[MONITOR_SELECTION_KEY_LEN];
    CHECK(Settings_MonitorKey(&monitor, key));
    CHECK(wcscmp(key, L"DDC:DISPLAY\\DDC1") == 0);
    CHECK(Settings_MonitorSelected(&selection, &monitor));
    selection.selectedOnly = TRUE;
    CHECK(!Settings_MonitorSelected(&selection, &monitor)); /* Custom empty is not All. */
    selection.count = 1;
    wcscpy(selection.keys[0], L"ddc:display\\ddc1");
    CHECK(Settings_MonitorSelected(&selection, &monitor));
    monitor = SelectionMonitor(BACKEND_WMI, L"DISPLAY\\DDC1");
    CHECK(Settings_MonitorKey(&monitor, key));
    CHECK(wcscmp(key, L"WMI:DISPLAY\\DDC1") == 0);
    CHECK(!Settings_MonitorSelected(&selection, &monitor)); /* Backend prefixes differ. */
    monitor = SelectionMonitor(BACKEND_DDC, L"");
    monitor.hPhysical = (HANDLE)(UINT_PTR)1;
    monitor.hMonitor = (HMONITOR)(UINT_PTR)1;
    CHECK(!Settings_MonitorKey(&monitor, key) && key[0] == L'\0');
    CHECK(!Settings_MonitorSelected(&selection, &monitor)); /* Never name/index/handle fallback. */
    monitor = SelectionMonitor(BACKEND_NONE, L"DISPLAY\\DDC1");
    CHECK(!Settings_MonitorKey(&monitor, key));
    monitor = SelectionMonitor(BACKEND_DDC, L"DISPLAY\\DDC1\nInjected=1");
    CHECK(!Settings_MonitorKey(&monitor, key));
    monitor = SelectionMonitor(BACKEND_DDC, L"");
    FillMockPath(monitor.deviceInstance, 255);
    CHECK(Settings_MonitorKey(&monitor, key) && wcslen(key) == 259);
    for (size_t i = 0; i < ARRAYSIZE(monitor.deviceInstance); i++) monitor.deviceInstance[i] = L'a';
    CHECK(!Settings_MonitorKey(&monitor, key) && key[0] == L'\0');
}

static void TestApplyMonitorSelection(void)
{
    Settings settings = { 0 };
    MonitorList view = { 0 };
    view.count = 3;
    view.monitors[0] = SelectionMonitor(BACKEND_DDC, L"DISPLAY\\DDC1");
    view.monitors[1] = SelectionMonitor(BACKEND_DDC, L"DISPLAY\\DDC2");
    view.monitors[2] = SelectionMonitor(BACKEND_WMI, L"DISPLAY\\PANEL_0");
    for (int i = 0; i < view.count; i++) view.monitors[i].excludedFromControl = TRUE;
    Settings_ApplyMonitorSelection(&settings, &view);
    CHECK(!view.selectedOnly);
    for (int i = 0; i < view.count; i++) CHECK(!view.monitors[i].excludedFromControl);

    settings.monitorSelection.selectedOnly = TRUE;
    Settings_ApplyMonitorSelection(&settings, &view);
    CHECK(view.selectedOnly);
    for (int i = 0; i < view.count; i++) CHECK(view.monitors[i].excludedFromControl);
    settings.monitorSelection.count = 1;
    wcscpy(settings.monitorSelection.keys[0], L"DDC:DISPLAY\\OFFLINE");
    wcscpy(settings.monitorSelection.names[0], L"Offline display");
    Settings_ApplyMonitorSelection(&settings, &view);
    for (int i = 0; i < view.count; i++) CHECK(view.monitors[i].excludedFromControl);
    CHECK(settings.monitorSelection.count == 1);
    CHECK(wcscmp(settings.monitorSelection.names[0], L"Offline display") == 0);

    Settings_MonitorKey(&view.monitors[0], settings.monitorSelection.keys[0]);
    settings.monitorSelection.count = 2;
    Settings_MonitorKey(&view.monitors[2], settings.monitorSelection.keys[1]);
    Settings_ApplyMonitorSelection(&settings, &view);
    CHECK(!view.monitors[0].excludedFromControl && view.monitors[1].excludedFromControl);
    CHECK(!view.monitors[2].excludedFromControl);
    view.monitors[1] = view.monitors[0]; /* Two monitors expose the selected identity. */
    Settings_ApplyMonitorSelection(&settings, &view);
    CHECK(view.monitors[0].excludedFromControl && view.monitors[1].excludedFromControl);
    CHECK(!view.monitors[2].excludedFromControl); /* Other unique selected keys remain enabled. */
    view.monitors[1].controllable = FALSE;
    Settings_ApplyMonitorSelection(&settings, &view);
    CHECK(!view.monitors[0].excludedFromControl && !view.monitors[2].excludedFromControl);
}

static void TestMonitorSelectionPersistence(void)
{
    Settings settings = { 0 }, loaded = { 0 };
    ResetMocks();
    loaded.monitorSelection.selectedOnly = TRUE;
    loaded.monitorSelection.count = 1;
    Settings_Load(&loaded);
    CHECK(!loaded.monitorSelection.selectedOnly && loaded.monitorSelection.count == 0);
    PutSelectionIni(L"Mode", L"Selected");
    PutSelectionIni(L"Count", L"0");
    Settings_Load(&loaded);
    CHECK(loaded.monitorSelection.selectedOnly && loaded.monitorSelection.count == 0);

    settings.monitorSelection.selectedOnly = TRUE;
    settings.monitorSelection.count = 2;
    wcscpy(settings.monitorSelection.keys[0], L"DDC:DISPLAY\\DESKTOP");
    wcscpy(settings.monitorSelection.names[0], L"Desktop display");
    wcscpy(settings.monitorSelection.keys[1], L"WMI:DISPLAY\\OFFLINE_0");
    wcscpy(settings.monitorSelection.names[1], L"Offline internal panel");
    Settings_Save(&settings);
    Settings_Load(&loaded);
    CHECK(loaded.monitorSelection.selectedOnly && loaded.monitorSelection.count == 2);
    CHECK(wcscmp(loaded.monitorSelection.keys[0], settings.monitorSelection.keys[0]) == 0);
    CHECK(wcscmp(loaded.monitorSelection.names[1], L"Offline internal panel") == 0);
    CHECK(mock.createCalls == 0 && mock.openCalls == 0 && mock.setCalls == 0);
    settings.monitorSelection.selectedOnly = FALSE;
    Settings_Save(&settings);
    Settings_Load(&loaded);
    CHECK(!loaded.monitorSelection.selectedOnly && loaded.monitorSelection.count == 2);
    CHECK(wcscmp(loaded.monitorSelection.names[1], L"Offline internal panel") == 0);
}

static void TestMonitorSelectionIniBounds(void)
{
    Settings settings = { 0 };
    WCHAR field[16], value[MONITOR_SELECTION_KEY_LEN + 1];
    ResetMocks();
    PutSelectionIni(L"Mode", L"Selected");
    PutSelectionIni(L"Count", L"1000");
    for (int i = 0; i <= MAX_MONITORS; i++) {
        StringCchPrintfW(field, ARRAYSIZE(field), L"Key%d", i);
        StringCchPrintfW(value, ARRAYSIZE(value), L"DDC:DISPLAY\\MONITOR%d", i);
        PutSelectionIni(field, value);
    }
    Settings_Load(&settings);
    CHECK(settings.monitorSelection.selectedOnly && settings.monitorSelection.count == MAX_MONITORS);
    CHECK(wcscmp(settings.monitorSelection.keys[MAX_MONITORS - 1], L"DDC:DISPLAY\\MONITOR15") == 0);

    ResetMocks();
    PutSelectionIni(L"Mode", L"Selected");
    PutSelectionIni(L"Count", L"1");
    FillMockPath(value, MONITOR_SELECTION_KEY_LEN);
    memcpy(value, L"DDC:", 4 * sizeof(WCHAR));
    PutSelectionIni(L"Key0", value);
    Settings_Load(&settings);
    CHECK(settings.monitorSelection.selectedOnly && settings.monitorSelection.count == 0);
    value[MONITOR_SELECTION_KEY_LEN - 1] = L'\0'; /* Exact maximum length is valid. */
    PutSelectionIni(L"Key0", value);
    Settings_Load(&settings);
    CHECK(settings.monitorSelection.count == 1 && wcslen(settings.monitorSelection.keys[0]) == 259);

    PutSelectionIni(L"Count", L"-1");
    Settings_Load(&settings);
    CHECK(settings.monitorSelection.selectedOnly && settings.monitorSelection.count == 0);
    PutSelectionIni(L"Mode", L"CorruptMode");
    PutSelectionIni(L"Count", L"1");
    Settings_Load(&settings);
    CHECK(settings.monitorSelection.selectedOnly && settings.monitorSelection.count == 0);
}

static void TestMonitorSelectionInvalidEntries(void)
{
    Settings settings = { 0 }, loaded = { 0 };
    const WCHAR *keys[] = { L"UNKNOWN:id", L"DDC:", L"WMI:", L"",
                           L"DDC:id\r\nInjected=1", L"DDC:DISPLAY\\VALID",
                           L"ddc:display\\valid", L"WMI:DISPLAY\\OFFLINE_0" };
    WCHAR field[16];
    ResetMocks();
    PutSelectionIni(L"Mode", L"Selected");
    PutSelectionIni(L"Count", L"8");
    for (int i = 0; i < (int)ARRAYSIZE(keys); i++) {
        StringCchPrintfW(field, ARRAYSIZE(field), L"Key%d", i);
        PutSelectionIni(field, keys[i]);
    }
    PutSelectionIni(L"Name5", L"First selected display");
    PutSelectionIni(L"Name6", L"Duplicate display name");
    PutSelectionIni(L"Name7", L"Offline saved panel");
    Settings_Load(&settings);
    CHECK(settings.monitorSelection.selectedOnly && settings.monitorSelection.count == 2);
    CHECK(wcscmp(settings.monitorSelection.names[0], L"First selected display") == 0);
    CHECK(wcscmp(settings.monitorSelection.names[1], L"Offline saved panel") == 0);
    settings.monitorSelection.count = 4;
    wcscpy(settings.monitorSelection.keys[2], L"INVALID:entry");
    wcscpy(settings.monitorSelection.keys[3], L"ddc:display\\valid");
    Settings_Save(&settings);
    Settings_Load(&loaded);
    CHECK(loaded.monitorSelection.selectedOnly && loaded.monitorSelection.count == 2);
    CHECK(wcscmp(loaded.monitorSelection.names[1], L"Offline saved panel") == 0);
}

int main(void)
{
    TestSettingsPaths();
    TestAutostartPaths();
    TestAutostartFailures();
    TestDayBrightness();
    TestAutostartUpgrade();
    TestAutostartUpgradeInvalidData();
    TestAutostartUpgradeErrors();
    TestMonitorSelectionKeys();
    TestApplyMonitorSelection();
    TestMonitorSelectionPersistence();
    TestMonitorSelectionIniBounds();
    TestMonitorSelectionInvalidEntries();
    if (failures) {
        printf("%d settings checks failed\n", failures);
        return 1;
    }
    puts("ALL PASS: settings paths, autostart, daytime preset and monitor selection (mocked I/O)");
    return 0;
}
