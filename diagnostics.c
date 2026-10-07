#include "diagnostics.h"
#include <shlobj.h>
#include <strsafe.h>
#include <stdio.h>
#include <stdarg.h>

#define DIAGNOSTICS_MAX_BYTES (5ULL * 1024ULL * 1024ULL)
#define DIAGNOSTICS_FILE L"lumos-diagnostics.log"
#define DIAGNOSTICS_PREVIOUS_FILE L"lumos-diagnostics.previous.log"

static SRWLOCK g_diagnosticsLock = SRWLOCK_INIT;
static HANDLE g_diagnosticsFile = INVALID_HANDLE_VALUE;
static WCHAR g_diagnosticsPath[MAX_PATH];
static WCHAR g_diagnosticsPrevious[MAX_PATH];
static ULONGLONG g_diagnosticsBytes;
static BOOL g_diagnosticsOpened, g_diagnosticsClosed;

static void Utf8(const WCHAR *text, char *out, int capacity)
{
    out[0] = '\0';
    if (text && !WideCharToMultiByte(CP_UTF8, 0, text, -1, out, capacity, NULL, NULL))
        StringCchCopyA(out, (size_t)capacity, "<unavailable>");
    for (char *p = out; *p; p++)
        if (*p == '\r' || *p == '\n') *p = ' ';
}

static BOOL OpenFileLocked(void)
{
    g_diagnosticsFile = CreateFileW(g_diagnosticsPath, FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_diagnosticsFile == INVALID_HANDLE_VALUE) return FALSE;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(g_diagnosticsFile, &size)) {
        DWORD error = GetLastError();
        CloseHandle(g_diagnosticsFile);
        g_diagnosticsFile = INVALID_HANDLE_VALUE;
        SetLastError(error);
        return FALSE;
    }
    g_diagnosticsBytes = (ULONGLONG)size.QuadPart;
    return TRUE;
}

/* No working-directory dependency. If the exe folder is read-only, record that
   failure in an explicitly identified AppData fallback instead of elevating. */
static DWORD OpenLocked(void)
{
    DWORD primaryError = ERROR_SUCCESS;
    WCHAR directory[MAX_PATH];
    DWORD length = GetModuleFileNameW(NULL, directory, ARRAYSIZE(directory));
    WCHAR *slash = length && length < ARRAYSIZE(directory) ? wcsrchr(directory, L'\\') : NULL;
    if (slash) {
        slash[1] = L'\0';
        if (SUCCEEDED(StringCchPrintfW(g_diagnosticsPath, ARRAYSIZE(g_diagnosticsPath),
                                      L"%s%s", directory, DIAGNOSTICS_FILE)) &&
            SUCCEEDED(StringCchPrintfW(g_diagnosticsPrevious, ARRAYSIZE(g_diagnosticsPrevious),
                                      L"%s%s", directory, DIAGNOSTICS_PREVIOUS_FILE)) &&
            OpenFileLocked()) return ERROR_SUCCESS;
        primaryError = GetLastError();
        if (!primaryError) primaryError = ERROR_FILENAME_EXCED_RANGE;
    } else {
        primaryError = ERROR_FILENAME_EXCED_RANGE;
    }
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, directory)) &&
        SUCCEEDED(StringCchCatW(directory, ARRAYSIZE(directory), L"\\Lumos"))) {
        CreateDirectoryW(directory, NULL);
        if (SUCCEEDED(StringCchPrintfW(g_diagnosticsPath, ARRAYSIZE(g_diagnosticsPath),
                                      L"%s\\%s", directory, DIAGNOSTICS_FILE)) &&
            SUCCEEDED(StringCchPrintfW(g_diagnosticsPrevious, ARRAYSIZE(g_diagnosticsPrevious),
                                      L"%s\\%s", directory, DIAGNOSTICS_PREVIOUS_FILE)))
            OpenFileLocked();
    }
    return primaryError;
}

static void WriteLocked(const char *line)
{
    DWORD length = (DWORD)strlen(line), written;
    if (g_diagnosticsFile == INVALID_HANDLE_VALUE) {
        if (!g_diagnosticsPath[0] || !OpenFileLocked()) {
            OutputDebugStringA(line);
            return;
        }
    }
    if (g_diagnosticsBytes + length > DIAGNOSTICS_MAX_BYTES) {
        CloseHandle(g_diagnosticsFile);
        g_diagnosticsFile = INVALID_HANDLE_VALUE;
        /* Retain only the current file and one previous file. Never grow an
           already full file if rotation fails (e.g. a restrictive reader). */
        if (!MoveFileExW(g_diagnosticsPath, g_diagnosticsPrevious, MOVEFILE_REPLACE_EXISTING) ||
            !OpenFileLocked()) {
            /* Reopen without appending so the next entry can retry rotation
               after a reader releases its restrictive file-sharing handle. */
            if (g_diagnosticsFile == INVALID_HANDLE_VALUE) OpenFileLocked();
            OutputDebugStringA("Lumos diagnostics: log rotation failed.\n");
            OutputDebugStringA(line);
            return;
        }
    }
    written = 0;
    BOOL ok = WriteFile(g_diagnosticsFile, line, length, &written, NULL);
    g_diagnosticsBytes += written;
    if (!ok || written != length)
        OutputDebugStringA("Lumos diagnostics: writing the log failed.\n");
    /* WriteFile has no CRT buffering: readers see entries immediately, without
       forcing a physical disk flush on the UI thread for every message. */
}

static void Emit(const BrightMonitor *monitor, const char *level, const char *scope,
                 const char *format, va_list args)
{
    DWORD previousError = GetLastError();
    char message[2048], tag[1664] = "", line[4096];
    vsnprintf(message, sizeof(message), format, args);
    message[sizeof(message) - 1] = '\0';
    for (char *p = message; *p; p++) if (*p == '\r' || *p == '\n') *p = ' ';
    if (monitor) {
        char name[512], identity[1024];
        const char *backend = monitor->backend == BACKEND_DDC ? "DDC" :
                              monitor->backend == BACKEND_WMI ? "WMI" : "NONE";
        Utf8(monitor->name, name, sizeof(name));
        Utf8(monitor->backend == BACKEND_WMI ? monitor->wmiInstance : monitor->deviceInstance,
             identity, sizeof(identity));
        StringCchPrintfA(tag, ARRAYSIZE(tag), " [monitor=\"%s\" id=\"%s:%s\" hMonitor=%p hPhysical=%p]",
                         name, backend, identity, (void *)monitor->hMonitor, monitor->hPhysical);
    }
    SYSTEMTIME time;
    GetLocalTime(&time);
    StringCchPrintfA(line, ARRAYSIZE(line),
        "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [pid=%lu tid=%lu] [%s] [%s]%s %s\r\n",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
        time.wMilliseconds, GetCurrentProcessId(), GetCurrentThreadId(), level, scope, tag, message);
    AcquireSRWLockExclusive(&g_diagnosticsLock);
    if (!g_diagnosticsClosed) {
        if (!g_diagnosticsOpened) {
            g_diagnosticsOpened = TRUE;
            DWORD primaryError = OpenLocked();
            char path[MAX_PATH * 4], header[1536];
            Utf8(g_diagnosticsPath, path, sizeof(path));
            StringCchPrintfA(header, ARRAYSIZE(header),
                "[diagnostics] path=\"%s\" maxBytes=5242880 previousFiles=1 exeFolderError=0x%08lX\r\n",
                path, primaryError);
            WriteLocked(header);
        }
        WriteLocked(line);
    }
    ReleaseSRWLockExclusive(&g_diagnosticsLock);
    SetLastError(previousError);
}

void Diagnostics_Log(const char *level, const char *scope, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    Emit(NULL, level, scope, format, args);
    va_end(args);
}

void Diagnostics_Monitor(const BrightMonitor *monitor, const char *level,
                         const char *scope, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    Emit(monitor, level, scope, format, args);
    va_end(args);
}

void Diagnostics_Init(void)
{
    Diagnostics_Log("INFO", "session", "START build=%s %s diagnostics=enabled", __DATE__, __TIME__);
}

void Diagnostics_Close(void)
{
    DWORD previousError = GetLastError();
    Diagnostics_Log("INFO", "session", "END");
    AcquireSRWLockExclusive(&g_diagnosticsLock);
    g_diagnosticsClosed = TRUE;
    if (g_diagnosticsFile != INVALID_HANDLE_VALUE) CloseHandle(g_diagnosticsFile);
    g_diagnosticsFile = INVALID_HANDLE_VALUE;
    ReleaseSRWLockExclusive(&g_diagnosticsLock);
    SetLastError(previousError);
}

const char *Diagnostics_InputName(DWORD input)
{
    switch (input) {
    case 0: return "unknown/unassigned";
    case 1: return "VGA1";
    case 3: return "DVI1";
    case 4: return "DVI2";
    case 15: return "DisplayPort1";
    case 16: return "DisplayPort2";
    case 17: return "HDMI1";
    case 18: return "HDMI2";
    default: return "vendor/other";
    }
}

const char *Diagnostics_WritePurpose(MonitorWritePurpose purpose)
{
    return purpose == MONITOR_WRITE_IDLE ? "idle-dim" :
           purpose == MONITOR_WRITE_IDLE_RELEASE ? "restore-on-other-input" : "normal";
}

const char *Diagnostics_SourceReason(const BrightMonitor *monitor)
{
    if (monitor->backend == BACKEND_WMI || !monitor->sourceFilter) return "filter-off";
    if (!monitor->expectedInput || monitor->expectedInput > 255) return "PC-input-unassigned";
    if (!monitor->sourceKnown) return "input-unavailable";
    return monitor->currentInput == monitor->expectedInput ? "showing-this-PC" : "other-input";
}

void Diagnostics_MonitorState(const BrightMonitor *monitor, const char *reason)
{
    DWORD previousError = GetLastError();
    ULONGLONG age = monitor->sourceCheckedTick ? GetTickCount64() - monitor->sourceCheckedTick : 0;
    Diagnostics_Monitor(monitor, "INFO", "state",
        "%s selected=%d controllable=%d hasHandle=%d idleMode=%s configuredRange=%d..%d "
        "raw=%lu nativeRange=%lu..%lu filter=%d expected=0x%02lX(%s) current=0x%02lX(%s) "
        "sourceKnown=%d ageMs=%llu sourcePolicy=%s desiredValid=%d desired=%lu "
        "idleApplied=%d dimPending=%d releasePending=%d baselineValid=%d baselineRaw=%lu epoch=%lu",
        reason, !monitor->excludedFromControl, monitor->controllable, monitor->hasHandle,
        monitor->idleBlack ? "true-black" : "brightness", monitor->rangeLo, monitor->rangeHi,
        monitor->brightnessCur, monitor->brightnessMin, monitor->brightnessMax,
        monitor->sourceFilter, monitor->expectedInput, Diagnostics_InputName(monitor->expectedInput),
        monitor->currentInput, Diagnostics_InputName(monitor->currentInput), monitor->sourceKnown,
        (unsigned long long)age, Diagnostics_SourceReason(monitor), monitor->desiredBrightnessValid,
        monitor->desiredBrightness, monitor->idleApplied, monitor->idleDimPending,
        monitor->idleReleasePending, monitor->preIdleBrightnessValid, monitor->preIdleBrightness,
        monitor->idleEpoch);
    SetLastError(previousError);
}
