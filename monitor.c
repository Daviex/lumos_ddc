#include "monitor.h"
#include "brightmap.h"
#include "wmibright.h"
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <stdio.h>

/* ---- Logging (only in debug builds: -DDEBUG) ---- */

#ifdef DEBUG

#include <shlobj.h>

static FILE *g_logFile = NULL;

/* The log goes to %APPDATA%\Lumos, next to config.ini, and not next to the
   exe: the app normally lives under Program Files, where a non-elevated
   process cannot create files, and a 64-bit process gets no VirtualStore
   redirection either, so an exe-relative log silently never appears. */
static void LogOpen(void)
{
    if (g_logFile) return;
    WCHAR path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, path))) {
        wcscat(path, L"\\Lumos");
        CreateDirectoryW(path, NULL);
        wcscat(path, L"\\lumos-ddc.log");
    } else {
        wcscpy(path, L".\\lumos-ddc.log");
    }
    g_logFile = _wfopen(path, L"a");
}

static void Log(const char *fmt, ...)
{
    if (!g_logFile) LogOpen();
    if (!g_logFile) return;

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
}

static void LogW(const char *prefix, const WCHAR *wstr)
{
    if (!g_logFile) LogOpen();
    if (!g_logFile) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_logFile, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] %s: %ls\n",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            prefix, wstr);
    fflush(g_logFile);
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
        if (_wcsicmp(monKey, wmiKey) == 0) {
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

    if (GetPhysicalMonitorsFromHMONITOR(hMon, numPhysical, phys)) {
        for (DWORD i = 0; i < numPhysical && ml->count < MAX_MONITORS; i++) {
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

            /* A monitor that answers a brightness read is controllable, which is
               the only signal we need. */
            bm->backend = BACKEND_NONE;
            bm->controllable = FALSE;
            bm->wmiInstance[0] = L'\0';

            DWORD bMin = 0, bCur = 0, bMax = 0;
            BOOL brOk = GetMonitorBrightness(phys[i].hPhysicalMonitor, &bMin, &bCur, &bMax);

            if (brOk && bMax > bMin) {
                bm->backend = BACKEND_DDC;
                bm->controllable = TRUE;
                bm->brightnessMin = bMin;
                bm->brightnessCur = bCur;
                bm->brightnessMax = bMax;
                Log("    Brightness: min=%lu cur=%lu max=%lu", bMin, bCur, bMax);
            } else if (TryAttachWmiPanel(ctx, hMon, bm)) {
                /* Internal laptop panel: no DDC, but WMI backlight works. */
                Log("    Using WMI backlight backend");
            } else {
                bm->brightnessMin = 0;
                bm->brightnessCur = 50;
                bm->brightnessMax = 100;
                Log("    No brightness control (no DDC, no WMI match, err=%lu)", GetLastError());
            }

            ml->count++;
        }
    } else {
        Log("  GetPhysicalMonitorsFromHMONITOR FAILED, err=%lu", GetLastError());
        for (DWORD i = 0; i < numPhysical; i++)
            DestroyPhysicalMonitor(phys[i].hPhysicalMonitor);
    }

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
    for (int i = 0; i < ml->count; i++) {
        if (ml->monitors[i].hasHandle)
            DestroyPhysicalMonitor(ml->monitors[i].hPhysical);
        ml->monitors[i].hasHandle = FALSE;
    }
    ml->count = 0;
}

void Monitor_RefreshBrightness(MonitorList *ml)
{
    /* Throttle: don't hammer DDC bus more than once per 200ms */
    static DWORD lastRefresh = 0;
    DWORD now = GetTickCount();
    if (now - lastRefresh < 200)
        return;
    lastRefresh = now;

    for (int i = 0; i < ml->count; i++) {
        BrightMonitor *bm = &ml->monitors[i];
        if (bm->backend == BACKEND_DDC && bm->hasHandle) {
            BOOL ok = GetMonitorBrightness(bm->hPhysical,
                                 &bm->brightnessMin,
                                 &bm->brightnessCur,
                                 &bm->brightnessMax);
            if (!ok) {
                Log("RefreshBrightness[%d] FAILED, err=%lu", i, GetLastError());
            }
        } else if (bm->backend == BACKEND_WMI) {
            DWORD pct = 0;
            if (Wmi_GetBrightness(bm->wmiInstance, &pct)) {
                bm->brightnessCur = pct;   /* WMI range is fixed 0-100 */
            } else {
                Log("RefreshBrightness[%d] WMI failed", i);
            }
        }
    }
}

BOOL Monitor_SetBrightness(BrightMonitor *mon, DWORD percent)
{
    if (!mon->controllable)
        return FALSE;

    if (percent > 100) percent = 100;

    if (mon->backend == BACKEND_WMI) {
        BOOL ok = Wmi_SetBrightness(mon->wmiInstance, percent);
        Log("SetBrightness(WMI): '%ls' pct=%lu -> %s", mon->name, percent, ok ? "OK" : "FAILED");
        if (ok)
            mon->brightnessCur = percent;   /* WMI range is fixed 0-100 */
        return ok;
    }

    if (!mon->hasHandle)
        return FALSE;

    DWORD range = mon->brightnessMax - mon->brightnessMin;
    DWORD value = mon->brightnessMin + (range * percent) / 100;

    Log("SetBrightness: '%ls' pct=%lu val=%lu (range %lu-%lu) hPhys=%p",
        mon->name, percent, value, mon->brightnessMin, mon->brightnessMax, mon->hPhysical);

    BOOL ok = SetMonitorBrightness(mon->hPhysical, value);
    if (ok) {
        mon->brightnessCur = value;
        Log("  -> OK");
    } else {
        Log("  -> FAILED, err=%lu", GetLastError());
    }
    return ok;
}

static BOOL HandleReferencedBy(const MonitorList *keep, HANDLE h)
{
    if (!keep) return FALSE;
    for (int i = 0; i < keep->count; i++)
        if (keep->monitors[i].hasHandle && keep->monitors[i].hPhysical == h)
            return TRUE;
    return FALSE;
}

void Monitor_CleanupExcept(MonitorList *ml, const MonitorList *keep)
{
    for (int i = 0; i < ml->count; i++) {
        BrightMonitor *bm = &ml->monitors[i];
        if (!bm->hasHandle)
            continue;
        if (HandleReferencedBy(keep, bm->hPhysical)) {
            /* The other list holds the same handle, so it stays alive and will
               be released when that list is retired. Worst case we leak one
               handle; destroying it would break brightness control outright. */
            Log("Cleanup: keeping handle %p, still referenced", bm->hPhysical);
            continue;
        }
        DestroyPhysicalMonitor(bm->hPhysical);
    }
    memset(ml, 0, sizeof(*ml));
}

BOOL Monitor_HasControllable(const MonitorList *ml)
{
    for (int i = 0; i < ml->count; i++)
        if (ml->monitors[i].controllable)
            return TRUE;
    return FALSE;
}

/* The monitor of *prev that matches monitor i of *ml: same name, and the same
   position among the monitors of that name, so two identical models pair up
   in order instead of both matching the first. */
static const BrightMonitor *FindPrevious(const MonitorList *prev,
                                         const MonitorList *ml, int i)
{
    const WCHAR *name = ml->monitors[i].name;
    int nth = 0;
    for (int j = 0; j < i; j++)
        if (wcscmp(ml->monitors[j].name, name) == 0)
            nth++;
    for (int j = 0; j < prev->count; j++)
        if (wcscmp(prev->monitors[j].name, name) == 0 && nth-- == 0)
            return &prev->monitors[j];
    return NULL;
}

int Monitor_TrackUnanswered(MonitorList *fresh, const MonitorList *prev, unsigned *recovered)
{
    const BrightMonitor *match[MAX_MONITORS];
    BOOL prevMatched[MAX_MONITORS] = { FALSE };
    for (int i = 0; i < fresh->count; i++) {
        match[i] = FindPrevious(prev, fresh, i);
        if (match[i])
            prevMatched[match[i] - prev->monitors] = TRUE;
    }

    /* A monitor can come back from sleep under another name for a moment
       (Windows reports a "Digital Flat Panel" stand-in while the link trains).
       Count the monitors of *prev that worked, or were waiting, and have no
       namesake now, so an unknown monitor in their place is paired with them
       instead of being taken for a new one that never answered. */
    int lostWorking = 0, lostWaiting = 0;
    for (int j = 0; j < prev->count; j++) {
        if (prevMatched[j]) continue;
        if (prev->monitors[j].awaitingAnswer) lostWaiting++;
        else if (prev->monitors[j].controllable) lostWorking++;
    }

    int waiting = 0;
    *recovered = 0;
    for (int i = 0; i < fresh->count; i++) {
        BrightMonitor *mon = &fresh->monitors[i];
        const BrightMonitor *old = match[i];
        BOOL answeredAgain = FALSE, stillAwaited = FALSE;
        if (old) {
            answeredAgain = mon->controllable && old->awaitingAnswer;
            stillAwaited = !mon->controllable && (old->controllable || old->awaitingAnswer);
        } else if (mon->controllable) {
            if (lostWaiting > 0) { lostWaiting--; answeredAgain = TRUE; }
        } else if (lostWorking > 0) {
            lostWorking--; stillAwaited = TRUE;
        } else if (lostWaiting > 0) {
            lostWaiting--; stillAwaited = TRUE;
        }
        if (answeredAgain)
            *recovered |= 1u << i;
        if (stillAwaited) {
            mon->awaitingAnswer = TRUE;
            waiting++;
        }
    }
    return waiting;
}

BOOL Monitor_SetAllBrightness(MonitorList *ml, int percent)
{
    BOOL allOk = TRUE;
    for (int i = 0; i < ml->count; i++) {
        int adj = BrightMap_Level(percent, ml->monitors[i].rangeLo, ml->monitors[i].rangeHi);
        if (!Monitor_SetBrightness(&ml->monitors[i], (DWORD)adj) && ml->monitors[i].controllable)
            allOk = FALSE;
    }
    return allOk;
}

int Monitor_GetPercent(const BrightMonitor *mon)
{
    DWORD range = mon->brightnessMax - mon->brightnessMin;
    if (range == 0) return 0;
    return (int)(((mon->brightnessCur - mon->brightnessMin) * 100) / range);
}

void Monitor_AdjustActive(MonitorList *ml, int delta)
{
    if (ml->count == 0) return;
    if (ml->active < 0 || ml->active >= ml->count)
        ml->active = 0;

    BrightMonitor *mon = &ml->monitors[ml->active];
    if (!mon->controllable) return;

    int pct = Monitor_GetPercent(mon) + delta;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    Log("AdjustActive: monitor[%d] '%ls' delta=%d newPct=%d", ml->active, mon->name, delta, pct);
    Monitor_SetBrightness(mon, (DWORD)pct);
}

void Monitor_CycleActive(MonitorList *ml, int direction)
{
    if (ml->count <= 1) return;
    Log("CycleActive: %d dir=%d", ml->active, direction);
    ml->active += direction;
    if (ml->active < 0)
        ml->active = ml->count - 1;
    if (ml->active >= ml->count)
        ml->active = 0;
}
