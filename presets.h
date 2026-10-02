#ifndef PRESETS_H
#define PRESETS_H

#include <windows.h>
#include "monitor.h"
#include "schedule.h"

#define MAX_PRESETS 10
#define MAX_PRESET_NAME 64
#define DEFAULT_DAY_BRIGHTNESS 80
#define LUMOS_STARTUP_ARGUMENT L"--startup"
#define MONITOR_SELECTION_KEY_LEN 260 /* backend prefix + 255-character identity + NUL */

typedef struct {
    WCHAR key[MONITOR_SELECTION_KEY_LEN];
    WCHAR name[128];
    BOOL enabled;
    DWORD input; /* DDC VCP 0x60 value, 1..255; 0 means not configured. */
} MonitorInputRule;

typedef struct {
    BOOL selectedOnly; /* FALSE: all monitors; TRUE: only the saved stable keys */
    int count;
    WCHAR keys[MAX_MONITORS][MONITOR_SELECTION_KEY_LEN];
    WCHAR names[MAX_MONITORS][128]; /* retained even while a selected monitor is offline */
    int inputRuleCount;
    MonitorInputRule inputRules[MAX_MONITORS]; /* independent of selection/offline state */
} MonitorSelection;

typedef struct {
    WCHAR name[MAX_PRESET_NAME];
    DWORD brightness; /* 0-100 */
} Preset;

typedef struct {
    WCHAR iniPath[MAX_PATH];
    Preset presets[MAX_PRESETS];
    int    presetCount;
    int    step;       /* brightness step for hotkeys and mouse wheel (default 5) */
    BOOL   autostart;
    int    deltaCount;
    WCHAR  deltaNames[MAX_MONITORS][128];
    int    deltaValues[MAX_MONITORS];
    SchedulePoint schedule[MAX_SCHEDULE];
    int           scheduleCount;
    BOOL          scheduleEnabled;
    BOOL          idleDimEnabled;
    int           idleDimPercent;   /* level held while the session is idle (0-100) */
    int           idleDimMinutes;   /* idle time before dimming */
    MonitorSelection monitorSelection;
} Settings;

/* Initialize settings path and load from INI */
void Settings_Init(Settings *s);

/* Load presets and settings from INI file */
void Settings_Load(Settings *s);

/* Save current settings to INI file */
void Settings_Save(Settings *s);

/* Create default INI if it doesn't exist */
void Settings_CreateDefaults(Settings *s);

/* Resolve the configured daytime preset without I/O. Names are case-insensitive;
   Giorno takes precedence over Day. The first match for each name wins, with
   DEFAULT_DAY_BRIGHTNESS used when neither name is present. */
int Settings_DayBrightness(const Settings *s);

/* Stable backend-prefixed keys, never a display name, index or physical handle.
   Missing identity cannot match a custom selection. */
BOOL Settings_MonitorKey(const BrightMonitor *monitor, WCHAR key[MONITOR_SELECTION_KEY_LEN]);
BOOL Settings_MonitorKeyValid(const WCHAR *key);
BOOL Settings_MonitorSelected(const MonitorSelection *selection, const BrightMonitor *monitor);
/* Find a source rule using the DDC stable identity. Names never participate. */
const MonitorInputRule *Settings_MonitorInputRule(const MonitorSelection *selection,
                                                const BrightMonitor *monitor);
/* Apply global scope without discarding saved offline entries. All matching
   monitors are excluded when a selected identity is ambiguous in this list. */
void Settings_ApplyMonitorSelection(const Settings *s, MonitorList *view);

/* Toggle autostart registry entry; returns whether the change succeeded. */
BOOL Settings_SetAutostart(BOOL enable);
BOOL Settings_GetAutostart(void);

/* Upgrade only an existing legacy command for this executable to include the
   startup flag. Absent entries and other commands are left unchanged. */
BOOL Settings_UpgradeAutostart(void);

/* Load delta values from settings into monitors (match by name) */
void Settings_LoadDeltas(Settings *s, MonitorList *ml);

/* Copy current monitor deltas into settings for saving */
void Settings_SaveDeltas(Settings *s, MonitorList *ml);

#endif /* PRESETS_H */
