/* Exercise the real picker and Settings working-copy model. Native window
   destruction, user data and notifications are mocked; no window is created,
   no hardware is linked and no registry or configuration I/O is performed. */
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <strsafe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ui.h"
#include "../ui_monitor_selection.h"

static int failures, destroys, saveNotifications;
static const HWND testParent = (HWND)(UINT_PTR)1;
static const HWND testPicker = (HWND)(UINT_PTR)2;
static const HWND testOwner = (HWND)(UINT_PTR)3;
static LONG_PTR pickerUserData;
static ULONGLONG sourceTestTick = 20000;
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

#undef GetWindowLongPtrW
#define GetWindowLongPtrW MockGetWindowLongPtrW
#define DestroyWindow MockDestroyWindow
#define PostMessageW MockPostMessageW
#define GetTickCount64 MockGetTickCount64
#include "../ui_monitor_selection.c"
#include "../ui.c"
#undef GetWindowLongPtrW
#undef DestroyWindow
#undef PostMessageW
#undef GetTickCount64

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
    parent->keyboardRow = -1;
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
    SetSave(testParent, &parent);
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
        SetMoveKeyboard(&parent, 1);
        if (parent.keyboardRow == chooser) visitedChooser = TRUE;
        if (parent.keyboardRow == parent.rowCount) visitedSave = TRUE;
        else CHECK(parent.rows[parent.keyboardRow].kind != SET_SECTION);
    }
    CHECK(visitedChooser && visitedSave);
    parent.keyboardRow = 1;
    SetMoveKeyboard(&parent, -1);
    CHECK(parent.keyboardRow == parent.rowCount); /* Shift-Tab wraps to Save. */

    parent.monitorSelection.selectedOnly = TRUE;
    parent.monitorSelection.count = 0;
    int oldDestroys = destroys, oldNotifications = saveNotifications;
    CHECK(!SetCanSave(&parent));
    SetSave(testParent, &parent);
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

int main(void)
{
    TestAllAndCustomWorkingCopies();
    TestOfflineRemovalAndReordering();
    TestLimitsAndUnusableIdentities();
    TestParentRowsAndSaveValidation();
    TestSourceRulesAndLiveTelemetry();
    TestSourceStatusAndBuiltIn();
    if (failures) {
        printf("monitor selection UI: %d failure(s)\n", failures);
        return 1;
    }
    puts("ALL PASS: monitor picker working copies, source rules, live telemetry, Apply/Cancel/Save and identities (mocked UI)");
    return 0;
}
