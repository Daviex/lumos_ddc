#include "presets.h"
#include <strsafe.h>

/* These helpers only inspect snapshots and saved selection data. They never
   access the INI, registry, monitor hardware or hardware worker. */
BOOL Settings_MonitorKeyValid(const WCHAR *key)
{
    size_t length;
    if (!key || FAILED(StringCchLengthW(key, MONITOR_SELECTION_KEY_LEN, &length)) || length <= 4)
        return FALSE;
    if (_wcsnicmp(key, L"DDC:", 4) != 0 && _wcsnicmp(key, L"WMI:", 4) != 0)
        return FALSE;
    for (size_t i = 4; i < length; i++)
        if (key[i] == L'\r' || key[i] == L'\n') return FALSE;
    return TRUE;
}

BOOL Settings_MonitorKey(const BrightMonitor *monitor, WCHAR key[MONITOR_SELECTION_KEY_LEN])
{
    const WCHAR *identity;
    const WCHAR *prefix;
    size_t length;
    if (!key) return FALSE;
    key[0] = L'\0';
    if (!monitor) return FALSE;
    if (monitor->backend == BACKEND_DDC) {
        identity = monitor->deviceInstance;
        prefix = L"DDC:";
    } else if (monitor->backend == BACKEND_WMI) {
        identity = monitor->wmiInstance;
        prefix = L"WMI:";
    } else {
        return FALSE;
    }
    if (FAILED(StringCchLengthW(identity, ARRAYSIZE(monitor->deviceInstance), &length)) ||
        length == 0 || FAILED(StringCchPrintfW(key, MONITOR_SELECTION_KEY_LEN,
                                              L"%s%s", prefix, identity)) ||
        !Settings_MonitorKeyValid(key)) {
        key[0] = L'\0';
        return FALSE;
    }
    return TRUE;
}

BOOL Settings_MonitorSelected(const MonitorSelection *selection, const BrightMonitor *monitor)
{
    WCHAR key[MONITOR_SELECTION_KEY_LEN];
    if (!selection || !selection->selectedOnly) return TRUE;
    if (!Settings_MonitorKey(monitor, key)) return FALSE;
    for (int i = 0; i < selection->count && i < MAX_MONITORS; i++)
        if (Settings_MonitorKeyValid(selection->keys[i]) &&
            _wcsicmp(selection->keys[i], key) == 0) return TRUE;
    return FALSE;
}

void Settings_ApplyMonitorSelection(const Settings *s, MonitorList *view)
{
    const MonitorSelection *selection = &s->monitorSelection;
    view->selectedOnly = selection->selectedOnly;
    for (int i = 0; i < view->count; i++) {
        BrightMonitor *monitor = &view->monitors[i];
        monitor->excludedFromControl = FALSE;
        if (!monitor->controllable || !selection->selectedOnly) continue;
        monitor->excludedFromControl = !Settings_MonitorSelected(selection, monitor);
        if (!monitor->excludedFromControl) {
            WCHAR key[MONITOR_SELECTION_KEY_LEN];
            int matches = 0;
            Settings_MonitorKey(monitor, key);
            for (int j = 0; j < view->count; j++) {
                WCHAR candidateKey[MONITOR_SELECTION_KEY_LEN];
                if (view->monitors[j].controllable &&
                    Settings_MonitorKey(&view->monitors[j], candidateKey) &&
                    _wcsicmp(key, candidateKey) == 0) matches++;
            }
            monitor->excludedFromControl = matches != 1;
        }
    }
}
