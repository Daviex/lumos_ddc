/* Real formatting, locking and rotation with in-memory file APIs. No runtime
   file, monitor, application window or configuration is touched. */
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>
#include "../diagnostics.h"

static int failures, opens, closes, moves, debugLines;
static HANDLE formattingReached, resumeFormatting;
static BOOL denyExe, denyAll, failMove, failWrite;
static ULONGLONG fileBytes;
static WCHAR openedPath[MAX_PATH];
static char currentLog[262144], previousLog[262144];
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)

static DWORD WINAPI MockModule(HMODULE module, WCHAR *path, DWORD capacity)
{
    const WCHAR *value = L"C:\\Installed\\Lumos\\lumos.exe";
    (void)module;
    CHECK(capacity > wcslen(value));
    wcscpy(path, value);
    return (DWORD)wcslen(value);
}

static HRESULT WINAPI MockAppData(HWND owner, int folder, HANDLE token, DWORD flags, WCHAR *path)
{
    (void)owner; (void)token; (void)flags;
    CHECK(folder == CSIDL_APPDATA);
    wcscpy(path, L"C:\\Profile\\AppData");
    return S_OK;
}

static BOOL WINAPI MockDirectory(const WCHAR *path, LPSECURITY_ATTRIBUTES attrs)
{ (void)path; (void)attrs; return TRUE; }

static HANDLE WINAPI MockOpen(const WCHAR *path, DWORD access, DWORD share,
                             LPSECURITY_ATTRIBUTES attrs, DWORD disposition, DWORD flags, HANDLE templateFile)
{
    (void)attrs; (void)flags; (void)templateFile;
    CHECK((access & FILE_APPEND_DATA) != 0);
    CHECK((share & FILE_SHARE_READ) != 0);
    CHECK(disposition == OPEN_ALWAYS);
    opens++;
    wcscpy(openedPath, path);
    if (denyAll || (denyExe && wcsstr(path, L"Installed"))) {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    SetLastError(1234);
    return (HANDLE)(UINT_PTR)11;
}

static BOOL WINAPI MockSize(HANDLE file, LARGE_INTEGER *size)
{ CHECK(file == (HANDLE)(UINT_PTR)11); size->QuadPart = (LONGLONG)fileBytes; return TRUE; }

static BOOL WINAPI MockClose(HANDLE file)
{ CHECK(file == (HANDLE)(UINT_PTR)11); closes++; SetLastError(5678); return TRUE; }

static BOOL WINAPI MockWrite(HANDLE file, const void *data, DWORD bytes, DWORD *written, LPOVERLAPPED overlapped)
{
    (void)overlapped;
    CHECK(file == (HANDLE)(UINT_PTR)11);
    *written = 0;
    if (failWrite) { SetLastError(ERROR_DISK_FULL); return FALSE; }
    size_t used = strlen(currentLog);
    CHECK(used + bytes < sizeof(currentLog));
    if (used + bytes >= sizeof(currentLog)) return FALSE;
    memcpy(currentLog + used, data, bytes);
    currentLog[used + bytes] = '\0';
    *written = bytes;
    fileBytes += bytes;
    SetLastError(9012);
    return TRUE;
}

static BOOL WINAPI MockMove(const WCHAR *from, const WCHAR *to, DWORD flags)
{
    CHECK(wcsstr(from, L"lumos-diagnostics.log") != NULL);
    CHECK(wcsstr(to, L"lumos-diagnostics.previous.log") != NULL);
    CHECK(flags == MOVEFILE_REPLACE_EXISTING);
    moves++;
    if (failMove) { SetLastError(ERROR_SHARING_VIOLATION); return FALSE; }
    strcpy(previousLog, currentLog);
    currentLog[0] = '\0';
    fileBytes = 0;
    return TRUE;
}

static void WINAPI MockDebug(const char *text)
{ CHECK(text != NULL); debugLines++; }

static void WINAPI MockTime(LPSYSTEMTIME time)
{
    GetLocalTime(time);
    if (formattingReached) {
        SetEvent(formattingReached);
        CHECK(WaitForSingleObject(resumeFormatting, 10000) == WAIT_OBJECT_0);
    }
}

#define GetModuleFileNameW MockModule
#define SHGetFolderPathW MockAppData
#define CreateDirectoryW MockDirectory
#define CreateFileW MockOpen
#define GetFileSizeEx MockSize
#define CloseHandle MockClose
#define WriteFile MockWrite
#define MoveFileExW MockMove
#define OutputDebugStringA MockDebug
#define GetLocalTime MockTime
#include "../diagnostics.c"
#undef CloseHandle

static void Reset(void)
{
    g_diagnosticsFile = INVALID_HANDLE_VALUE;
    g_diagnosticsOpened = g_diagnosticsClosed = FALSE;
    InterlockedExchange(&g_diagnosticsEnabled, FALSE);
    g_diagnosticsBytes = fileBytes = 0;
    g_diagnosticsPath[0] = g_diagnosticsPrevious[0] = L'\0';
    currentLog[0] = previousLog[0] = '\0';
    denyExe = denyAll = failMove = failWrite = FALSE;
    opens = closes = moves = debugLines = 0;
    formattingReached = resumeFormatting = NULL;
}

static DWORD WINAPI Writer(void *parameter)
{
    int index = (int)(INT_PTR)parameter;
    for (int i = 0; i < 100; i++) {
        SetLastError(0xC0262582);
        Diagnostics_Log("INFO", "parallel", "writer=%d item=%d", index, i);
        CHECK(GetLastError() == 0xC0262582);
    }
    return 0;
}

static void TestDisabledStartupAndRuntimeSwitch(void)
{
    Reset();
    denyAll = TRUE;
    BrightMonitor monitor = {0};
    SetLastError(73);
    Diagnostics_Log("INFO", "pre-init", "settings are not loaded yet");
    Diagnostics_Init(FALSE);
    Diagnostics_Log("ERROR", "disabled", "must not open a file or use debug output");
    Diagnostics_Monitor(&monitor, "INFO", "disabled", "monitor entry");
    Diagnostics_MonitorState(&monitor, "state entry");
    CHECK(GetLastError() == 73);
    CHECK(opens == 0 && closes == 0 && moves == 0 && debugLines == 0 && !currentLog[0]);
    denyAll = FALSE;
    Diagnostics_SetEnabled(TRUE);
    CHECK(GetLastError() == 73 && opens == 1 && strstr(currentLog, "START build=") != NULL);
    Diagnostics_Log("INFO", "enabled", "first enabled segment");
    size_t length = strlen(currentLog);
    ULONGLONG bytes = fileBytes;
    Diagnostics_SetEnabled(TRUE); /* Reapplying an unchanged preference is idempotent. */
    CHECK(opens == 1 && strlen(currentLog) == length);
    Diagnostics_SetEnabled(FALSE);
    CHECK(GetLastError() == 73 && closes == 1 && g_diagnosticsFile == INVALID_HANDLE_VALUE);
    Diagnostics_Log("INFO", "disabled", "must not append while disabled");
    Diagnostics_Close();
    CHECK(opens == 1 && closes == 1 && fileBytes == bytes && strlen(currentLog) == length);
    CHECK(strstr(currentLog, "must not append") == NULL && moves == 0 && debugLines == 0);
    Diagnostics_SetEnabled(TRUE); /* Final shutdown cannot be reversed by late callers. */
    CHECK(opens == 1 && fileBytes == bytes);

    Reset();
    Diagnostics_Init(TRUE);
    Diagnostics_Log("INFO", "enabled", "history survives the toggle");
    Diagnostics_SetEnabled(FALSE);
    int oldOpens = opens, oldCloses = closes;
    Diagnostics_SetEnabled(FALSE);
    CHECK(opens == oldOpens && closes == oldCloses);
    Diagnostics_SetEnabled(TRUE);
    Diagnostics_Log("INFO", "reenabled", "second enabled segment");
    CHECK(opens == oldOpens + 1 && closes == oldCloses);
    CHECK(strstr(currentLog, "history survives") != NULL && strstr(currentLog, "second enabled segment") != NULL);
    CHECK(moves == 0);
    fileBytes = g_diagnosticsBytes = DIAGNOSTICS_MAX_BYTES;
    Diagnostics_SetEnabled(FALSE);
    Diagnostics_SetEnabled(TRUE); /* Reload the existing size before appending/rotating. */
    CHECK(moves == 1 && fileBytes < DIAGNOSTICS_MAX_BYTES && strstr(previousLog, "history survives") != NULL);
    Diagnostics_Close();
}

static DWORD WINAPI QueuedWriter(void *parameter)
{
    (void)parameter;
    SetLastError(0xC0262582);
    Diagnostics_Log("INFO", "race", "queued writer must not append after disable");
    CHECK(GetLastError() == 0xC0262582);
    return 0;
}

static void TestDisableDropsAlreadyFormattingWriter(void)
{
    Reset();
    Diagnostics_Init(TRUE);
    formattingReached = CreateEventW(NULL, TRUE, FALSE, NULL);
    resumeFormatting = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(formattingReached && resumeFormatting);
    HANDLE thread = CreateThread(NULL, 0, QueuedWriter, NULL, 0, NULL);
    CHECK(thread != NULL);
    CHECK(WaitForSingleObject(formattingReached, 10000) == WAIT_OBJECT_0);
    ULONGLONG bytes = fileBytes;
    size_t length = strlen(currentLog);
    Diagnostics_SetEnabled(FALSE);
    SetEvent(resumeFormatting);
    CHECK(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
    CHECK(closes == 1 && opens == 1 && fileBytes == bytes && strlen(currentLog) == length);
    CHECK(strstr(currentLog, "queued writer") == NULL && debugLines == 0);
    CloseHandle(thread);
    CloseHandle(formattingReached);
    CloseHandle(resumeFormatting);
    formattingReached = resumeFormatting = NULL;
    Diagnostics_SetEnabled(TRUE);
    Diagnostics_Log("INFO", "race", "fresh logging resumes");
    CHECK(strstr(currentLog, "fresh logging resumes") != NULL && opens == 2);
    Diagnostics_Close();
}

int main(void)
{
    TestDisabledStartupAndRuntimeSwitch();
    TestDisableDropsAlreadyFormattingWriter();
    Reset();
    SetLastError(0xC0262582);
    Diagnostics_Init(TRUE);
    CHECK(GetLastError() == 0xC0262582);
    CHECK(wcscmp(openedPath, L"C:\\Installed\\Lumos\\lumos-diagnostics.log") == 0);
    CHECK(strstr(currentLog, "START build=") != NULL);
    BrightMonitor monitor = {0};
    monitor.backend = BACKEND_DDC;
    wcscpy(monitor.name, L"Monitor \x00E8");
    wcscpy(monitor.deviceInstance, L"DISPLAY\\AUS2722\\SECOND");
    monitor.expectedInput = 15;
    monitor.currentInput = 17;
    monitor.sourceKnown = monitor.sourceFilter = TRUE;
    Diagnostics_MonitorState(&monitor, "other-input-check");
    CHECK(GetLastError() == 0xC0262582);
    CHECK(strstr(currentLog, "Monitor \xC3\xA8") != NULL);
    CHECK(strstr(currentLog, "DDC:DISPLAY\\AUS2722\\SECOND") != NULL);
    CHECK(strstr(currentLog, "current=0x11(HDMI1)") != NULL);
    CHECK(strstr(currentLog, "sourcePolicy=other-input") != NULL);
    CHECK(strstr(currentLog, "[pid=") != NULL && strstr(currentLog, " tid=") != NULL);

    HANDLE threads[4];
    for (int i = 0; i < 4; i++) threads[i] = CreateThread(NULL, 0, Writer, (void *)(INT_PTR)i, 0, NULL);
    CHECK(WaitForMultipleObjects(4, threads, TRUE, 10000) == WAIT_OBJECT_0);
    for (int i = 0; i < 4; i++) {
        CloseHandle(threads[i]);
        for (int j = 0; j < 100; j++) {
            char expected[96];
            snprintf(expected, sizeof(expected), "[parallel] writer=%d item=%d\r\n", i, j);
            CHECK(strstr(currentLog, expected) != NULL);
        }
    }
    fileBytes = g_diagnosticsBytes = DIAGNOSTICS_MAX_BYTES;
    Diagnostics_Log("INFO", "rotation", "next file");
    CHECK(moves == 1 && fileBytes < DIAGNOSTICS_MAX_BYTES);
    CHECK(strstr(previousLog, "other-input-check") != NULL);
    CHECK(strstr(currentLog, "next file") != NULL);
    Diagnostics_Close();
    CHECK(strstr(currentLog, "[session] END") != NULL);
    int oldOpens = opens;
    Diagnostics_Log("INFO", "late-worker", "must not reopen after shutdown");
    CHECK(opens == oldOpens && strstr(currentLog, "must not reopen") == NULL);

    Reset(); denyExe = TRUE;
    Diagnostics_Init(TRUE);
    CHECK(wcscmp(openedPath, L"C:\\Profile\\AppData\\Lumos\\lumos-diagnostics.log") == 0);
    CHECK(strstr(currentLog, "exeFolderError=0x00000005") != NULL);
    Reset(); denyAll = TRUE;
    SetLastError(42); Diagnostics_Init(TRUE);
    CHECK(GetLastError() == 42 && debugLines >= 2 && currentLog[0] == '\0');
    Reset(); Diagnostics_Init(TRUE); failWrite = TRUE;
    SetLastError(73); Diagnostics_Log("ERROR", "disk-test", "disk full");
    CHECK(GetLastError() == 73 && debugLines > 0);
    failWrite = FALSE; failMove = TRUE;
    fileBytes = g_diagnosticsBytes = DIAGNOSTICS_MAX_BYTES;
    Diagnostics_Log("INFO", "rotation", "rotation must not grow a full file");
    CHECK(fileBytes == DIAGNOSTICS_MAX_BYTES && debugLines > 0);
    failMove = FALSE;
    Diagnostics_Log("INFO", "rotation", "reader released: rotation recovered");
    CHECK(fileBytes < DIAGNOSTICS_MAX_BYTES && strstr(currentLog, "rotation recovered") != NULL);
    printf("diagnostics: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
