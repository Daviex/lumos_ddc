#include "monitor.h"
#include "brightness.h"
#include "brightmap.h"
#include "wmibright.h"
#include "monitor_worker.h"
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <strsafe.h>

typedef struct HandleLease {
    HANDLE handle;
    unsigned references;
    struct HandleLease *next;
} HandleLease;

static SRWLOCK g_leaseLock = SRWLOCK_INIT;
/* Serializes native handle acquisition/destruction, never taken by UI setters. */
static SRWLOCK g_acquireLock = SRWLOCK_INIT;
static HandleLease *g_leases;

static BOOL TrackHandle(HANDLE handle)
{
    HandleLease *lease;
    AcquireSRWLockExclusive(&g_leaseLock);
    for (lease = g_leases; lease; lease = lease->next)
        if (lease->handle == handle) break;
    if (!lease) {
        lease = (HandleLease *)malloc(sizeof(*lease));
        if (lease) {
            lease->handle = handle;
            lease->references = 0;
            lease->next = g_leases;
            g_leases = lease;
        }
    }
    if (lease) ++lease->references;
    ReleaseSRWLockExclusive(&g_leaseLock);
    return lease != NULL;
}

void Monitor_Retain(const BrightMonitor *mon)
{
    if (!mon->hasHandle) return;
    AcquireSRWLockExclusive(&g_leaseLock);
    for (HandleLease *lease = g_leases; lease; lease = lease->next) {
        if (lease->handle == mon->hPhysical) {
            ++lease->references;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_leaseLock);
}

void Monitor_Release(const BrightMonitor *mon)
{
    if (!mon->hasHandle) return;
    AcquireSRWLockExclusive(&g_leaseLock);
    for (HandleLease *lease = g_leases; lease; lease = lease->next) {
        if (lease->handle == mon->hPhysical) {
            if (lease->references > 0) --lease->references;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_leaseLock);
    MonitorWorker_Wake();
}

void Monitor_FlushRetiredHandles(void)
{
    /* A hung rescan must not prevent writes through still-live handles. */
    if (!TryAcquireSRWLockExclusive(&g_acquireLock)) return;
    for (;;) {
        HandleLease *retired = NULL;
        AcquireSRWLockExclusive(&g_leaseLock);
        HandleLease **link = &g_leases;
        while (*link) {
            if ((*link)->references == 0) {
                retired = *link;
                *link = retired->next;
                break;
            }
            link = &(*link)->next;
        }
        ReleaseSRWLockExclusive(&g_leaseLock);
        if (!retired) break;
        DestroyPhysicalMonitor(retired->handle);
        free(retired);
    }
    ReleaseSRWLockExclusive(&g_acquireLock);
}

/* ---- Logging (only in debug builds: -DDEBUG) ---- */

#ifdef DEBUG

#include <shlobj.h>

static FILE *g_logFile = NULL;
static SRWLOCK g_logLock = SRWLOCK_INIT;

/* The log goes to %APPDATA%\Lumos, next to config.ini, and not next to the
   exe: the app normally lives under Program Files, where a non-elevated
   process cannot create files, and a 64-bit process gets no VirtualStore
   redirection either, so an exe-relative log silently never appears. */
static void LogOpen(void)
{
    if (g_logFile) return;
    WCHAR path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, path))) {
        if (FAILED(StringCchCatW(path, MAX_PATH, L"\\Lumos"))) return;
        CreateDirectoryW(path, NULL);
        if (FAILED(StringCchCatW(path, MAX_PATH, L"\\lumos-ddc.log"))) return;
    } else {
        wcscpy(path, L".\\lumos-ddc.log");
    }
    g_logFile = _wfopen(path, L"a");
}

static void Log(const char *fmt, ...)
{
    AcquireSRWLockExclusive(&g_logLock);
    if (!g_logFile) LogOpen();
    if (!g_logFile) { ReleaseSRWLockExclusive(&g_logLock); return; }

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_logFile, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_logFile, fmt, ap);
    va_end(ap);

    fprintf(g_logFile, "\n");
    fflush(g_logFile);
    ReleaseSRWLockExclusive(&g_logLock);
}

static void LogW(const char *prefix, const WCHAR *wstr)
{
    AcquireSRWLockExclusive(&g_logLock);
    if (!g_logFile) LogOpen();
    if (!g_logFile) { ReleaseSRWLockExclusive(&g_logLock); return; }

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_logFile, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] %s: %ls\n",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            prefix, wstr);
    fflush(g_logFile);
    ReleaseSRWLockExclusive(&g_logLock);
}

#else
#define Log(...) ((void)0)
#define LogW(...) ((void)0)
#endif

/* ---- Friendly monitor name via EnumDisplayDevices ---- */

/* Parse monitor name from EDID block (descriptor tag 0xFC) */
static BOOL ParseEdidName(const BYTE *edid, DWORD edidLen, WCHAR *out, int outLen)
{
    if (edidLen < 128) return FALSE;

    /* EDID descriptors start at offset 54, each is 18 bytes, 4 descriptors */
    for (int i = 0; i < 4; i++) {
        int off = 54 + i * 18;
        if (off + 18 > (int)edidLen) break;

        /* Monitor name descriptor: bytes 0-2 = 0x00, byte 3 = 0xFC */
        if (edid[off] == 0x00 && edid[off+1] == 0x00 &&
            edid[off+2] == 0x00 && edid[off+3] == 0xFC) {
            /* Name is at offset+5, up to 13 chars, terminated by 0x0A */
            int j = 0;
            for (int k = 5; k < 18 && j < outLen - 1; k++) {
                BYTE c = edid[off + k];
                if (c == 0x0A || c == 0x00) break;
                out[j++] = (WCHAR)c;
            }
            /* Trim trailing spaces */
            while (j > 0 && out[j-1] == L' ') j--;
            out[j] = L'\0';
            return j > 0;
        }
    }
    return FALSE;
}

/* Get friendly monitor name by reading EDID from SetupAPI registry */
static void GetFriendlyName(HMONITOR hMon, WCHAR *out, int outLen)
{
    out[0] = L'\0';

    MONITORINFOEXW mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, (MONITORINFO *)&mi))
        return;

    /* Get the monitor's DeviceID via EnumDisplayDevices */
    DISPLAY_DEVICEW dd;
    memset(&dd, 0, sizeof(dd));
    dd.cb = sizeof(dd);
    if (!EnumDisplayDevicesW(mi.szDevice, 0, &dd, EDD_GET_DEVICE_INTERFACE_NAME))
        return;

    Log("    EnumDD DeviceID: %ls", dd.DeviceID);

    /* Search EDID in registry: HKLM\SYSTEM\CurrentControlSet\Enum\DISPLAY\*\*\Device Parameters\EDID */
    HKEY hEnum;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Enum\\DISPLAY",
                      0, KEY_READ, &hEnum) != ERROR_SUCCESS)
        return;

    /* Extract monitor hardware ID from DeviceID (e.g. "MONITOR\DELA0D4\...") */
    /* DeviceID format: \\?\DISPLAY#XXXX#YYY...#{GUID} — we need XXXX */
    WCHAR hwId[32] = { 0 };
    const WCHAR *p = dd.DeviceID;
    /* Skip to first # */
    while (*p && *p != L'#') p++;
    if (*p == L'#') p++;
    /* Copy until next # */
    int idx = 0;
    while (*p && *p != L'#' && idx < 31) hwId[idx++] = *p++;
    hwId[idx] = L'\0';

    Log("    EDID lookup hwId: %ls", hwId);

    if (hwId[0] == L'\0') { RegCloseKey(hEnum); return; }

    /* Open DISPLAY\<hwId> */
    HKEY hMon2;
    if (RegOpenKeyExW(hEnum, hwId, 0, KEY_READ, &hMon2) != ERROR_SUCCESS) {
        RegCloseKey(hEnum);
        return;
    }

    /* Enumerate instance subkeys */
    WCHAR subkey[256];
    for (DWORD i2 = 0; RegEnumKeyW(hMon2, i2, subkey, 256) == ERROR_SUCCESS; i2++) {
        WCHAR path[512];
        wsprintfW(path, L"%s\\Device Parameters", subkey);

        HKEY hParams;
        if (RegOpenKeyExW(hMon2, path, 0, KEY_READ, &hParams) == ERROR_SUCCESS) {
            BYTE edid[256];
            DWORD edidLen = sizeof(edid);
            DWORD type = 0;
            if (RegQueryValueExW(hParams, L"EDID", NULL, &type, edid, &edidLen) == ERROR_SUCCESS
                && type == REG_BINARY && edidLen >= 128) {
                WCHAR name[64] = { 0 };
                if (ParseEdidName(edid, edidLen, name, 64) && name[0] != L'\0') {
                    wcsncpy(out, name, outLen - 1);
                    out[outLen - 1] = L'\0';
                    RegCloseKey(hParams);
                    RegCloseKey(hMon2);
                    RegCloseKey(hEnum);
                    return;
                }
            }
            RegCloseKey(hParams);
        }
    }

    RegCloseKey(hMon2);
    RegCloseKey(hEnum);
}

/* ---- Internal-panel matching (WMI) ---- */

/*
 * Build the normalized PnP instance key for a monitor, e.g.
 *   DISPLAY\LEN40BA\5&9a2b9bb&4&UID256
 * from the device interface path returned by EnumDisplayDevices. This is the
 * same key WMI exposes as InstanceName (minus a trailing "_N" collection
 * index), so it lets us match a display to its WMI panel generically, without
 * any hardcoded vendor or product IDs.
 */
static BOOL GetMonitorInstanceKey(HMONITOR hMon, WCHAR *out, int outLen)
{
    out[0] = L'\0';

    MONITORINFOEXW mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, (MONITORINFO *)&mi))
        return FALSE;

    DISPLAY_DEVICEW dd;
    memset(&dd, 0, sizeof(dd));
    dd.cb = sizeof(dd);
    if (!EnumDisplayDevicesW(mi.szDevice, 0, &dd, EDD_GET_DEVICE_INTERFACE_NAME))
        return FALSE;

    /* DeviceID like: \\?\DISPLAY#LEN40BA#5&9a2b9bb&4&UID256#{guid} */
    const WCHAR *p = wcsstr(dd.DeviceID, L"DISPLAY#");
    if (!p) return FALSE;

    int hashes = 0, j = 0;
    for (; *p && j < outLen - 1; p++) {
        if (*p == L'#') {
            if (++hashes >= 3) break;   /* stop before the interface GUID */
            out[j++] = L'\\';
        } else {
            out[j++] = *p;
        }
    }
    out[j] = L'\0';
    return j > 0;
}

/* Copy a WMI InstanceName, stripping a trailing "_<digits>" collection index. */
static void NormalizeWmiInstance(const WCHAR *in, WCHAR *out, int outLen)
{
    wcsncpy(out, in, outLen - 1);
    out[outLen - 1] = L'\0';

    int n = (int)wcslen(out);
    int k = n;
    while (k > 0 && out[k - 1] >= L'0' && out[k - 1] <= L'9')
        k--;
    if (k > 0 && k < n && out[k - 1] == L'_')
        out[k - 1] = L'\0';
}

/* ---- Enumeration ---- */

typedef struct {
    MonitorList *ml;
    WmiPanel     wmiPanels[MAX_MONITORS];
    int          wmiCount;
} EnumCtx;

/* If this monitor matches a WMI panel, configure it as a WMI-backed internal
 * panel and return TRUE. Not device specific: matches purely by instance key. */
static BOOL TryAttachWmiPanel(EnumCtx *ctx, HMONITOR hMon, BrightMonitor *bm)
{
    if (ctx->wmiCount == 0)
        return FALSE;

    WCHAR monKey[256];
    if (!GetMonitorInstanceKey(hMon, monKey, 256))
        return FALSE;

    for (int i = 0; i < ctx->wmiCount; i++) {
        WCHAR wmiKey[256];
        NormalizeWmiInstance(ctx->wmiPanels[i].instanceName, wmiKey, 256);
        if (_wcsicmp(monKey, wmiKey) == 0 && ctx->wmiPanels[i].currentBrightness <= 100) {
            bm->backend = BACKEND_WMI;
            bm->controllable = TRUE;
            bm->brightnessMin = 0;
            bm->brightnessMax = 100;
            bm->brightnessCur = ctx->wmiPanels[i].currentBrightness;
            wcsncpy(bm->wmiInstance, ctx->wmiPanels[i].instanceName, 255);
            bm->wmiInstance[255] = L'\0';
            Log("    Matched WMI panel: %ls (cur=%lu)", bm->wmiInstance, bm->brightnessCur);
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL CALLBACK MonitorEnumProc(HMONITOR hMon, HDC hdcMon, LPRECT lpRect, LPARAM dwData)
{
    EnumCtx *ctx = (EnumCtx *)dwData;
    MonitorList *ml = ctx->ml;
    DWORD numPhysical = 0;

    (void)hdcMon;
    (void)lpRect;

    Log("EnumProc: hMonitor=%p rect=(%ld,%ld)-(%ld,%ld)",
        (void *)hMon, lpRect->left, lpRect->top, lpRect->right, lpRect->bottom);

    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hMon, &numPhysical)) {
        Log("  GetNumberOfPhysicalMonitors FAILED, err=%lu", GetLastError());
        return TRUE;
    }
    Log("  numPhysical=%lu", numPhysical);
    if (numPhysical == 0)
        return TRUE;

    PHYSICAL_MONITOR *phys = (PHYSICAL_MONITOR *)calloc(numPhysical, sizeof(PHYSICAL_MONITOR));
    if (!phys) {
        Log("  calloc failed for %lu monitors", numPhysical);
        return TRUE;
    }

    BOOL *tracked = (BOOL *)calloc(numPhysical, sizeof(BOOL));
    if (!tracked) { free(phys); return TRUE; }
    AcquireSRWLockExclusive(&g_acquireLock);
    BOOL acquired = GetPhysicalMonitorsFromHMONITOR(hMon, numPhysical, phys);
    if (acquired) {
        for (DWORD i = 0; i < numPhysical; i++) {
            tracked[i] = TrackHandle(phys[i].hPhysicalMonitor);
            if (!tracked[i]) DestroyPhysicalMonitor(phys[i].hPhysicalMonitor);
        }
    }
    ReleaseSRWLockExclusive(&g_acquireLock);
    MonitorWorker_Wake();
    if (acquired) {
        for (DWORD i = 0; i < numPhysical && ml->count < MAX_MONITORS; i++) {
            if (!tracked[i]) continue;
            LogW("  Physical monitor", phys[i].szPhysicalMonitorDescription);
            Log("    hPhysical=%p", phys[i].hPhysicalMonitor);

            /* GetMonitorCapabilities is deliberately not called. It was only a
               hint, since GetMonitorBrightness is tried below in every case, it
               costs close to a second on monitors that answer it with an error,
               and it is where the enumeration hung for minutes when a
               DisplayPort link came back after the display had slept. */
            BrightMonitor *bm = &ml->monitors[ml->count];
            bm->hPhysical = phys[i].hPhysicalMonitor;
            bm->hasHandle = TRUE;
            bm->hMonitor = hMon;
            GetMonitorInstanceKey(hMon, bm->deviceInstance, ARRAYSIZE(bm->deviceInstance));
            bm->rangeLo = 0;     /* until Settings_ApplyRanges finds a saved one */
            bm->rangeHi = 100;

            /* Try to get friendly name from EnumDisplayDevices */
            WCHAR friendly[128] = { 0 };
            GetFriendlyName(hMon, friendly, 128);

            if (friendly[0] != L'\0' && wcscmp(friendly, L"Generic PnP Monitor") != 0) {
                wcsncpy(bm->name, friendly, 127);
            } else {
                wcsncpy(bm->name, phys[i].szPhysicalMonitorDescription, 127);
            }
            bm->name[127] = L'\0';
            LogW("    Friendly name", bm->name);

            /* Physical DDC identity/source access survives a failed brightness
               read. Brightness capability is recovered independently later. */
            bm->backend = BACKEND_DDC;
            bm->controllable = FALSE;
            bm->wmiInstance[0] = L'\0';

            if (Monitor_ReadBrightnessSync(bm)) {
                Log("    Brightness: min=%lu cur=%lu max=%lu",
                    bm->brightnessMin, bm->brightnessCur, bm->brightnessMax);
            } else if (TryAttachWmiPanel(ctx, hMon, bm)) {
                /* Internal laptop panel: no DDC, but WMI backlight works. */
                Log("    Using WMI backlight backend");
            } else {
                bm->brightnessMin = 0;
                bm->brightnessCur = 50;
                bm->brightnessMax = 100;
                Log("    No brightness control (no DDC, no WMI match, err=%lu)", GetLastError());
            }

            if (bm->backend == BACKEND_DDC)
                Monitor_ReadSourceSync(bm);

            ml->count++;
            tracked[i] = FALSE; /* ownership transferred to the monitor list */
        }
    } else {
        Log("  GetPhysicalMonitorsFromHMONITOR FAILED, err=%lu", GetLastError());
    }

    for (DWORD i = 0; i < numPhysical; i++) {
        if (tracked[i]) {
            BrightMonitor excess = { 0 };
            excess.hasHandle = TRUE;
            excess.hPhysical = phys[i].hPhysicalMonitor;
            Monitor_Release(&excess);
        }
    }

    free(tracked);
    free(phys);
    return TRUE;
}

void Monitor_Enumerate(MonitorList *ml)
{
    Monitor_Cleanup(ml);
    ml->count = 0;
    ml->active = 0;

    Log("=== Monitor_Enumerate START ===");

    EnumCtx ctx;
    ctx.ml = ml;
    ctx.wmiCount = Wmi_QueryPanels(ctx.wmiPanels, MAX_MONITORS);
    Log("  WMI panels reported: %d", ctx.wmiCount);
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, (LPARAM)&ctx);

    Log("=== Monitor_Enumerate END: %d monitors found ===", ml->count);

    if (ml->count == 0) {
        BrightMonitor *bm = &ml->monitors[0];
        bm->hPhysical = NULL;
        bm->hasHandle = FALSE;
        bm->hMonitor = NULL;
        wcscpy(bm->name, L"No DDC/CI monitors found");
        bm->rangeLo = 0;
        bm->rangeHi = 100;
        bm->brightnessMin = 0;
        bm->brightnessCur = 0;
        bm->brightnessMax = 100;
        bm->controllable = FALSE;
        bm->backend = BACKEND_NONE;
        bm->wmiInstance[0] = L'\0';
        ml->count = 1;
        Log("  Added dummy entry (no monitors)");
    }
}

void Monitor_Cleanup(MonitorList *ml)
{
    /* Other lists and in-flight requests retain their own leases. */
    for (int i = 0; i < ml->count; i++)
        Monitor_Release(&ml->monitors[i]);
    memset(ml, 0, sizeof(*ml));
}

BOOL Monitor_ReadBrightnessSync(BrightMonitor *bm)
{
    if (bm->backend == BACKEND_DDC && bm->hasHandle) {
        DWORD minimum, current, maximum;
        BOOL ok = GetMonitorBrightness(bm->hPhysical,
                                       &minimum, &current, &maximum);
        if (ok && maximum > minimum && current >= minimum && current <= maximum) {
            bm->brightnessMin = minimum;
            bm->brightnessCur = current;
            bm->brightnessMax = maximum;
            bm->controllable = TRUE;
            bm->awaitingAnswer = FALSE;
            return TRUE;
        }
    } else if (bm->backend == BACKEND_WMI) {
        DWORD pct = 0;
        if (Wmi_GetBrightness(bm->wmiInstance, &pct) && pct <= 100) {
            bm->brightnessCur = pct;   /* WMI range is fixed 0-100 */
            bm->controllable = TRUE;
            bm->awaitingAnswer = FALSE;
            return TRUE;
        }
    }
    /* Transient failures cannot revoke previously validated capability/values. */
    return FALSE;
}

DWORD Monitor_RefreshBrightnessSync(MonitorList *ml)
{
    DWORD readMask = 0;
    for (int i = 0; i < ml->count; i++) {
        if (Monitor_ReadBrightnessSync(&ml->monitors[i])) readMask |= 1u << i;
        else Log("RefreshBrightness[%d] FAILED, err=%lu", i, GetLastError());
    }
    return readMask;
}

BOOL Monitor_ReadSourceSync(BrightMonitor *mon)
{
    DWORD input = 0, maximum = 0;
    MC_VCP_CODE_TYPE type;
    BOOL ok = mon->backend == BACKEND_DDC && mon->hasHandle &&
              GetVCPFeatureAndVCPFeatureReply(mon->hPhysical, 0x60, &type,
                                             &input, &maximum);
    /* Some displays reject back-to-back DDC commands with a transient error.
       Retry once after a short pause; never reuse a cached match to authorize a
       write. This runs on the worker (or initial enumeration), not the UI. */
    if (!ok && mon->backend == BACKEND_DDC && mon->hasHandle) {
        Sleep(100);
        ok = GetVCPFeatureAndVCPFeatureReply(mon->hPhysical, 0x60, &type,
                                          &input, &maximum);
    }
    mon->sourceKnown = ok && input > 0 && input <= 255;
    mon->currentInput = mon->sourceKnown ? input : 0;
    mon->sourceCheckedTick = GetTickCount64();
    return mon->sourceKnown;
}

MonitorWriteOutcome Monitor_SetBrightnessForPurposeGuardedSync(
    BrightMonitor *mon, DWORD value, MonitorWritePurpose purpose, DWORD otherInput,
    MonitorWriteGuard guard, void *context, BOOL *sourceUpdated)
{
    BOOL captureBaseline = purpose == MONITOR_WRITE_IDLE && !mon->preIdleBrightnessValid;
    DWORD baseline = 0;
    if (sourceUpdated) *sourceUpdated = FALSE;
    if (!Monitor_CanControl(mon))
        return MONITOR_WRITE_SKIPPED;

    if (purpose == MONITOR_WRITE_IDLE_RELEASE &&
        (mon->backend != BACKEND_DDC || !mon->sourceFilter ||
         !mon->expectedInput || mon->expectedInput > 255 ||
         !otherInput || otherInput > 255 || otherInput == mon->expectedInput))
        return MONITOR_WRITE_SKIPPED;

    DWORD percent = value > 100 ? 100 : value;

    if (mon->backend == BACKEND_WMI) {
        if (captureBaseline && (!Wmi_GetBrightness(mon->wmiInstance, &baseline) || baseline > 100))
            return MONITOR_WRITE_FAILED;
        if (guard && !guard(context)) return MONITOR_WRITE_CANCELLED;
        BOOL ok = Wmi_SetBrightness(mon->wmiInstance, percent);
        Log("SetBrightness(WMI): '%ls' pct=%lu -> %s", mon->name, percent, ok ? "OK" : "FAILED");
        if (ok) {
            mon->brightnessCur = percent;   /* WMI range is fixed 0-100 */
            if (captureBaseline) {
                mon->preIdleBrightness = baseline;
                mon->preIdleBrightnessValid = TRUE;
            }
        }
        return ok ? MONITOR_WRITE_APPLIED : MONITOR_WRITE_FAILED;
    }

    if (!mon->hasHandle || mon->backend != BACKEND_DDC ||
        mon->brightnessMax <= mon->brightnessMin)
        return MONITOR_WRITE_FAILED;

    if (captureBaseline) {
        DWORD minimum, maximum;
        if (!GetMonitorBrightness(mon->hPhysical, &minimum, &baseline, &maximum) ||
            maximum <= minimum || baseline < minimum || baseline > maximum)
            return MONITOR_WRITE_FAILED;
        mon->brightnessMin = minimum;
        mon->brightnessMax = maximum;
    }

    if (mon->sourceFilter) {
        Monitor_ReadSourceSync(mon);
        if (sourceUpdated) *sourceUpdated = TRUE;
    }
    if (guard && !guard(context)) return MONITOR_WRITE_CANCELLED;
    if (purpose == MONITOR_WRITE_IDLE_RELEASE) {
        if (!mon->sourceKnown || mon->currentInput != otherInput)
            return MONITOR_WRITE_SKIPPED;
    } else if (!Monitor_SourceAllowsControl(mon)) return MONITOR_WRITE_SKIPPED;

    if (purpose == MONITOR_WRITE_IDLE_RELEASE) {
        if (value < mon->brightnessMin || value > mon->brightnessMax)
            return MONITOR_WRITE_FAILED;
    } else {
        value = Brightness_ToRaw(mon, percent);
    }

    Log("SetBrightness: '%ls' pct=%lu val=%lu (range %lu-%lu) hPhys=%p",
        mon->name, percent, value, mon->brightnessMin, mon->brightnessMax, mon->hPhysical);

    BOOL ok = SetMonitorBrightness(mon->hPhysical, value);
    if (ok) {
        mon->brightnessCur = value;
        if (captureBaseline) {
            mon->preIdleBrightness = baseline;
            mon->preIdleBrightnessValid = TRUE;
        }
        Log("  -> OK");
    } else {
        Log("  -> FAILED, err=%lu", GetLastError());
    }
    return ok ? MONITOR_WRITE_APPLIED : MONITOR_WRITE_FAILED;
}

MonitorWriteOutcome Monitor_SetBrightnessGuardedSync(BrightMonitor *mon, DWORD percent,
                                                      MonitorWriteGuard guard, void *context)
{
    return Monitor_SetBrightnessForPurposeGuardedSync(mon, percent, MONITOR_WRITE_NORMAL,
                                                     0, guard, context, NULL);
}

BOOL Monitor_SetBrightnessSync(BrightMonitor *mon, DWORD percent)
{
    return Monitor_SetBrightnessGuardedSync(mon, percent, NULL, NULL) == MONITOR_WRITE_APPLIED;
}

void Monitor_PreviewBrightness(BrightMonitor *mon, DWORD percent)
{
    if (!Monitor_CanControl(mon)) return;
    /* A filtered request may be skipped after its fresh source read. */
    if (mon->sourceFilter && mon->backend != BACKEND_WMI) return;
    mon->brightnessCur = Brightness_ToRaw(mon, percent);
}

BOOL Monitor_SetBrightness(BrightMonitor *mon, DWORD percent)
{
    if (!Monitor_CanControl(mon)) return FALSE;
    if (percent > 100) percent = 100;
    mon->desiredBrightnessValid = TRUE;
    mon->desiredBrightness = percent;
    if (!MonitorWorker_Running()) {
        BOOL applied = Monitor_SetBrightnessSync(mon, percent);
        if (applied) {
            mon->idleApplied = FALSE;
            mon->idleDimPending = FALSE;
            mon->idleReleasePending = FALSE;
        }
        return applied;
    }
    if (!MonitorWorker_Set(mon, percent)) return FALSE;
    Monitor_PreviewBrightness(mon, percent);
    return TRUE;
}

BOOL Monitor_SetIdleBrightness(BrightMonitor *mon, DWORD percent)
{
    if (!Monitor_CanControl(mon)) return FALSE;
    if (percent > 100) percent = 100;
    if (!MonitorWorker_Running())
        return Monitor_SetBrightnessForPurposeGuardedSync(mon, percent, MONITOR_WRITE_IDLE,
                                                          0, NULL, NULL, NULL) == MONITOR_WRITE_APPLIED;
    if (!MonitorWorker_SetIdle(mon, percent)) return FALSE;
    Monitor_PreviewBrightness(mon, percent);
    return TRUE;
}

BOOL Monitor_ReleaseIdleBrightness(BrightMonitor *mon, DWORD rawBrightness, DWORD otherInput)
{
    if (!Monitor_CanControl(mon)) return FALSE;
    if (!MonitorWorker_Running())
        return Monitor_SetBrightnessForPurposeGuardedSync(mon, rawBrightness, MONITOR_WRITE_IDLE_RELEASE,
                                                          otherInput, NULL, NULL, NULL) == MONITOR_WRITE_APPLIED;
    return MonitorWorker_ReleaseIdle(mon, rawBrightness, otherInput);
}

void Monitor_RefreshBrightness(MonitorList *ml)
{
    if (MonitorWorker_Running()) MonitorWorker_Refresh(ml);
    else Monitor_RefreshBrightnessSync(ml);
}

BOOL Monitor_HasControllable(const MonitorList *ml)
{
    for (int i = 0; i < ml->count; i++)
        if (ml->monitors[i].controllable)
            return TRUE;
    return FALSE;
}

/* Prefer stable PnP identity, which survives reordered/renamed displays.
   Without one, pair equal names by occurrence as upstream does. Known but
   different PnP keys must never be paired merely because names match. */
static const BrightMonitor *FindPrevious(const MonitorList *prev,
                                         const BrightMonitor *monitor,
                                         const BOOL matched[MAX_MONITORS],
                                         BOOL identityOnly)
{
    for (int j = 0; j < prev->count; j++) {
        const BrightMonitor *candidate = &prev->monitors[j];
        if (matched[j]) continue;
        if (identityOnly) {
            if (monitor->deviceInstance[0] &&
                _wcsicmp(candidate->deviceInstance, monitor->deviceInstance) == 0)
                return candidate;
        } else if (!(monitor->deviceInstance[0] && candidate->deviceInstance[0]) &&
                   wcscmp(candidate->name, monitor->name) == 0) {
            return candidate;
        }
    }
    return NULL;
}

int Monitor_TrackUnanswered(MonitorList *fresh, const MonitorList *prev, unsigned *recovered)
{
    const BrightMonitor *match[MAX_MONITORS] = { NULL };
    BOOL prevMatched[MAX_MONITORS] = { FALSE };
    /* Match every known identity before considering name-only stand-ins,
       otherwise an unknown identical model could consume a known match. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < fresh->count; i++) {
            if (match[i]) continue;
            match[i] = FindPrevious(prev, &fresh->monitors[i], prevMatched, pass == 0);
            if (match[i])
                prevMatched[match[i] - prev->monitors] = TRUE;
        }
    }

    /* A monitor can come back from sleep under another name for a moment
       (Windows reports a "Digital Flat Panel" stand-in while the link trains).
       Count the monitors of *prev that worked, or were waiting, and have no
       namesake now, so an unknown monitor in their place is paired with them
       instead of being taken for a new one that never answered. */
    int lostWorking = 0, lostWaiting = 0;
    for (int j = 0; j < prev->count; j++) {
        if (prevMatched[j] || prev->monitors[j].excludedFromControl) continue;
        if (prev->monitors[j].awaitingAnswer) lostWaiting++;
        else if (prev->monitors[j].controllable) lostWorking++;
    }

    int waiting = 0;
    *recovered = 0;
    for (int i = 0; i < fresh->count; i++) {
        BrightMonitor *mon = &fresh->monitors[i];
        const BrightMonitor *old = match[i];
        BOOL answeredAgain = FALSE, stillAwaited = FALSE;
        if (mon->excludedFromControl) {
            mon->awaitingAnswer = FALSE;
            continue;
        }
        if (old) {
            answeredAgain = mon->controllable && old->awaitingAnswer;
            stillAwaited = !mon->controllable && (old->controllable || old->awaitingAnswer);
        } else if (mon->deviceInstance[0]) {
            /* A new display with a different stable key is not the old one
               recovering. Stand-ins with unknown identity use the fallback. */
        } else if (mon->controllable) {
            if (lostWaiting > 0) { lostWaiting--; answeredAgain = TRUE; }
        } else if (lostWorking > 0) {
            lostWorking--; stillAwaited = TRUE;
        } else if (lostWaiting > 0) {
            lostWaiting--; stillAwaited = TRUE;
        }
        if (answeredAgain)
            *recovered |= 1u << i;
        mon->awaitingAnswer = stillAwaited;
        if (stillAwaited) {
            waiting++;
        }
    }
    return waiting;
}

BOOL Monitor_SetAllBrightness(MonitorList *ml, int percent)
{
    BOOL allOk = TRUE;
    for (int i = 0; i < ml->count; i++) {
        BrightMonitor *monitor = &ml->monitors[i];
        if (monitor->excludedFromControl) continue;
        int adj = BrightMap_Level(percent, monitor->rangeLo, monitor->rangeHi);
        if (!monitor->controllable) {
            /* Keep the newest master intent for connected/waking displays.
               Recovery must not replay their older per-monitor target; these
               snapshots cannot yet queue a hardware write. */
            if (monitor->awaitingAnswer ||
                (monitor->backend == BACKEND_DDC && monitor->hasHandle)) {
                monitor->desiredBrightnessValid = TRUE;
                monitor->desiredBrightness = (DWORD)adj;
            }
            continue;
        }
        if (!Monitor_SetBrightness(monitor, (DWORD)adj))
            allOk = FALSE;
    }
    return allOk;
}

int Monitor_GetPercent(const BrightMonitor *mon)
{
    return Brightness_GetPercent(mon);
}

int Monitor_MasterFromSnapshot(const MonitorList *ml)
{
    return Brightness_MasterTarget(ml);
}

BOOL Monitor_StepAllBrightness(MonitorList *ml, int delta)
{
    /* Clamp before addition so even an untrusted large step cannot overflow. */
    if (delta < -100) delta = -100;
    if (delta > 100) delta = 100;
    return Monitor_SetAllBrightness(ml, Monitor_MasterFromSnapshot(ml) + delta);
}

void Monitor_AdjustActive(MonitorList *ml, int delta)
{
    if (ml->count == 0) return;
    if (ml->active < 0 || ml->active >= ml->count)
        ml->active = 0;

    BrightMonitor *mon = &ml->monitors[ml->active];
    if (!Monitor_CanControl(mon)) return;

    int pct = Monitor_GetPercent(mon) + delta;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    Log("AdjustActive: monitor[%d] '%ls' delta=%d newPct=%d", ml->active, mon->name, delta, pct);
    Monitor_SetBrightness(mon, (DWORD)pct);
}

void Monitor_CycleActive(MonitorList *ml, int direction)
{
    if (ml->count == 0) return;
    Log("CycleActive: %d dir=%d", ml->active, direction);
    int index = ml->active;
    if (index < 0 || index >= ml->count) index = 0;
    for (int i = 0; i < ml->count; i++) {
        index = (index + (direction < 0 ? ml->count - 1 : 1)) % ml->count;
        if (Monitor_CanControl(&ml->monitors[index])) {
            ml->active = index;
            return;
        }
    }
}
