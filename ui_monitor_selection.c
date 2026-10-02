#include "ui_monitor_selection.h"
#include "ui.h"
#include "ui_graphics.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <strsafe.h>
#include <stdlib.h>
#include <string.h>

#define PICKER_WIDTH 790
#define PICKER_HEIGHT 536
#define PICKER_MAX_ROWS (MAX_MONITORS * 3)
#define PICKER_ALL 101
#define PICKER_SELECTED 102
#define PICKER_LIST 103
#define PICKER_HINT 104
#define PICKER_SOURCE_FILTER 105
#define PICKER_INPUT 106
#define PICKER_ASSOCIATE 107

typedef struct {
    WCHAR key[MONITOR_SELECTION_KEY_LEN];
    WCHAR name[128];
    BOOL connected;
    BOOL canAdd;
    BOOL checked;
    BOOL builtIn;
    BOOL rulePresent;
    BOOL sourceFilter;
    DWORD expectedInput;
    BOOL sourceKnown;
    DWORD currentInput;
    ULONGLONG sourceCheckedTick;
} SelectionRow;

typedef struct {
    HWND owner, list, all, selected, apply, hint;
    HWND sourceFilter, input, associate;
    MonitorSelection *destination;
    MonitorSelectionClosedCallback closed;
    SelectionRow rows[PICKER_MAX_ROWS];
    int count;
    BOOL selectedOnly, filling, applied, restoreOwner, notifyOwner;
} SelectionEdit;

typedef struct { DWORD value; const WCHAR *label; } InputChoice;
static const InputChoice INPUT_CHOICES[] = {
    { 0, L"Not set" }, { 0x0F, L"DisplayPort 1" }, { 0x10, L"DisplayPort 2" },
    { 0x11, L"HDMI 1" }, { 0x12, L"HDMI 2" },
    { 0x01, L"VGA 1" }, { 0x02, L"VGA 2" },
    { 0x03, L"DVI 1" }, { 0x04, L"DVI 2" }
};

static const WCHAR PICKER_CLASS[] = L"LumosMonitorSelection";
static HINSTANCE g_pickerInstance;
static HWND g_pickerWindow;
static SelectionEdit *g_pickerPending;

static int FindSelectionRow(const SelectionEdit *edit, const WCHAR *key)
{
    if (!key[0]) return -1;
    for (int i = 0; i < edit->count; i++)
        if (_wcsicmp(edit->rows[i].key, key) == 0) return i;
    return -1;
}

static void FormatInput(DWORD input, WCHAR *text, size_t capacity)
{
    for (int i = 0; i < (int)ARRAYSIZE(INPUT_CHOICES); i++) {
        if (INPUT_CHOICES[i].value != input) continue;
        StringCchCopyW(text, capacity, INPUT_CHOICES[i].label);
        return;
    }
    StringCchPrintfW(text, capacity, L"Input 0x%02lX", (unsigned long)input);
}

static SelectionRow *AddSavedRow(SelectionEdit *edit, const WCHAR *key, const WCHAR *name)
{
    if (edit->count >= PICKER_MAX_ROWS) return NULL;
    SelectionRow *row = &edit->rows[edit->count++];
    ZeroMemory(row, sizeof(*row));
    StringCchCopyW(row->key, ARRAYSIZE(row->key), key);
    StringCchCopyW(row->name, ARRAYSIZE(row->name), name[0] ? name : L"Saved monitor");
    row->canAdd = TRUE;
    row->builtIn = _wcsnicmp(key, L"WMI:", 4) == 0;
    return row;
}

/* This merge never learns an association from a newly observed input. */
static void MergeSelectionTelemetry(SelectionEdit *edit, const MonitorList *monitors)
{
    for (int i = 0; i < edit->count; i++) {
        edit->rows[i].connected = FALSE;
        edit->rows[i].sourceKnown = FALSE;
    }
    if (!monitors) return;
    for (int i = 0; i < monitors->count && i < MAX_MONITORS; i++) {
        const BrightMonitor *monitor = &monitors->monitors[i];
        if (!monitor->controllable) continue;
        WCHAR key[MONITOR_SELECTION_KEY_LEN] = { 0 };
        BOOL identified = Settings_MonitorKey(monitor, key);
        int match = identified ? FindSelectionRow(edit, key) : -1;
        if (!identified) {
            for (int j = 0; j < edit->count; j++)
                if (!edit->rows[j].key[0] && !edit->rows[j].connected &&
                    wcscmp(edit->rows[j].name, monitor->name) == 0) { match = j; break; }
        }
        if (match >= 0 && edit->rows[match].connected) {
            edit->rows[match].canAdd = FALSE;
            edit->rows[match].sourceKnown = FALSE;
            continue;
        }
        SelectionRow *row;
        if (match >= 0) row = &edit->rows[match];
        else {
            row = AddSavedRow(edit, key, monitor->name);
            if (!row) break;
        }
        StringCchCopyW(row->name, ARRAYSIZE(row->name), monitor->name);
        row->connected = TRUE;
        row->canAdd = identified;
        row->builtIn = monitor->backend == BACKEND_WMI;
        row->sourceKnown = monitor->sourceKnown;
        row->currentInput = monitor->currentInput;
        row->sourceCheckedTick = monitor->sourceCheckedTick;
    }
}

/* Only stable keys enter the selection; copied names are display labels. */
static void BuildSelectionRows(SelectionEdit *edit, const MonitorSelection *selection,
                               const MonitorList *monitors)
{
    edit->count = 0;
    edit->selectedOnly = selection->selectedOnly;
    MergeSelectionTelemetry(edit, monitors);
    for (int i = 0; i < selection->count && i < MAX_MONITORS; i++) {
        if (!Settings_MonitorKeyValid(selection->keys[i])) continue;
        int match = FindSelectionRow(edit, selection->keys[i]);
        if (match < 0 && edit->count < PICKER_MAX_ROWS) {
            match = edit->count;
            AddSavedRow(edit, selection->keys[i], selection->names[i]);
        }
        if (match >= 0) edit->rows[match].checked = TRUE;
    }
    for (int i = 0; i < selection->inputRuleCount && i < MAX_MONITORS; i++) {
        const MonitorInputRule *rule = &selection->inputRules[i];
        if (!Settings_MonitorKeyValid(rule->key) || _wcsnicmp(rule->key, L"DDC:", 4) != 0) continue;
        int match = FindSelectionRow(edit, rule->key);
        if (match < 0 && edit->count < PICKER_MAX_ROWS) {
            match = edit->count;
            AddSavedRow(edit, rule->key, rule->name);
        }
        if (match < 0) continue;
        SelectionRow *row = &edit->rows[match];
        row->rulePresent = TRUE;
        row->sourceFilter = rule->enabled;
        row->expectedInput = rule->input;
    }
    /* Switching from the default All mode starts with connected, identifiable
       displays checked, while a saved custom selection keeps its own choices. */
    if (!selection->selectedOnly && selection->count == 0) {
        for (int i = 0; i < edit->count; i++)
            edit->rows[i].checked = edit->rows[i].canAdd && edit->rows[i].connected;
    }
}

static int SelectionCheckedCount(const SelectionEdit *edit)
{
    int count = 0;
    for (int i = 0; i < edit->count; i++)
        if (edit->rows[i].checked && edit->rows[i].key[0]) count++;
    return count;
}

/* Apply commits into the parent's working copy, never into live Settings. */
static BOOL CommitMonitorSelection(SelectionEdit *edit)
{
    MonitorSelection result = { 0 };
    result.selectedOnly = edit->selectedOnly;
    for (int i = 0; i < edit->count; i++) {
        const SelectionRow *row = &edit->rows[i];
        if (!row->key[0]) continue;
        if (row->checked) {
            if (result.count >= MAX_MONITORS) return FALSE;
            StringCchCopyW(result.keys[result.count], MONITOR_SELECTION_KEY_LEN, row->key);
            StringCchCopyW(result.names[result.count], 128, row->name);
            result.count++;
        }
        if (!row->builtIn && (row->sourceFilter || row->expectedInput)) {
            if (result.inputRuleCount >= MAX_MONITORS) return FALSE;
            MonitorInputRule *rule = &result.inputRules[result.inputRuleCount++];
            StringCchCopyW(rule->key, ARRAYSIZE(rule->key), row->key);
            StringCchCopyW(rule->name, ARRAYSIZE(rule->name), row->name);
            rule->enabled = row->sourceFilter;
            rule->input = row->expectedInput;
        }
    }
    if (result.selectedOnly && result.count == 0) return FALSE;
    *edit->destination = result;
    return TRUE;
}

static int SourceRuleCount(const SelectionEdit *edit)
{
    int count = 0;
    for (int i = 0; i < edit->count; i++) {
        const SelectionRow *row = &edit->rows[i];
        if (row->key[0] && !row->builtIn && (row->sourceFilter || row->expectedInput)) count++;
    }
    return count;
}

static BOOL CanEditSource(const SelectionEdit *edit, const SelectionRow *row)
{
    return row->key[0] && row->canAdd && !row->builtIn &&
           (row->sourceFilter || row->expectedInput ||
            SourceRuleCount(edit) < MAX_MONITORS);
}

static BOOL SourceTelemetryCurrent(const SelectionRow *row)
{
    return row->connected && row->sourceKnown && row->currentInput > 0 && row->currentInput <= 0xFF &&
           GetTickCount64() - row->sourceCheckedTick <= 10000;
}

static const WCHAR *SelectionRowStatus(const SelectionEdit *edit, const SelectionRow *row)
{
    if (!row->connected) return L"Not connected";
    if (!row->canAdd) return row->key[0] ? L"Ambiguous identity" : L"Identity unavailable";
    if (edit->selectedOnly && !row->checked) return L"Not selected";
    if (row->builtIn) return L"Built-in";
    if (!row->sourceFilter) return L"In control";
    if (!row->expectedInput) return L"Paused: input not set";
    if (!SourceTelemetryCurrent(row)) return L"Paused: input unavailable";
    return row->expectedInput == row->currentInput ? L"In control" : L"Paused: other input";
}

static void UpdateSelectionRow(SelectionEdit *edit, int index)
{
    SelectionRow *row = &edit->rows[index];
    WCHAR expected[40], current[40];
    if (row->builtIn) {
        StringCchCopyW(expected, ARRAYSIZE(expected), L"Built-in");
        StringCchCopyW(current, ARRAYSIZE(current), L"Built-in");
    } else {
        FormatInput(row->expectedInput, expected, ARRAYSIZE(expected));
        if (SourceTelemetryCurrent(row)) FormatInput(row->currentInput, current, ARRAYSIZE(current));
        else StringCchCopyW(current, ARRAYSIZE(current), L"Unavailable");
    }
    ListView_SetItemText(edit->list, index, 0, row->name);
    ListView_SetItemText(edit->list, index, 1, expected);
    ListView_SetItemText(edit->list, index, 2, current);
    ListView_SetItemText(edit->list, index, 3, (WCHAR *)SelectionRowStatus(edit, row));
}

static SelectionRow *EditingRow(SelectionEdit *edit)
{
    int index = ListView_GetNextItem(edit->list, -1, LVNI_SELECTED);
    return index >= 0 && index < edit->count ? &edit->rows[index] : NULL;
}

static void RefreshSourceEditor(SelectionEdit *edit, BOOL fillInput)
{
    SelectionRow *row = EditingRow(edit);
    BOOL enabled = row && CanEditSource(edit, row);
    EnableWindow(edit->sourceFilter, enabled);
    EnableWindow(edit->input, enabled);
    EnableWindow(edit->associate, enabled && SourceTelemetryCurrent(row));
    SendMessageW(edit->sourceFilter, BM_SETCHECK, row && row->sourceFilter ? BST_CHECKED : BST_UNCHECKED, 0);
    if (!fillInput) return;
    SendMessageW(edit->input, CB_RESETCONTENT, 0, 0);
    int selected = 0;
    BOOL found = !row;
    for (int i = 0; i < (int)ARRAYSIZE(INPUT_CHOICES); i++) {
        LRESULT index = SendMessageW(edit->input, CB_ADDSTRING, 0, (LPARAM)INPUT_CHOICES[i].label);
        if (index == CB_ERR || index == CB_ERRSPACE) continue;
        SendMessageW(edit->input, CB_SETITEMDATA, (WPARAM)index, INPUT_CHOICES[i].value);
        if (row && row->expectedInput == INPUT_CHOICES[i].value) { selected = (int)index; found = TRUE; }
    }
    if (!found && row) {
        WCHAR text[40];
        FormatInput(row->expectedInput, text, ARRAYSIZE(text));
        LRESULT index = SendMessageW(edit->input, CB_ADDSTRING, 0, (LPARAM)text);
        if (index != CB_ERR && index != CB_ERRSPACE) {
            SendMessageW(edit->input, CB_SETITEMDATA, (WPARAM)index, row->expectedInput);
            selected = (int)index;
        }
    }
    SendMessageW(edit->input, CB_SETCURSEL, selected, 0);
}

static BOOL AssociateCurrentInput(SelectionEdit *edit, SelectionRow *row)
{
    if (!row || !CanEditSource(edit, row) || !SourceTelemetryCurrent(row))
        return FALSE;
    row->expectedInput = row->currentInput;
    row->rulePresent = TRUE;
    return TRUE;
}

static void RefreshSelectionControls(SelectionEdit *edit)
{
    int count = SelectionCheckedCount(edit);
    BOOL unavailable = FALSE;
    for (int i = 0; i < edit->count; i++)
        if (edit->rows[i].checked && edit->rows[i].connected && !edit->rows[i].canAdd)
            unavailable = TRUE;
    WCHAR hint[160];
    if (!edit->selectedOnly)
        StringCchCopyW(hint, ARRAYSIZE(hint), L"Control all connected monitors, including new ones.");
    else if (!count)
        StringCchCopyW(hint, ARRAYSIZE(hint), L"Select at least one monitor.");
    else if (unavailable)
        StringCchPrintfW(hint, ARRAYSIZE(hint),
                         L"%d selected. Ambiguous identities cannot be controlled.", count);
    else
        StringCchPrintfW(hint, ARRAYSIZE(hint),
                         L"%d selected. Apply here, then Save in Settings.", count);
    SetWindowTextW(edit->hint, hint);
    for (int i = 0; i < edit->count; i++) UpdateSelectionRow(edit, i);
    RefreshSourceEditor(edit, FALSE);
    EnableWindow(edit->apply, !edit->selectedOnly || (count > 0 && count <= MAX_MONITORS));
    SendMessageW(edit->all, BM_SETCHECK, edit->selectedOnly ? BST_UNCHECKED : BST_CHECKED, 0);
    SendMessageW(edit->selected, BM_SETCHECK, edit->selectedOnly ? BST_CHECKED : BST_UNCHECKED, 0);
}

static HWND PickerControl(HWND hwnd, const WCHAR *type, const WCHAR *text,
                          DWORD style, int x, int y, int w, int h, int id)
{
    HWND control = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style,
                                  x, y, w, h, hwnd, (HMENU)(INT_PTR)id,
                                  g_pickerInstance, NULL);
    if (control)
        SendMessageW(control, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return control;
}

static BOOL PopulateSelectionList(SelectionEdit *edit)
{
    ListView_SetExtendedListViewStyle(edit->list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ListView_SetBkColor(edit->list, UI_ColorRef(CLR_SURFACE));
    ListView_SetTextBkColor(edit->list, UI_ColorRef(CLR_SURFACE));
    ListView_SetTextColor(edit->list, UI_ColorRef(CLR_TEXT));
    LVCOLUMNW column = { 0 };
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.pszText = L"Monitor";
    column.cx = 235;
    if (ListView_InsertColumn(edit->list, 0, &column) < 0) return FALSE;
    column.pszText = L"This PC input";
    column.cx = 150;
    if (ListView_InsertColumn(edit->list, 1, &column) < 0) return FALSE;
    column.pszText = L"Current input";
    column.cx = 145;
    if (ListView_InsertColumn(edit->list, 2, &column) < 0) return FALSE;
    column.pszText = L"Status";
    column.cx = 208;
    if (ListView_InsertColumn(edit->list, 3, &column) < 0) return FALSE;
    edit->filling = TRUE;
    for (int i = 0; i < edit->count; i++) {
        SelectionRow *row = &edit->rows[i];
        LVITEMW item = { 0 };
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = row->name;
        if (ListView_InsertItem(edit->list, &item) != i) {
            edit->filling = FALSE;
            return FALSE;
        }
        UpdateSelectionRow(edit, i);
        ListView_SetCheckState(edit->list, i, row->checked);
        if (!row->key[0]) ListView_SetItemState(edit->list, i, 0, LVIS_STATEIMAGEMASK);
    }
    edit->filling = FALSE;
    if (edit->count) ListView_SetItemState(edit->list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    RefreshSourceEditor(edit, TRUE);
    return TRUE;
}

static void CloseSelectionWindow(HWND hwnd, SelectionEdit *edit, BOOL apply)
{
    if (apply && !CommitMonitorSelection(edit)) return;
    edit->applied = apply;
    DestroyWindow(hwnd);
}

static LRESULT CALLBACK SelectionWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    SelectionEdit *edit = (SelectionEdit *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        edit = (SelectionEdit *)((CREATESTRUCTW *)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)edit);
        return TRUE;
    case WM_CREATE: {
        edit->all = PickerControl(hwnd, L"BUTTON", L"All Monitors", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP,
                                  16, 12, 200, 24, PICKER_ALL);
        edit->selected = PickerControl(hwnd, L"BUTTON", L"Only selected monitors", BS_AUTORADIOBUTTON,
                                       16, 40, 300, 24, PICKER_SELECTED);
        edit->list = PickerControl(hwnd, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP | WS_GROUP,
                                   16, 76, PICKER_WIDTH - 32, 230, PICKER_LIST);
        edit->sourceFilter = PickerControl(hwnd, L"BUTTON", L"Only when showing this PC", BS_AUTOCHECKBOX | WS_TABSTOP,
                                           16, 318, 360, 24, PICKER_SOURCE_FILTER);
        PickerControl(hwnd, L"STATIC", L"This PC input:", SS_LEFT, 16, 353, 100, 22, -1);
        edit->input = PickerControl(hwnd, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                                    122, 348, 225, 230, PICKER_INPUT);
        edit->associate = PickerControl(hwnd, L"BUTTON", L"Use current input for this PC", BS_PUSHBUTTON | WS_TABSTOP,
                                        360, 346, 285, 28, PICKER_ASSOCIATE);
        PickerControl(hwnd, L"STATIC",
                      L"Select a row to configure its source filter. Input names refer to ports on the monitor.\n"
                      L"Use the current input only while that monitor is showing this PC.",
                      SS_LEFT, 16, 385, PICKER_WIDTH - 32, 36, -1);
        edit->hint = PickerControl(hwnd, L"STATIC", L"", SS_LEFT, 16, 435, PICKER_WIDTH - 32, 32, PICKER_HINT);
        edit->apply = PickerControl(hwnd, L"BUTTON", L"Apply", BS_DEFPUSHBUTTON | WS_TABSTOP,
                                    PICKER_WIDTH - 198, 486, 84, 28, IDOK);
        HWND cancel = PickerControl(hwnd, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP,
                                     PICKER_WIDTH - 104, 486, 88, 28, IDCANCEL);
        if (!edit->all || !edit->selected || !edit->list || !edit->hint || !edit->apply || !cancel ||
            !edit->sourceFilter || !edit->input || !edit->associate)
            return -1;
        if (!PopulateSelectionList(edit)) return -1;
        RefreshSelectionControls(edit);
        return 0;
    }
    case WM_COMMAND:
        if (!edit) break;
        switch (LOWORD(wp)) {
        case PICKER_ALL: edit->selectedOnly = FALSE; RefreshSelectionControls(edit); return 0;
        case PICKER_SELECTED: edit->selectedOnly = TRUE; RefreshSelectionControls(edit); return 0;
        case PICKER_SOURCE_FILTER: {
            SelectionRow *row = EditingRow(edit);
            if (row && CanEditSource(edit, row)) {
                row->sourceFilter = SendMessageW(edit->sourceFilter, BM_GETCHECK, 0, 0) == BST_CHECKED;
                row->rulePresent = TRUE;
            }
            RefreshSelectionControls(edit);
            return 0;
        }
        case PICKER_INPUT:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                SelectionRow *row = EditingRow(edit);
                LRESULT index = SendMessageW(edit->input, CB_GETCURSEL, 0, 0);
                if (row && CanEditSource(edit, row) && index != CB_ERR) {
                    LRESULT value = SendMessageW(edit->input, CB_GETITEMDATA, (WPARAM)index, 0);
                    if (value != CB_ERR) { row->expectedInput = (DWORD)value; row->rulePresent = TRUE; }
                }
                RefreshSelectionControls(edit);
            }
            return 0;
        case PICKER_ASSOCIATE:
            if (AssociateCurrentInput(edit, EditingRow(edit))) {
                RefreshSourceEditor(edit, TRUE);
                RefreshSelectionControls(edit);
            }
            return 0;
        case IDOK: CloseSelectionWindow(hwnd, edit, TRUE); return 0;
        case IDCANCEL: CloseSelectionWindow(hwnd, edit, FALSE); return 0;
        }
        break;
    case WM_NOTIFY: {
        if (!edit || edit->filling) return 0;
        NMHDR *header = (NMHDR *)lp;
        if (!header || header->idFrom != PICKER_LIST ||
            (header->code != LVN_ITEMCHANGING && header->code != LVN_ITEMCHANGED))
            return 0;
        NMLISTVIEW *change = (NMLISTVIEW *)lp;
        if (change->iItem < 0 || change->iItem >= edit->count)
            return 0;
        BOOL checking = (change->uNewState & LVIS_STATEIMAGEMASK) == INDEXTOSTATEIMAGEMASK(2);
        BOOL wasChecked = (change->uOldState & LVIS_STATEIMAGEMASK) == INDEXTOSTATEIMAGEMASK(2);
        if (change->hdr.code == LVN_ITEMCHANGING && (change->uChanged & LVIF_STATE) &&
            checking != wasChecked &&
            (!edit->selectedOnly || (checking &&
             (!edit->rows[change->iItem].canAdd || SelectionCheckedCount(edit) >= MAX_MONITORS))))
            return TRUE;
        if (change->hdr.code == LVN_ITEMCHANGED && (change->uChanged & LVIF_STATE) &&
            checking != wasChecked) {
            edit->rows[change->iItem].checked = checking;
            RefreshSelectionControls(edit);
        }
        if (change->hdr.code == LVN_ITEMCHANGED &&
            ((change->uOldState ^ change->uNewState) & LVIS_SELECTED))
            RefreshSourceEditor(edit, TRUE);
        return 0;
    }
    case DM_GETDEFID: return MAKELRESULT(IDOK, DC_HASDEFID);
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && edit) { CloseSelectionWindow(hwnd, edit, FALSE); return 0; }
        break;
    case WM_CLOSE:
        if (edit) CloseSelectionWindow(hwnd, edit, FALSE);
        return 0;
    case WM_NCDESTROY:
        if (g_pickerWindow == hwnd) g_pickerWindow = NULL;
        if (edit) {
            HWND owner = edit->owner;
            BOOL restore = edit->restoreOwner && IsWindow(owner);
            MonitorSelectionClosedCallback closed = edit->notifyOwner ? edit->closed : NULL;
            BOOL applied = edit->applied;
            if (g_pickerPending == edit) g_pickerPending = NULL;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            free(edit);
            if (restore) EnableWindow(owner, TRUE);
            if (closed) closed(applied);
            if (restore) { SetForegroundWindow(owner); SetFocus(owner); }
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

BOOL UI_MonitorSelectionInit(HINSTANCE instance)
{
    g_pickerInstance = instance;
    INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_LISTVIEW_CLASSES };
    if (!InitCommonControlsEx(&controls)) return FALSE;
    WNDCLASSEXW window = { 0 };
    window.cbSize = sizeof(window);
    window.hInstance = instance;
    window.lpfnWndProc = SelectionWndProc;
    window.hCursor = LoadCursor(NULL, IDC_ARROW);
    window.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    window.lpszClassName = PICKER_CLASS;
    return RegisterClassExW(&window) != 0;
}

HWND UI_ShowMonitorSelection(HWND owner, MonitorSelection *working,
                             const MonitorList *monitors,
                             MonitorSelectionClosedCallback closed)
{
    if (g_pickerWindow) { SetForegroundWindow(g_pickerWindow); return g_pickerWindow; }
    SelectionEdit *edit = (SelectionEdit *)calloc(1, sizeof(*edit));
    if (!edit) return NULL;
    edit->owner = owner;
    edit->destination = working;
    edit->closed = closed;
    BuildSelectionRows(edit, working, monitors);
    RECT parent = { 0 };
    GetWindowRect(owner, &parent);
    RECT size = { 0, 0, PICKER_WIDTH, PICKER_HEIGHT };
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
    DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
    AdjustWindowRectEx(&size, style, FALSE, exStyle);
    int width = size.right - size.left, height = size.bottom - size.top;
    HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = { 0 };
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info))
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &info.rcWork, 0);
    int x = parent.left + ((parent.right - parent.left) - width) / 2;
    int y = parent.top + 24;
    if (x + width > info.rcWork.right) x = info.rcWork.right - width;
    if (y + height > info.rcWork.bottom) y = info.rcWork.bottom - height;
    if (x < info.rcWork.left) x = info.rcWork.left;
    if (y < info.rcWork.top) y = info.rcWork.top;
    g_pickerPending = edit;
    g_pickerWindow = CreateWindowExW(exStyle, PICKER_CLASS, L"Choose Monitors", style,
                                      x, y, width, height, owner, NULL, g_pickerInstance, edit);
    if (!g_pickerWindow) {
        /* WM_NCDESTROY owns data after WM_NCCREATE. Earlier failures leave it here. */
        if (g_pickerPending) free(g_pickerPending);
        g_pickerPending = NULL;
        return NULL;
    }
    g_pickerPending = NULL;
    edit->restoreOwner = TRUE;
    edit->notifyOwner = TRUE;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_pickerWindow, 20, &dark, sizeof(dark));
    EnableWindow(owner, FALSE);
    ShowWindow(g_pickerWindow, SW_SHOW);
    SetForegroundWindow(g_pickerWindow);
    SetFocus(edit->selectedOnly ? edit->selected : edit->all);
    return g_pickerWindow;
}

void UI_CloseMonitorSelection(void)
{
    if (!g_pickerWindow) return;
    SelectionEdit *edit = (SelectionEdit *)GetWindowLongPtrW(g_pickerWindow, GWLP_USERDATA);
    if (edit) edit->restoreOwner = FALSE;
    DestroyWindow(g_pickerWindow);
}

BOOL UI_MonitorSelectionMessage(MSG *message)
{
    return g_pickerWindow && IsDialogMessageW(g_pickerWindow, message);
}

BOOL UI_MonitorSelectionIsOpen(void)
{
    return g_pickerWindow != NULL;
}

void UI_MonitorSelectionRefresh(const MonitorList *monitors)
{
    if (!g_pickerWindow) return;
    SelectionEdit *edit = (SelectionEdit *)GetWindowLongPtrW(g_pickerWindow, GWLP_USERDATA);
    if (!edit) return;
    int oldCount = edit->count;
    MergeSelectionTelemetry(edit, monitors);
    edit->filling = TRUE;
    for (int i = oldCount; i < edit->count; i++) {
        LVITEMW item = { 0 };
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = edit->rows[i].name;
        ListView_InsertItem(edit->list, &item);
        ListView_SetCheckState(edit->list, i, edit->rows[i].checked);
        if (!edit->rows[i].key[0]) ListView_SetItemState(edit->list, i, 0, LVIS_STATEIMAGEMASK);
    }
    edit->filling = FALSE;
    RefreshSelectionControls(edit);
}
