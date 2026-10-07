/* Exercise the real picker and Settings working-copy model. Native window
   destruction, user data and notifications are mocked; no window is created,
   no hardware is linked and no registry or configuration I/O is performed. */
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <dwmapi.h>
#ifndef STRSAFE_NO_DEPRECATE
#define STRSAFE_NO_DEPRECATE
#endif
#include <strsafe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ui.h"
#include "../ui_monitor_selection.h"
#include "../ui_graphics.h"

static int failures, destroys, saveNotifications;
static const HWND testParent = (HWND)(UINT_PTR)1;
static const HWND testPicker = (HWND)(UINT_PTR)2;
static const HWND testOwner = (HWND)(UINT_PTR)3;
static LONG_PTR pickerUserData;
static ULONGLONG sourceTestTick = 20000;
static HWND settingsCapture;
static BYTE *settingsFrame;
static int settingsFrameHeight;
static SHORT WINAPI MockGetKeyState(int vk) { (void)vk; return 0; }
#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("FAIL line %d: %s\n", __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static BOOL WINAPI MockDestroyWindow(HWND);
static BOOL WINAPI MockPostMessageW(HWND, UINT, WPARAM, LPARAM);
static LONG_PTR WINAPI MockGetWindowLongPtrW(HWND, int);
static ULONGLONG WINAPI MockGetTickCount64(void) { return sourceTestTick; }
static HWND WINAPI MockGetCapture(void) { return settingsCapture; }
static HWND WINAPI MockSetCapture(HWND window) { HWND old = settingsCapture; settingsCapture = window; return old; }
static BOOL WINAPI MockReleaseCapture(void) { settingsCapture = NULL; return TRUE; }
static BOOL WINAPI MockScreenToClient(HWND window, LPPOINT point) { (void)window; (void)point; return TRUE; }
static void MockSettingsCommit(HWND window, HDC dc, int width, int height);

#undef GetWindowLongPtrW
#define GetWindowLongPtrW MockGetWindowLongPtrW
#define DestroyWindow MockDestroyWindow
#define PostMessageW MockPostMessageW
#define GetTickCount64 MockGetTickCount64
#define GetCapture MockGetCapture
#define SetCapture MockSetCapture
#define ReleaseCapture MockReleaseCapture
#define ScreenToClient MockScreenToClient
#define GetKeyState MockGetKeyState
#define CommitLayered MockSettingsCommit
#include "../ui_monitor_selection.c"
#include "../ui_settings.c"
#undef GetWindowLongPtrW
#undef DestroyWindow
#undef PostMessageW
#undef GetTickCount64
#undef GetCapture
#undef SetCapture
#undef ReleaseCapture
#undef ScreenToClient
#undef GetKeyState
#undef CommitLayered

HINSTANCE g_uiInst;

/* Accessibility notifications are isolated from the desktop in these tests. */
void A11y_Attach(HWND window, const A11yModel *model) { (void)window; (void)model; }
void A11y_Detach(HWND window) { (void)window; }
BOOL A11y_HandleGetObject(HWND window, WPARAM wp, LPARAM lp, LRESULT *result)
{ (void)window; (void)wp; (void)lp; (void)result; return FALSE; }
void A11y_NotifyFocus(HWND window, int index) { (void)window; (void)index; }
void A11y_NotifyValue(HWND window, int index) { (void)window; (void)index; }
void A11y_NotifyName(HWND window, int index) { (void)window; (void)index; }
void A11y_NotifyState(HWND window, int index) { (void)window; (void)index; }

static void MockSettingsCommit(HWND window, HDC dc, int width, int height)
{
    CHECK(window == testParent && width == SET_WIDTH);
    DIBSECTION dib = {0};
    HBITMAP bitmap = (HBITMAP)GetCurrentObject(dc, OBJ_BITMAP);
    CHECK(GetObjectW(bitmap, sizeof(dib), &dib) == sizeof(dib));
    CHECK(dib.dsBm.bmBits != NULL && dib.dsBm.bmBitsPixel == 32);
    if (!dib.dsBm.bmBits) return;
    GdiFlush();
    BYTE *frame = (BYTE *)realloc(settingsFrame, (size_t)width * height * 4);
    CHECK(frame != NULL);
    if (!frame) return;
    settingsFrame = frame;
    settingsFrameHeight = height;
    memcpy(settingsFrame, dib.dsBm.bmBits, (size_t)width * height * 4);
}

static BOOL WINAPI MockDestroyWindow(HWND window)
{
    CHECK(window == testPicker || window == testParent);
    destroys++;
    return TRUE;
}

static BOOL WINAPI MockPostMessageW(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    CHECK(window == testOwner && message == WM_COMMAND);
    CHECK(wp == IDM_SETTINGS_SAVED && lp == 0);
    saveNotifications++;
    return TRUE;
}

static LONG_PTR WINAPI MockGetWindowLongPtrW(HWND window, int index)
{
    CHECK(window == testPicker && index == GWLP_USERDATA);
    return pickerUserData;
}

/* Required by unused public UI entry points; running them is forbidden here. */
BOOL UI_PopupInit(HINSTANCE instance) { (void)instance; CHECK(FALSE); return FALSE; }
void UI_PopupShutdown(void) { CHECK(FALSE); }
BOOL Settings_GetAutostart(void) { CHECK(FALSE); return FALSE; }

static BrightMonitor MakeMonitor(int identity)
{
    BrightMonitor monitor = { 0 };
    monitor.controllable = TRUE;
    monitor.backend = BACKEND_DDC;
    wsprintfW(monitor.deviceInstance, L"DISPLAY\\DEVICE%d", identity);
    wsprintfW(monitor.name, L"Display %d", identity);
    return monitor;
}

static void SaveIdentity(MonitorSelection *selection, const BrightMonitor *monitor)
{
    CHECK(selection->count < MAX_MONITORS);
    if (selection->count >= MAX_MONITORS) return;
    int index = selection->count++;
    CHECK(Settings_MonitorKey(monitor, selection->keys[index]));
    wcscpy(selection->names[index], monitor->name);
}

static void InitParent(SetEditData *parent, Settings *settings)
{
    memset(parent, 0, sizeof(*parent));
    parent->settings = settings;
    parent->owner = testOwner;
    parent->monitorSelection = settings->monitorSelection;
    parent->step = settings->step;
    parent->idleDimPercent = settings->idleDimPercent;
    parent->idleDimMinutes = settings->idleDimMinutes;
    parent->autostart = settings->autostart;
    parent->idleDimEnabled = settings->idleDimEnabled;
    parent->scheduleEnabled = settings->scheduleEnabled;
    parent->sourcePollSeconds = Settings_ClampSourcePollSeconds(settings->sourcePollSeconds);
    parent->activeSliderRow = -1;
    parent->focusRow = 1;
    parent->captureRow = parent->errorRow = -1;
    BuildSettingsRows(parent);
}

static void TestAllAndCustomWorkingCopies(void)
{
    Settings live = { 0 };
    live.step = 5;
    MonitorList view = { 0 };
    view.count = 2;
    view.monitors[0] = MakeMonitor(1);
    view.monitors[1] = MakeMonitor(2);
    SetEditData parent;
    InitParent(&parent, &live);
    SelectionEdit picker = { 0 };
    picker.destination = &parent.monitorSelection;
    BuildSelectionRows(&picker, &parent.monitorSelection, &view);
    CHECK(!picker.selectedOnly && picker.count == 2 && SelectionCheckedCount(&picker) == 2);
    CHECK(picker.rows[0].canAdd && picker.rows[1].canAdd);

    picker.selectedOnly = TRUE;
    picker.rows[1].checked = FALSE;
    pickerUserData = (LONG_PTR)&picker;
    destroys = saveNotifications = 0;
    SelectionWndProc(testPicker, WM_COMMAND, IDCANCEL, 0);
    CHECK(destroys == 1 && !picker.applied && !parent.monitorSelection.selectedOnly);
    CHECK(parent.monitorSelection.count == 0 && live.monitorSelection.count == 0);

    SelectionWndProc(testPicker, WM_COMMAND, IDOK, 0);
    CHECK(destroys == 2 && picker.applied && parent.monitorSelection.selectedOnly);
    CHECK(parent.monitorSelection.count == 1 && live.monitorSelection.count == 0);
    CHECK(_wcsicmp(parent.monitorSelection.keys[0], L"DDC:DISPLAY\\DEVICE1") == 0);
    CHECK(SetCanSave(&parent));
    SetTrySave(testParent, &parent);
    CHECK(destroys == 3 && saveNotifications == 1);
    CHECK(live.monitorSelection.selectedOnly && live.monitorSelection.count == 1);
    CHECK(memcmp(&live.monitorSelection, &parent.monitorSelection,
                 sizeof(live.monitorSelection)) == 0);

    BuildSelectionRows(&picker, &parent.monitorSelection, &view);
    picker.selectedOnly = FALSE;
    CHECK(CommitMonitorSelection(&picker));
    CHECK(!parent.monitorSelection.selectedOnly && parent.monitorSelection.count == 1);
    CHECK(live.monitorSelection.selectedOnly); /* Parent Cancel would discard this copy. */
    SetCommit(&parent);
    CHECK(!live.monitorSelection.selectedOnly && live.monitorSelection.count == 1);
    /* All retains saved checked keys, so reopening custom does not lose them. */
    BuildSelectionRows(&picker, &live.monitorSelection, &view);
    CHECK(picker.rows[0].checked && !picker.rows[1].checked);
}

static void TestOfflineRemovalAndReordering(void)
{
    MonitorSelection selection = { 0 };
    selection.selectedOnly = TRUE;
    BrightMonitor first = MakeMonitor(1), offline = MakeMonitor(9);
    SaveIdentity(&selection, &first);
    SaveIdentity(&selection, &offline);
    MonitorSelection original = selection;
    MonitorList view = { 0 };
    view.count = 2;
    view.monitors[0] = MakeMonitor(2);
    view.monitors[1] = first;
    wcscpy(view.monitors[1].name, L"Renamed selected display");
    SelectionEdit picker = { 0 };
    picker.destination = &selection;
    BuildSelectionRows(&picker, &selection, &view);
    CHECK(picker.count == 3 && SelectionCheckedCount(&picker) == 2);
    CHECK(!picker.rows[0].checked && picker.rows[1].checked);
    CHECK(picker.rows[2].checked && !picker.rows[2].connected && picker.rows[2].canAdd);
    CHECK(wcscmp(picker.rows[2].name, offline.name) == 0);
    CHECK(wcscmp(picker.rows[1].name, L"Renamed selected display") == 0);
    picker.rows[2].checked = FALSE;
    CHECK(memcmp(&selection, &original, sizeof(selection)) == 0); /* Editing is isolated. */
    CHECK(CommitMonitorSelection(&picker));
    CHECK(selection.count == 1 && _wcsicmp(selection.keys[0], original.keys[0]) == 0);
    CHECK(wcscmp(selection.names[0], L"Renamed selected display") == 0);
}

static LRESULT TryCheckRow(SelectionEdit *picker, int row)
{
    NMLISTVIEW change = { 0 };
    change.hdr.idFrom = PICKER_LIST;
    change.hdr.code = LVN_ITEMCHANGING;
    change.iItem = row;
    change.uChanged = LVIF_STATE;
    change.uOldState = INDEXTOSTATEIMAGEMASK(1);
    change.uNewState = INDEXTOSTATEIMAGEMASK(2);
    pickerUserData = (LONG_PTR)picker;
    return SelectionWndProc(testPicker, WM_NOTIFY, 0, (LPARAM)&change);
}

static void TestConnectedMonitorWithoutBrightness(void)
{
    MonitorSelection selection = {0};
    MonitorList view = {0};
    view.count = 1;
    view.monitors[0] = MakeMonitor(7);
    view.monitors[0].controllable = FALSE;
    view.monitors[0].hasHandle = TRUE;
    view.monitors[0].sourceKnown = TRUE;
    view.monitors[0].currentInput = 0x0F;
    view.monitors[0].sourceCheckedTick = sourceTestTick;
    SaveIdentity(&selection, &view.monitors[0]);
    selection.selectedOnly = TRUE;
    SelectionEdit picker = {0};
    BuildSelectionRows(&picker, &selection, &view);
    CHECK(picker.count == 1 && picker.rows[0].connected && picker.rows[0].checked);
    CHECK(picker.rows[0].canAdd && SourceTelemetryCurrent(&picker.rows[0]));
    CHECK(picker.rows[0].currentInput == 0x0F);
    CHECK(CanEditSource(&picker, &picker.rows[0]) && CanEditIdleBlack(&picker, &picker.rows[0]));
    view.monitors[0].sourceKnown = FALSE;
    MergeSelectionTelemetry(&picker, &view);
    CHECK(picker.rows[0].connected && !SourceTelemetryCurrent(&picker.rows[0]));
}

static void TestLimitsAndUnusableIdentities(void)
{
    MonitorSelection selection = { 0 };
    MonitorList view = { 0 };
    view.count = 4;
    view.monitors[0] = MakeMonitor(1);
    view.monitors[1] = MakeMonitor(1); /* Ambiguous stable identity. */
    view.monitors[2] = MakeMonitor(2);
    view.monitors[2].deviceInstance[0] = L'\0';
    view.monitors[3] = MakeMonitor(3);
    view.monitors[3].controllable = FALSE;
    SelectionEdit picker = { 0 };
    picker.destination = &selection;
    BuildSelectionRows(&picker, &selection, &view);
    CHECK(picker.count == 2 && SelectionCheckedCount(&picker) == 0);
    CHECK(!picker.rows[0].canAdd && !picker.rows[1].canAdd);
    CHECK(TryCheckRow(&picker, 0) == TRUE && TryCheckRow(&picker, 1) == TRUE);
    picker.selectedOnly = TRUE;
    CHECK(!CommitMonitorSelection(&picker) && selection.count == 0);

    memset(&selection, 0, sizeof(selection));
    selection.selectedOnly = TRUE;
    view.count = MAX_MONITORS;
    for (int i = 0; i < MAX_MONITORS; i++) {
        view.monitors[i] = MakeMonitor(i);
        BrightMonitor offline = MakeMonitor(i + MAX_MONITORS);
        SaveIdentity(&selection, &offline);
    }
    MonitorSelection original = selection;
    BuildSelectionRows(&picker, &selection, &view);
    CHECK(picker.count == MAX_MONITORS * 2 && SelectionCheckedCount(&picker) == MAX_MONITORS);
    CHECK(TryCheckRow(&picker, 0) == TRUE); /* UI rejects a seventeenth checked key. */
    picker.rows[0].checked = TRUE;
    CHECK(!CommitMonitorSelection(&picker));
    CHECK(memcmp(&selection, &original, sizeof(selection)) == 0);
    picker.rows[0].checked = FALSE;
    CHECK(CommitMonitorSelection(&picker) && selection.count == MAX_MONITORS);
}

static void TestParentRowsAndSaveValidation(void)
{
    Settings live = { 0 };
    SetEditData parent;
    InitParent(&parent, &live);
    CHECK(!parent.idleDimEnabled);
    int chooser = -1, top = SET_HEADER_H;
    for (int i = 0; i < parent.rowCount; i++) {
        if (parent.rows[i].kind == SET_MONITORS) {
            int hit;
            chooser = i;
            CHECK(SetHitTest(&parent, SET_WIDTH / 2, top + 5, &hit) == i);
            CHECK(hit == SETHIT_MONITORS);
        }
        top += SetRowHeight(&parent.rows[i]);
    }
    CHECK(chooser >= 0); /* Global scope remains available while idle dim is disabled. */
    BOOL visitedChooser = FALSE, visitedSave = FALSE;
    for (int i = 0; i <= parent.rowCount; i++) {
        parent.focusRow = SetStepFocus(&parent, parent.focusRow, 1);
        if (parent.focusRow == chooser) visitedChooser = TRUE;
        if (parent.focusRow == SET_SAVE(&parent)) visitedSave = TRUE;
        else if (parent.focusRow < parent.rowCount) CHECK(parent.rows[parent.focusRow].kind != SET_SECTION);
    }
    CHECK(visitedChooser && visitedSave);
    parent.focusRow = 1;
    parent.focusRow = SetStepFocus(&parent, parent.focusRow, -1);
    CHECK(parent.focusRow == SET_SAVE(&parent)); /* Shift-Tab wraps to Save. */

    parent.monitorSelection.selectedOnly = TRUE;
    parent.monitorSelection.count = 0;
    int oldDestroys = destroys, oldNotifications = saveNotifications;
    CHECK(!SetCanSave(&parent));
    SetTrySave(testParent, &parent);
    CHECK(destroys == oldDestroys && saveNotifications == oldNotifications);
    CHECK(!live.monitorSelection.selectedOnly);
    parent.monitorSelection.count = MAX_MONITORS + 1;
    CHECK(!SetCanSave(&parent));
    parent.monitorSelection.selectedOnly = FALSE;
    CHECK(SetCanSave(&parent));
}

static void TestSourceRulesAndLiveTelemetry(void)
{
    MonitorSelection selection = { 0 };
    MonitorList view = { 0 };
    view.count = 2;
    view.monitors[0] = MakeMonitor(1);
    view.monitors[0].sourceKnown = TRUE;
    view.monitors[0].currentInput = 0x0F;
    view.monitors[0].sourceCheckedTick = sourceTestTick;
    view.monitors[1] = MakeMonitor(2);
    SelectionEdit picker = { 0 };
    picker.destination = &selection;
    BuildSelectionRows(&picker, &selection, &view);
    CHECK(!picker.selectedOnly);
    CHECK(TryCheckRow(&picker, 1)); /* All mode keeps the list usable but locks scope checkboxes. */
    CHECK(CanEditSource(&picker, &picker.rows[0]));
    CHECK(!picker.rows[0].expectedInput && !picker.rows[0].sourceFilter); /* No auto-association. */
    CHECK(AssociateCurrentInput(&picker, &picker.rows[0]));
    CHECK(picker.rows[0].expectedInput == 0x0F);
    CHECK(!AssociateCurrentInput(&picker, &picker.rows[1]));
    CHECK(!picker.rows[1].expectedInput && !picker.rows[1].rulePresent);
    picker.rows[0].sourceFilter = TRUE;
    CHECK(wcscmp(SelectionRowStatus(&picker, &picker.rows[0]), L"In control") == 0);

    /* Hardware results can arrive while editing; expected input and selection
       must remain local even when live settings disagree or monitor order changes. */
    picker.selectedOnly = TRUE;
    picker.rows[0].checked = FALSE;
    BrightMonitor swapped = view.monitors[0];
    view.monitors[0] = view.monitors[1];
    view.monitors[1] = swapped;
    view.monitors[1].currentInput = 0x12;
    view.monitors[1].sourceFilter = FALSE;
    view.monitors[1].expectedInput = 0x11;
    wcscpy(view.monitors[1].name, L"Renamed display");
    g_pickerWindow = testPicker;
    pickerUserData = (LONG_PTR)&picker;
    CHECK(UI_MonitorSelectionIsOpen());
    UI_MonitorSelectionRefresh(&view);
    g_pickerWindow = NULL;
    CHECK(!UI_MonitorSelectionIsOpen());
    CHECK(!picker.rows[0].checked && picker.rows[1].checked);
    CHECK(picker.rows[0].sourceFilter && picker.rows[0].expectedInput == 0x0F);
    CHECK(picker.rows[0].currentInput == 0x12 && picker.rows[0].sourceKnown);
    CHECK(wcscmp(picker.rows[0].name, L"Renamed display") == 0);
    CHECK(wcscmp(SelectionRowStatus(&picker, &picker.rows[0]), L"Not selected") == 0);
    CHECK(selection.count == 0 && selection.inputRuleCount == 0); /* Not committed yet. */
    CHECK(CommitMonitorSelection(&picker));
    CHECK(selection.count == 1 && selection.inputRuleCount == 1);
    CHECK(_wcsicmp(selection.inputRules[0].key, L"DDC:DISPLAY\\DEVICE1") == 0);
    CHECK(selection.inputRules[0].input == 0x0F && selection.inputRules[0].enabled);

    /* A rule survives an unchecked monitor going offline, then a disabled filter
       retains its associated port for the next time the user enables it. */
    view.count = 1;
    BuildSelectionRows(&picker, &selection, &view);
    CHECK(picker.count == 2 && picker.rows[0].checked && !picker.rows[1].checked);
    CHECK(!picker.rows[1].connected && picker.rows[1].sourceFilter);
    CHECK(picker.rows[1].expectedInput == 0x0F);
    CHECK(!AssociateCurrentInput(&picker, &picker.rows[1]));
    picker.rows[1].sourceFilter = FALSE;
    CHECK(CommitMonitorSelection(&picker));
    CHECK(selection.inputRuleCount == 1 && !selection.inputRules[0].enabled);
    CHECK(selection.inputRules[0].input == 0x0F);
    picker.rows[1].expectedInput = 0;
    CHECK(CommitMonitorSelection(&picker));
    CHECK(selection.inputRuleCount == 0); /* Clear the port and disable to free a saved rule slot. */
}

static void TestSourceStatusAndBuiltIn(void)
{
    SelectionEdit picker = { 0 };
    SelectionRow row = { 0 };
    row.connected = row.canAdd = row.sourceFilter = TRUE;
    wcscpy(row.key, L"DDC:DISPLAY\\DEVICE1");
    CHECK(wcscmp(SelectionRowStatus(&picker, &row), L"Paused: input not set") == 0);
    row.expectedInput = 0x11;
    CHECK(wcscmp(SelectionRowStatus(&picker, &row), L"Paused: input unavailable") == 0);
    row.sourceKnown = TRUE;
    row.currentInput = 0x12;
    row.sourceCheckedTick = sourceTestTick;
    CHECK(wcscmp(SelectionRowStatus(&picker, &row), L"Paused: other input") == 0);
    row.currentInput = row.expectedInput;
    CHECK(wcscmp(SelectionRowStatus(&picker, &row), L"In control") == 0);
    sourceTestTick += 10001;
    CHECK(wcscmp(SelectionRowStatus(&picker, &row), L"Paused: input unavailable") == 0);
    CHECK(!AssociateCurrentInput(&picker, &row));
    row.sourceCheckedTick = sourceTestTick;
    row.currentInput = 0x100;
    CHECK(!AssociateCurrentInput(&picker, &row));
    row.currentInput = 0x12;
    row.builtIn = TRUE;
    CHECK(wcscmp(SelectionRowStatus(&picker, &row), L"Built-in") == 0);
    CHECK(!CanEditSource(&picker, &row) && !AssociateCurrentInput(&picker, &row));
    WCHAR text[40];
    FormatInput(0x99, text, ARRAYSIZE(text));
    CHECK(wcscmp(text, L"Input 0x99") == 0);
}

static void TestBlackIdleOfflineAndBuiltInWorkingCopy(void)
{
    MonitorSelection live = {0};
    live.idleBlackCount = 1;
    wcscpy(live.idleBlackKeys[0], L"DDC:OFFLINE-OLED");
    wcscpy(live.idleBlackNames[0], L"Disconnected OLED");
    MonitorList view = {0};
    view.count = 1;
    view.monitors[0] = MakeMonitor(1);
    view.monitors[0].backend = BACKEND_WMI;
    wcscpy(view.monitors[0].wmiInstance, L"BUILT-IN-OLED");
    SelectionEdit picker = {0};
    MonitorSelection working = live;
    picker.destination = &working;
    BuildSelectionRows(&picker, &working, &view);
    CHECK(picker.count == 2 && picker.rows[0].builtIn);
    CHECK(CanEditIdleBlack(&picker, &picker.rows[0]));
    CHECK(!CanEditSource(&picker, &picker.rows[0]));
    picker.rows[0].idleBlack = TRUE;
    MergeSelectionTelemetry(&picker, &view);
    CHECK(picker.rows[0].idleBlack && picker.rows[1].idleBlack);
    CHECK(live.idleBlackCount == 1);
    CHECK(CommitMonitorSelection(&picker));
    CHECK(working.idleBlackCount == 2 && !working.selectedOnly);
    CHECK(working.inputRuleCount == 0);
    picker.rows[0].idleBlack = FALSE;
    CHECK(CommitMonitorSelection(&picker));
    CHECK(working.idleBlackCount == 1 && !wcscmp(working.idleBlackKeys[0], L"DDC:OFFLINE-OLED"));
}

static void TestSourceIntervalSlider(void)
{
    Settings live = {0};
    live.sourcePollSeconds = 3;
    InitParent(&g_set, &live);
    int row = -1;
    for (int i = 0; i < g_set.rowCount; i++)
        if (g_set.rows[i].kind == SET_SLIDER) row = i;
    CHECK(row >= 0);
    if (row < 0) return;
    SetRow *slider = &g_set.rows[row];
    CHECK(slider->lo == 1 && slider->hi == 60 && slider->step == 1);
    CHECK(slider->ival == &g_set.sourcePollSeconds);
    RECT track;
    SetSliderRect(SetRowTop(&g_set, row), &track);
    int cy = (track.top + track.bottom) / 2;
    int hit;
    CHECK(SetHitTest(&g_set, track.left, cy, &hit) == row && hit == SETHIT_SLIDER);
    CHECK(SetHitTest(&g_set, track.left, SetRowTop(&g_set, row) + 10, &hit) == row && hit == SETHIT_ROW);
    for (int seconds = 1; seconds <= 60; seconds++) {
        int x = track.left + (seconds - 1) * (track.right - track.left) / 59;
        SetSliderValue(&g_set, row, x);
        CHECK(g_set.sourcePollSeconds == seconds);
    }
    SetSliderValue(&g_set, row, -100);
    CHECK(g_set.sourcePollSeconds == 1);
    SetAdjust(slider, -1);
    CHECK(g_set.sourcePollSeconds == 1);
    SetAdjust(slider, 1);
    CHECK(g_set.sourcePollSeconds == 2);
    SetSliderValue(&g_set, row, SET_WIDTH + 100);
    SetAdjust(slider, 1);
    CHECK(g_set.sourcePollSeconds == 60 && live.sourcePollSeconds == 3);

    SetWndProc(testParent, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(track.left, cy));
    CHECK(g_set.sourcePollSeconds == 1 && g_set.activeSliderRow == row && settingsCapture == testParent);
    SetWndProc(testParent, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(track.right, cy));
    CHECK(g_set.sourcePollSeconds == 60);
    SetWndProc(testParent, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(-50, cy));
    CHECK(g_set.sourcePollSeconds == 1); /* Signed coordinates while dragging outside. */
    SetWndProc(testParent, WM_LBUTTONUP, 0, MAKELPARAM(track.right, cy));
    CHECK(g_set.sourcePollSeconds == 60 && g_set.activeSliderRow == -1 && !settingsCapture);
    g_set.focusRow = row;
    SetWndProc(testParent, WM_KEYDOWN, VK_LEFT, 0);
    CHECK(g_set.sourcePollSeconds == 59);
    SetWndProc(testParent, WM_KEYDOWN, VK_HOME, 0);
    CHECK(g_set.sourcePollSeconds == 1);
    SetWndProc(testParent, WM_KEYDOWN, VK_RIGHT, 0);
    CHECK(g_set.sourcePollSeconds == 2);
    SetWndProc(testParent, WM_MOUSEWHEEL, MAKEWPARAM(0, 120), MAKELPARAM(track.left, cy));
    CHECK(g_set.sourcePollSeconds == 3);
    SetWndProc(testParent, WM_KEYDOWN, VK_END, 0);
    CHECK(g_set.sourcePollSeconds == 60 && live.sourcePollSeconds == 3);
    SetWndProc(testParent, WM_KEYDOWN, VK_ESCAPE, 0);
    CHECK(live.sourcePollSeconds == 3); /* Closing without Save discards the edit. */

    SetWndProc(testParent, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(track.right, cy));
    SetWndProc(testParent, WM_CANCELMODE, 0, 0);
    CHECK(g_set.activeSliderRow == -1 && !settingsCapture);
    SetWndProc(testParent, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(track.left, cy));
    SetWndProc(testParent, WM_CAPTURECHANGED, 0, (LPARAM)testOwner);
    settingsCapture = NULL;
    SetWndProc(testParent, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(track.right, cy));
    CHECK(g_set.activeSliderRow == -1 && g_set.sourcePollSeconds == 1);
    g_set.sourcePollSeconds = 37;
    SetTrySave(testParent, &g_set);
    CHECK(live.sourcePollSeconds == 37);
    g_set.presetCount = MAX_PRESETS;
    BuildSettingsRows(&g_set);
    CHECK(g_set.rowCount <= MAX_SET_ROWS && g_set.rows[g_set.rowCount - 1].kind == SET_NUMBER);
    CHECK(g_set.rows[g_set.rowCount - 1].ival == &g_set.presetValues[MAX_PRESETS - 1]);
}

static void TestSlowPollingTelemetryFreshness(void)
{
    SelectionRow row = {0};
    row.connected = row.sourceKnown = TRUE;
    row.currentInput = 0x0F;
    row.sourceCheckedTick = sourceTestTick;
    UI_MonitorSelectionSetSourcePollInterval(60000);
    sourceTestTick += 60000;
    CHECK(SourceTelemetryCurrent(&row));
    row.sourceKnown = FALSE;
    CHECK(!SourceTelemetryCurrent(&row)); /* An actual read failure still clears telemetry immediately. */
    row.sourceKnown = TRUE;
    sourceTestTick += 5001;
    CHECK(!SourceTelemetryCurrent(&row));
    UI_MonitorSelectionSetSourcePollInterval(3000);
    row.sourceCheckedTick = sourceTestTick;
    sourceTestTick += 10001;
    CHECK(!SourceTelemetryCurrent(&row));
}

static int hotkeyApplyCalls, hotkeyApplyFailure;
static BOOL hotkeysSuspended;
static int ApplyTestHotkeys(const Hotkey *hotkeys)
{ (void)hotkeys; hotkeyApplyCalls++; return hotkeyApplyFailure; }
static void SuspendTestHotkeys(BOOL suspended) { hotkeysSuspended = suspended; }
static int TestFirstFailedHotkey(void) { return -1; }

static void TestSettingsAccessibilityAndHotkeyValidation(void)
{
    Settings live = {0};
    InitParent(&g_set, &live);
    int chooser = -1, slider = -1;
    for (int i = 0; i < g_set.rowCount; i++) {
        if (g_set.rows[i].kind == SET_MONITORS) chooser = i;
        if (g_set.rows[i].kind == SET_SLIDER) slider = i;
    }
    A11yItem item = {0};
    CHECK(chooser >= 0 && slider >= 0);
    SetA11yDescribe(&g_set, SetModelFromRow(&g_set, chooser), &item);
    CHECK(item.role == ROLE_SYSTEM_PUSHBUTTON && wcsstr(item.name, L"all monitors"));
    ZeroMemory(&item, sizeof(item));
    SetA11yDescribe(&g_set, SetModelFromRow(&g_set, slider), &item);
    CHECK(item.role == ROLE_SYSTEM_SLIDER && wcsstr(item.value, L"second"));
    CHECK(item.rect.bottom - item.rect.top == SET_SLIDER_H);
    g_set.monitorSelection.selectedOnly = TRUE;
    ZeroMemory(&item, sizeof(item));
    SetA11yDescribe(&g_set, SetModelFromRow(&g_set, SET_SAVE(&g_set)), &item);
    CHECK(item.state & STATE_SYSTEM_UNAVAILABLE);

    static const HotkeyHost host = {ApplyTestHotkeys, SuspendTestHotkeys, TestFirstFailedHotkey};
    UI_SetHotkeyHost(&host);
    g_set.monitorSelection.selectedOnly = FALSE;
    g_set.hotkeys[HOTKEY_BRIGHTEN] = (Hotkey){HK_MOD_CONTROL | HK_MOD_ALT, VK_UP};
    g_set.hotkeys[HOTKEY_DIM] = g_set.hotkeys[HOTKEY_BRIGHTEN];
    int oldDestroys = destroys, oldNotifications = saveNotifications;
    hotkeyApplyCalls = 0;
    SetTrySave(testParent, &g_set);
    CHECK(destroys == oldDestroys && saveNotifications == oldNotifications);
    CHECK(hotkeyApplyCalls == 0 && g_set.errorRow == SetRowOfHotkey(&g_set, HOTKEY_DIM));
    CHECK(!live.hotkeys[HOTKEY_BRIGHTEN].vk);
    g_set.hotkeys[HOTKEY_DIM].vk = VK_DOWN;
    hotkeyApplyFailure = HOTKEY_DIM;
    SetTrySave(testParent, &g_set);
    CHECK(hotkeyApplyCalls == 1 && destroys == oldDestroys && saveNotifications == oldNotifications);
    CHECK(wcsstr(g_set.errorText, L"another app"));
    int capture = SetRowOfHotkey(&g_set, HOTKEY_BRIGHTEN);
    SetBeginCapture(testParent, &g_set, capture);
    CHECK(hotkeysSuspended && g_set.captureRow == capture);
    SetWndProc(testParent, WM_KEYDOWN, VK_BACK, 0);
    CHECK(!hotkeysSuspended && g_set.captureRow == -1 && !g_set.hotkeys[HOTKEY_BRIGHTEN].vk);
    hotkeyApplyFailure = -1;
    g_set.sourcePollSeconds = 23;
    SetTrySave(testParent, &g_set);
    CHECK(hotkeyApplyCalls == 2 && destroys == oldDestroys + 1 && saveNotifications == oldNotifications + 1);
    CHECK(live.hotkeys[HOTKEY_DIM].vk == VK_DOWN && live.sourcePollSeconds == 23);
    UI_SetHotkeyHost(NULL);
}

static void SaveSettingsPreview(const char *path, int seconds)
{
    Settings live = {0};
    live.sourcePollSeconds = seconds;
    live.step = live.idleDimMinutes = 5;
    live.idleDimPercent = 1;
    live.idleDimEnabled = TRUE;
    live.presetCount = 3;
    wcscpy(live.presets[0].name, L"Night"); live.presets[0].brightness = 30;
    wcscpy(live.presets[1].name, L"Day"); live.presets[1].brightness = 60;
    wcscpy(live.presets[2].name, L"Presentation"); live.presets[2].brightness = 100;
    InitParent(&g_set, &live);
    g_set.presetCount = live.presetCount;
    for (int i = 0; i < live.presetCount; i++) g_set.presetValues[i] = (int)live.presets[i].brightness;
    BuildSettingsRows(&g_set);
    RenderSettings(testParent, &g_set);
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    if (!file) return;
    BITMAPFILEHEADER header = {0};
    BITMAPINFOHEADER info = {0};
    header.bfType = 0x4D42;
    header.bfOffBits = sizeof(header) + sizeof(info);
    header.bfSize = header.bfOffBits + (DWORD)SET_WIDTH * settingsFrameHeight * 4;
    info.biSize = sizeof(info);
    info.biWidth = SET_WIDTH;
    info.biHeight = -settingsFrameHeight;
    info.biPlanes = 1;
    info.biBitCount = 32;
    CHECK(fwrite(&header, sizeof(header), 1, file) == 1);
    CHECK(fwrite(&info, sizeof(info), 1, file) == 1);
    CHECK(fwrite(settingsFrame, (size_t)SET_WIDTH * settingsFrameHeight * 4, 1, file) == 1);
    fclose(file);
}

int main(int argc, char **argv)
{
    TestBlackIdleOfflineAndBuiltInWorkingCopy();
    TestAllAndCustomWorkingCopies();
    TestOfflineRemovalAndReordering();
    TestLimitsAndUnusableIdentities();
    TestConnectedMonitorWithoutBrightness();
    TestParentRowsAndSaveValidation();
    TestSourceRulesAndLiveTelemetry();
    TestSourceStatusAndBuiltIn();
    TestSourceIntervalSlider();
    TestSlowPollingTelemetryFreshness();
    TestSettingsAccessibilityAndHotkeyValidation();
    if (argc > 1 && strcmp(argv[1], "--preview") == 0) {
        SaveSettingsPreview("build/settings-source-interval-3.bmp", 3);
        SaveSettingsPreview("build/settings-source-interval-60.bmp", 60);
    }
    free(settingsFrame);
    if (failures) {
        printf("monitor selection UI: %d failure(s)\n", failures);
        return 1;
    }
    puts("ALL PASS: monitor picker working copies, source rules, live telemetry, Apply/Cancel/Save and identities (mocked UI)");
    return 0;
}
