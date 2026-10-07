#include "capture.h"

/* Windows can report capability usage under ConsentStore. Recognized app
   records expose LastUsedTimeStart/Stop; a zero stop time is interpreted as
   an open microphone or camera. Reading it needs no elevation and does not
   capture audio or video. This is a per-user usage heuristic, rather than a
   documented universal call API: some desktop applications do not appear in
   Windows's privacy usage reporting. Do not infer freshness from the start
   time, since a legitimate device session can remain open for many hours. */
#define CONSENT_STORE \
    L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\"

#define MAX_KEY_NAME 256

static BOOL KeyReportsInUse(HKEY hKey)
{
    ULONGLONG stop = 0;
    DWORD size = sizeof(stop), type = 0;
    if (RegQueryValueExW(hKey, L"LastUsedTimeStop", NULL, &type,
                         (BYTE *)&stop, &size) != ERROR_SUCCESS)
        return FALSE;
    if (type != REG_QWORD || size != sizeof(stop))
        return FALSE;
    return stop == 0;
}

/* Any subkey of the capability that reports the device as open right now.
   Packaged applications sit directly under the capability, keyed by package
   family name; desktop applications sit one level deeper under NonPackaged,
   keyed by executable path. */
static BOOL AnyAppInUse(const WCHAR *capability)
{
    WCHAR path[512];
    wcscpy(path, CONSENT_STORE);
    wcsncat(path, capability, (sizeof(path) / sizeof(WCHAR)) - wcslen(path) - 1);

    HKEY hCap;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &hCap) != ERROR_SUCCESS)
        return FALSE;

    BOOL inUse = FALSE;
    for (DWORD i = 0; !inUse; i++) {
        WCHAR name[MAX_KEY_NAME];
        DWORD len = MAX_KEY_NAME;
        if (RegEnumKeyExW(hCap, i, name, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;

        HKEY hApp;
        if (RegOpenKeyExW(hCap, name, 0, KEY_READ, &hApp) != ERROR_SUCCESS)
            continue;

        if (_wcsicmp(name, L"NonPackaged") == 0) {
            for (DWORD j = 0; !inUse; j++) {
                WCHAR exeKey[MAX_KEY_NAME];
                DWORD exeLen = MAX_KEY_NAME;
                if (RegEnumKeyExW(hApp, j, exeKey, &exeLen, NULL, NULL, NULL, NULL)
                        != ERROR_SUCCESS)
                    break;
                HKEY hExe;
                if (RegOpenKeyExW(hApp, exeKey, 0, KEY_READ, &hExe) == ERROR_SUCCESS) {
                    inUse = KeyReportsInUse(hExe);
                    RegCloseKey(hExe);
                }
            }
        } else {
            inUse = KeyReportsInUse(hApp);
        }

        RegCloseKey(hApp);
    }

    RegCloseKey(hCap);
    return inUse;
}

BOOL Capture_InUse(void)
{
    /* Only the per-user store is consulted. The machine-wide store under HKLM
       also lists services such as the camera frame server, and a service that
       never writes a stop time would block dimming forever, which is a worse
       failure than missing an exotic case. */
    return AnyAppInUse(L"microphone") || AnyAppInUse(L"webcam");
}
