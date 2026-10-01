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
    int directoryCalls, createCalls, openCalls, setCalls, deleteCalls, closeCalls;
    WCHAR directory[MAX_PATH];
    WCHAR iniWritePath[MAX_PATH];
    WCHAR command[MAX_PATH + 2];
    DWORD commandBytes;
} mock;

static void ResetMocks(void)
{
    memset(&mock, 0, sizeof(mock));
    StringCchCopyW(mock.appData, ARRAYSIZE(mock.appData), L"C:\\Users\\Lumos Test\\AppData\\Roaming");
    StringCchCopyW(mock.exePath, ARRAYSIZE(mock.exePath), L"C:\\Program Files\\Lumos\\lumos.exe");
    mock.exeLength = (DWORD)wcslen(mock.exePath);
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

static BOOL WINAPI MockWritePrivateProfileStringW(LPCWSTR section, LPCWSTR key,
                                                 LPCWSTR value, LPCWSTR path)
{
    (void)section; (void)key; (void)value;
    CHECK(SUCCEEDED(StringCchCopyW(mock.iniWritePath, ARRAYSIZE(mock.iniWritePath), path)));
    return TRUE;
}

static BOOL WINAPI MockWritePrivateProfileSectionW(LPCWSTR section, LPCWSTR values,
                                                  LPCWSTR path)
{
    (void)section; (void)values; (void)path;
    return TRUE;
}

static DWORD WINAPI MockGetPrivateProfileStringW(LPCWSTR section, LPCWSTR key,
                                                LPCWSTR fallback, LPWSTR buffer,
                                                DWORD capacity, LPCWSTR path)
{
    (void)section; (void)key; (void)fallback; (void)path;
    if (capacity) buffer[0] = L'\0';
    return 0;
}

static UINT WINAPI MockGetPrivateProfileIntW(LPCWSTR section, LPCWSTR key,
                                            INT fallback, LPCWSTR path)
{
    (void)section; (void)key; (void)path;
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
    (void)handle; (void)name; (void)reserved; (void)type; (void)value; (void)bytes;
    return mock.queryStatus;
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
    CHECK(wcscmp(mock.command, L"\"C:\\Program Files\\Lumos\\lumos.exe\"") == 0);
    CHECK(mock.createCalls == 1 && mock.setCalls == 1 && mock.closeCalls == 1);

    ResetMocks();
    FillMockPath(mock.exePath, 259);
    mock.exeLength = 259;
    CHECK(Settings_SetAutostart(TRUE));
    CHECK(wcslen(mock.command) == 261);
    CHECK(mock.command[0] == L'"' && mock.command[260] == L'"');
    CHECK(mock.commandBytes == 262 * sizeof(WCHAR));

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

int main(void)
{
    TestSettingsPaths();
    TestAutostartPaths();
    TestAutostartFailures();
    if (failures) {
        printf("%d settings checks failed\n", failures);
        return 1;
    }
    puts("ALL PASS: settings paths and autostart errors (mocked I/O)");
    return 0;
}
