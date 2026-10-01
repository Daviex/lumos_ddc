#include "ui_monitor_selection.h"
#include "ui.h"
#include "ui_graphics.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <strsafe.h>
#include <stdlib.h>
#include <string.h>

#define PICKER_WIDTH 460
#define PICKER_HEIGHT 460
#define PICKER_MAX_ROWS (MAX_MONITORS * 2)
#define PICKER_ALL 101
#define PICKER_SELECTED 102
#define PICKER_LIST 103
#define PICKER_HINT 104

typedef struct {
    WCHAR key[MONITOR_SELECTION_KEY_LEN];
    WCHAR name[128];
    BOOL connected;
    BOOL canAdd;
    BOOL checked;
} SelectionRow;

typedef struct {
    HWND owner, list, all, selected, apply, hint;
    MonitorSelection *destination;
    MonitorSelectionClosedCallback closed;
    SelectionRow rows[PICKER_MAX_ROWS];
    int count;
    BOOL selectedOnly, filling, applied, restoreOwner, notifyOwner;
} SelectionEdit;

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

/* Only stable keys enter the selection; copied names are display labels. */
static void BuildSelectionRows(SelectionEdit *edit, const MonitorSelection *selection,
                               const MonitorList *monitors)
{
    edit->count = 0;
    edit->selectedOnly = selection->selectedOnly;
    if (monitors) {
        for (int i = 0; i < monitors->count && i < MAX_MONITORS; i++) {
            const BrightMonitor *monitor = &monitors->monitors[i];
            if (!monitor->controllable) continue;
            WCHAR key[MONITOR_SELECTION_KEY_LEN] = { 0 };
            BOOL identified = Settings_MonitorKey(monitor, key);
            int match = identified ? FindSelectionRow(edit, key) : -1;
            if (match >= 0) {
                edit->rows[match].canAdd = FALSE; /* ambiguous identity */
                continue;
            }
            if (edit->count >= PICKER_MAX_ROWS) break;
            SelectionRow *row = &edit->rows[edit->count++];
            ZeroMemory(row, sizeof(*row));
            StringCchCopyW(row->key, ARRAYSIZE(row->key), key);
            StringCchCopyW(row->name, ARRAYSIZE(row->name), monitor->name);
            row->connected = TRUE;
            row->canAdd = identified;
        }
    }
    for (int i = 0; i < selection->count && i < MAX_MONITORS; i++) {
        if (!Settings_MonitorKeyValid(selection->keys[i])) continue;
        int match = FindSelectionRow(edit, selection->keys[i]);
        if (match < 0 && edit->count < PICKER_MAX_ROWS) {
            match = edit->count++;
            SelectionRow *row = &edit->rows[match];
            ZeroMemory(row, sizeof(*row));
            StringCchCopyW(row->key, ARRAYSIZE(row->key), selection->keys[i]);
            StringCchCopyW(row->name, ARRAYSIZE(row->name), selection->names[i]);
            if (!row->name[0]) StringCchCopyW(row->name, ARRAYSIZE(row->name), L"Saved monitor");
            row->canAdd = TRUE;
        }
        if (match >= 0) edit->rows[match].checked = TRUE;
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
        if (!row->checked || !row->key[0]) continue;
        if (result.count >= MAX_MONITORS) return FALSE;
        StringCchCopyW(result.keys[result.count], MONITOR_SELECTION_KEY_LEN, row->key);
        StringCchCopyW(result.names[result.count], 128, row->name);
        result.count++;
    }
    if (result.selectedOnly && result.count == 0) return FALSE;
    *edit->destination = result;
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
    COLORREF background = edit->selectedOnly ? UI_ColorRef(CLR_SURFACE) : GetSysColor(COLOR_BTNFACE);
    COLORREF foreground = edit->selectedOnly ? UI_ColorRef(CLR_TEXT) : GetSysColor(COLOR_GRAYTEXT);
    ListView_SetBkColor(edit->list, background);
    ListView_SetTextBkColor(edit->list, background);
    ListView_SetTextColor(edit->list, foreground);
    EnableWindow(edit->list, edit->selectedOnly);
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
    column.cx = 260;
    if (ListView_InsertColumn(edit->list, 0, &column) < 0) return FALSE;
    column.pszText = L"Status";
    column.cx = 140;
    if (ListView_InsertColumn(edit->list, 1, &column) < 0) return FALSE;
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
        WCHAR *status = !row->connected ? L"Not connected" :
                        row->canAdd ? L"Connected" : row->key[0] ? L"Ambiguous identity" : L"Identity unavailable";
        ListView_SetItemText(edit->list, i, 1, status);
        ListView_SetCheckState(edit->list, i, row->checked);
        if (!row->key[0]) ListView_SetItemState(edit->list, i, 0, LVIS_STATEIMAGEMASK);
    }
    edit->filling = FALSE;
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
        edit->list = PickerControl(hwnd, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | WS_BORDER | WS_TABSTOP | WS_GROUP,
                                   16, 76, PICKER_WIDTH - 32, 270, PICKER_LIST);
        edit->hint = PickerControl(hwnd, L"STATIC", L"", SS_LEFT, 16, 354, PICKER_WIDTH - 32, 32, PICKER_HINT);
        edit->apply = PickerControl(hwnd, L"BUTTON", L"Apply", BS_DEFPUSHBUTTON | WS_TABSTOP,
                                    PICKER_WIDTH - 198, 398, 84, 28, IDOK);
        HWND cancel = PickerControl(hwnd, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP,
                                     PICKER_WIDTH - 104, 398, 88, 28, IDCANCEL);
        if (!edit->all || !edit->selected || !edit->list || !edit->hint || !edit->apply || !cancel)
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
            checking && !wasChecked &&
            (!edit->rows[change->iItem].canAdd || SelectionCheckedCount(edit) >= MAX_MONITORS))
            return TRUE;
        if (change->hdr.code == LVN_ITEMCHANGED && (change->uChanged & LVIF_STATE) &&
            checking != wasChecked) {
            edit->rows[change->iItem].checked = checking;
            RefreshSelectionControls(edit);
        }
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
