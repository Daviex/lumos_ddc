#ifndef PRESETS_H
#define PRESETS_H

#include <windows.h>
#include "monitor.h"
#include "schedule.h"
#include "hotkey.h"

#define MAX_PRESETS 10
#define MAX_PRESET_NAME 64

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
    /* Per-monitor ranges by monitor name, from [Ranges]. rangeConnected marks
       the entries whose monitor is in the current list (Settings_ApplyRanges). */
    int    rangeCount;
    WCHAR  rangeNames[MAX_MONITORS][128];
    int    rangeLo[MAX_MONITORS];
    int    rangeHi[MAX_MONITORS];
    BOOL   rangeConnected[MAX_MONITORS];
    SchedulePoint schedule[MAX_SCHEDULE];
    int           scheduleCount;
    BOOL          scheduleEnabled;
    BOOL          idleDimEnabled;
    int           idleDimPercent;   /* level held while the session is idle (0-100) */
    int           idleDimMinutes;   /* idle time before dimming */
    Hotkey        hotkeys[HOTKEY_COUNT];   /* indexed by HOTKEY_BRIGHTEN etc. */
} Settings;

/* Initialize settings path and load from INI */
void Settings_Init(Settings *s);

/* Load presets and settings from INI file */
void Settings_Load(Settings *s);

/* Save current settings to INI file */
void Settings_Save(Settings *s);

/* Create default INI if it doesn't exist */
void Settings_CreateDefaults(Settings *s);

/* Toggle autostart registry entry */
void Settings_SetAutostart(BOOL enable);
BOOL Settings_GetAutostart(void);

/* Give every monitor its saved range (matched by name). A monitor without one
   gets the full range and an entry, so the Settings window can list it. */
void Settings_ApplyRanges(Settings *s, MonitorList *ml);

/* Copy the monitors' ranges into the settings for saving. Entries of monitors
   that are not connected are kept. */
void Settings_StoreRanges(Settings *s, const MonitorList *ml);

#endif /* PRESETS_H */
