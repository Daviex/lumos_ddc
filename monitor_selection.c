#include "presets.h"
#include <strsafe.h>

/* These helpers only inspect snapshots and saved selection data. They never
   access the INI, registry, monitor hardware or hardware worker. */
int Settings_ClampSourcePollSeconds(int seconds)
{
    if (seconds < MIN_SOURCE_POLL_SECONDS) return MIN_SOURCE_POLL_SECONDS;
    if (seconds > MAX_SOURCE_POLL_SECONDS) return MAX_SOURCE_POLL_SECONDS;
    return seconds;
}

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

const MonitorInputRule *Settings_MonitorInputRule(const MonitorSelection *selection,
                                                const BrightMonitor *monitor)
{
    WCHAR key[MONITOR_SELECTION_KEY_LEN];
    if (!selection || !monitor || monitor->backend != BACKEND_DDC ||
        !Settings_MonitorKey(monitor, key)) return NULL;
    for (int i = 0; i < selection->inputRuleCount && i < MAX_MONITORS; i++)
        if (Settings_MonitorKeyValid(selection->inputRules[i].key) &&
            _wcsicmp(selection->inputRules[i].key, key) == 0)
            return &selection->inputRules[i];
    return NULL;
}

void Settings_ApplyMonitorSelection(const Settings *s, MonitorList *view)
{
    const MonitorSelection *selection = &s->monitorSelection;
    view->selectedOnly = selection->selectedOnly;
    for (int i = 0; i < view->count; i++) {
        BrightMonitor *monitor = &view->monitors[i];
        monitor->excludedFromControl = FALSE;
        monitor->idleBlack = FALSE;
        monitor->sourceFilter = FALSE;
        monitor->expectedInput = 0;
        WCHAR key[MONITOR_SELECTION_KEY_LEN];
        BOOL hasKey = Settings_MonitorKey(monitor, key);
        if (hasKey) {
            for (int j = 0; j < selection->idleBlackCount && j < MAX_MONITORS; j++)
                if (Settings_MonitorKeyValid(selection->idleBlackKeys[j]) &&
                    _wcsicmp(selection->idleBlackKeys[j], key) == 0)
                    monitor->idleBlack = TRUE;
        }
        int ruleMatches = 0;
        if (hasKey && monitor->backend == BACKEND_DDC) {
            for (int j = 0; j < selection->inputRuleCount && j < MAX_MONITORS; j++) {
                const MonitorInputRule *rule = &selection->inputRules[j];
                if (!Settings_MonitorKeyValid(rule->key) || _wcsicmp(rule->key, key) != 0)
                    continue;
                ruleMatches++;
                if (rule->enabled) monitor->sourceFilter = TRUE;
                monitor->expectedInput = rule->input <= 255 ? rule->input : 0;
            }
            /* Duplicate records can never turn an enabled filter into permission. */
            if (ruleMatches > 1) monitor->expectedInput = 0;
        }
        if (selection->selectedOnly)
            monitor->excludedFromControl = !Settings_MonitorSelected(selection, monitor);
        if (hasKey && (monitor->sourceFilter || monitor->idleBlack ||
                       (selection->selectedOnly && !monitor->excludedFromControl))) {
            int matches = 0;
            for (int j = 0; j < view->count; j++) {
                WCHAR candidateKey[MONITOR_SELECTION_KEY_LEN];
                if (Settings_MonitorKey(&view->monitors[j], candidateKey) &&
                    _wcsicmp(key, candidateKey) == 0) matches++;
            }
            if (matches != 1) {
                if (selection->selectedOnly) monitor->excludedFromControl = TRUE;
                if (monitor->sourceFilter) monitor->expectedInput = 0;
                monitor->idleBlack = FALSE;
            }
        }
    }
}
