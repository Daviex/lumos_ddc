#include "ui_internal.h"

static const WCHAR SET_CLASS[]     = L"LumosSettings";
static HWND g_setHwnd = NULL;

/* ---- Settings window ---- */

/* Rows are data, not code: the table below drives rendering, hit testing and
   editing, so adding a setting later is one BuildSettingsRows line. */
enum { SET_SECTION = 0, SET_TOGGLE, SET_NUMBER };
enum { SET_UNIT_PLAIN = 0, SET_UNIT_PERCENT, SET_UNIT_MINUTES };

/* Hit kinds returned by SetHitTest. */
enum { SETHIT_NONE = 0, SETHIT_MINUS, SETHIT_PLUS, SETHIT_TOGGLE, SETHIT_ROW };

typedef struct {
    int    kind;
    WCHAR  label[MAX_PRESET_NAME + 16];
    int   *ival;      /* SET_NUMBER: the value being edited */
    BOOL  *bval;      /* SET_TOGGLE: the flag being edited */
    int    lo, hi;    /* SET_NUMBER bounds */
    int    step;      /* SET_NUMBER increment (minutes scale instead, see SetStepFor) */
    int    unit;
} SetRow;

#define MAX_SET_ROWS (MAX_PRESETS + 12)

typedef struct {
    /* Working copy. Edits are discarded unless the user hits Save, which is why
       the window never writes into Settings directly. */
    int   step;
    BOOL  autostart;
    BOOL  scheduleEnabled;
    BOOL  idleDimEnabled;
    int   idleDimPercent;
    int   idleDimMinutes;
    int   presetValues[MAX_PRESETS];
    int   presetCount;

    SetRow rows[MAX_SET_ROWS];
    int    rowCount;
    int    hoverRow;
    Settings *settings;
    HWND   owner;
} SetEditData;

static SetEditData g_set;

static SetRow *SetAddRow(SetEditData *d, int kind, const WCHAR *label)
{
    if (d->rowCount >= MAX_SET_ROWS) return NULL;
    SetRow *r = &d->rows[d->rowCount++];
    memset(r, 0, sizeof(*r));
    r->kind = kind;
    wcsncpy(r->label, label, (sizeof(r->label) / sizeof(WCHAR)) - 1);
    return r;
}

static void SetAddToggle(SetEditData *d, const WCHAR *label, BOOL *val)
{
    SetRow *r = SetAddRow(d, SET_TOGGLE, label);
    if (r) r->bval = val;
}

static void SetAddNumber(SetEditData *d, const WCHAR *label, int *val,
                         int lo, int hi, int step, int unit)
{
    SetRow *r = SetAddRow(d, SET_NUMBER, label);
    if (!r) return;
    r->ival = val;
    r->lo = lo;
    r->hi = hi;
    r->step = step;
    r->unit = unit;
}

static void BuildSettingsRows(SetEditData *d)
{
    d->rowCount = 0;

    SetAddRow(d, SET_SECTION, L"GENERAL");
    SetAddNumber(d, L"Brightness step", &d->step, 1, 50, 1, SET_UNIT_PERCENT);
    SetAddToggle(d, L"Start with Windows", &d->autostart);

    SetAddRow(d, SET_SECTION, L"IDLE DIM");
    SetAddToggle(d, L"Dim when idle", &d->idleDimEnabled);
    SetAddNumber(d, L"Idle level", &d->idleDimPercent, 0, 100, 1, SET_UNIT_PERCENT);
    SetAddNumber(d, L"Dim after", &d->idleDimMinutes, 1, 1440, 5, SET_UNIT_MINUTES);

    SetAddRow(d, SET_SECTION, L"SCHEDULE");
    SetAddToggle(d, L"Brightness schedule", &d->scheduleEnabled);

    if (d->presetCount > 0) {
        SetAddRow(d, SET_SECTION, L"PRESETS");
        for (int i = 0; i < d->presetCount; i++)
            SetAddNumber(d, d->settings->presets[i].name, &d->presetValues[i],
                         0, 100, 5, SET_UNIT_PERCENT);
    }
}

static int SetRowHeight(SetRow *r)
{
    return (r->kind == SET_SECTION) ? SET_SECTION_H : SET_ROW_H;
}

static int SetHeight(SetEditData *d)
{
    int h = SET_HEADER_H + SET_FOOTER_H;
    for (int i = 0; i < d->rowCount; i++)
        h += SetRowHeight(&d->rows[i]);
    return h;
}

/* Number controls sit on the right edge: [-] value [+] */
static void SetControlRects(int top, RECT *rcMinus, RECT *rcValue, RECT *rcPlus)
{
    int t = top + 4, b = top + SET_ROW_H - 4;
    rcMinus->left = SET_WIDTH - 116; rcMinus->right = SET_WIDTH - 92;
    rcValue->left = SET_WIDTH - 90;  rcValue->right = SET_WIDTH - 44;
    rcPlus->left  = SET_WIDTH - 40;  rcPlus->right  = SET_WIDTH - 16;
    rcMinus->top = rcValue->top = rcPlus->top = t;
    rcMinus->bottom = rcValue->bottom = rcPlus->bottom = b;
}

static void SetToggleRect(int top, RECT *rc)
{
    rc->right  = SET_WIDTH - 16;
    rc->left   = rc->right - 36;
    rc->top    = top + (SET_ROW_H - 20) / 2;
    rc->bottom = rc->top + 20;
}

static void SetSaveRect(SetEditData *d, RECT *rc)
{
    int y = SetHeight(d) - SET_FOOTER_H + 10;
    rc->right = SET_WIDTH - 16; rc->left = SET_WIDTH - 112;
    rc->top = y; rc->bottom = y + 28;
}

/* Minutes run from 1 to 1440, so the increment scales with the value: a fixed
   step is either too coarse near 1 or takes hundreds of clicks near 1440. */
static int SetStepFor(SetRow *r, int value)
{
    if (r->unit != SET_UNIT_MINUTES) return r->step;
    if (value < 15) return 1;
    if (value < 60) return 5;
    return 15;
}

static void SetAdjust(SetRow *r, int dir)
{
    if (r->kind != SET_NUMBER || !r->ival) return;
    int v = *r->ival;
    /* Stepping down uses the bucket below the current value, so the same click
       count walks a value back to where it came from. */
    v += (dir > 0) ? SetStepFor(r, v) : -SetStepFor(r, v - 1);
    if (v < r->lo) v = r->lo;
    if (v > r->hi) v = r->hi;
    *r->ival = v;
}

/* Returns the row under (x,y) and sets *outHit, or -1 for none. */
static int SetHitTest(SetEditData *d, int x, int y, int *outHit)
{
    *outHit = SETHIT_NONE;
    int top = SET_HEADER_H;
    for (int i = 0; i < d->rowCount; i++) {
        SetRow *r = &d->rows[i];
        int rh = SetRowHeight(r);
        if (y >= top && y < top + rh) {
            if (r->kind == SET_SECTION) return -1;
            if (r->kind == SET_TOGGLE) {
                RECT rc;
                SetToggleRect(top, &rc);
                *outHit = (x >= rc.left && x <= rc.right) ? SETHIT_TOGGLE : SETHIT_ROW;
                return i;
            }
            RECT rcMinus, rcValue, rcPlus;
            SetControlRects(top, &rcMinus, &rcValue, &rcPlus);
            if (x >= rcMinus.left && x <= rcMinus.right)     *outHit = SETHIT_MINUS;
            else if (x >= rcPlus.left && x <= rcPlus.right)   *outHit = SETHIT_PLUS;
            else                                             *outHit = SETHIT_ROW;
            return i;
        }
        top += rh;
    }
    return -1;
}

static void RenderSettings(HWND hwnd, SetEditData *d)
{
    int w = SET_WIDTH;
    int h = SetHeight(d);

    BYTE *bits = NULL;
    HBITMAP bmp = NULL;
    HDC dc = CreateAlphaDC(w, h, &bmp, &bits);

    HBRUSH bg = CreateSolidBrush(HexToColorRef(CLR_BG));
    RECT rcAll = { 0, 0, w, h };
    FillRect(dc, &rcAll, bg);
    DeleteObject(bg);

    SetBkMode(dc, TRANSPARENT);
    HFONT hFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hFontBold = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hFontSmall = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    /* Title + hint */
    RECT rcTitle = { 16, 12, w - 16, 32 };
    SelectObject(dc, hFontBold);
    SetTextColor(dc, HexToColorRef(CLR_TEXT));
    DrawTextW(dc, L"Settings", -1, &rcTitle, DT_LEFT | DT_SINGLELINE);
    SelectObject(dc, hFontSmall);
    SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
    DrawTextW(dc, L"wheel = adjust", -1, &rcTitle, DT_RIGHT | DT_SINGLELINE);

    HPEN noPen = CreatePen(PS_NULL, 0, 0);
    HPEN oldPen = (HPEN)SelectObject(dc, noPen);

    int y = SET_HEADER_H;
    for (int i = 0; i < d->rowCount; i++) {
        SetRow *r = &d->rows[i];

        if (r->kind == SET_SECTION) {
            RECT rc = { 16, y, w - 16, y + SET_SECTION_H };
            SelectObject(dc, hFontSmall);
            SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
            DrawTextW(dc, r->label, -1, &rc, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
            y += SET_SECTION_H;
            continue;
        }

        if (i == d->hoverRow) {
            HBRUSH hb = CreateSolidBrush(HexToColorRef(CLR_SURFACE));
            HBRUSH ob = (HBRUSH)SelectObject(dc, hb);
            RoundRect(dc, 8, y + 2, w - 8, y + SET_ROW_H - 2, 8, 8);
            SelectObject(dc, ob);
            DeleteObject(hb);
        }

        RECT rcLabel = { 16, y, w - 124, y + SET_ROW_H };
        SelectObject(dc, hFont);
        SetTextColor(dc, HexToColorRef(CLR_TEXT));
        DrawTextW(dc, r->label, -1, &rcLabel,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        if (r->kind == SET_TOGGLE) {
            RECT rcT;
            SetToggleRect(y, &rcT);
            BOOL on = (r->bval && *r->bval);
            HBRUSH track = CreateSolidBrush(HexToColorRef(on ? CLR_ACCENT : CLR_TRACK));
            HBRUSH knob  = CreateSolidBrush(HexToColorRef(on ? CLR_BG : CLR_SUBTEXT));
            HBRUSH ob = (HBRUSH)SelectObject(dc, track);
            RoundRect(dc, rcT.left, rcT.top, rcT.right, rcT.bottom, 20, 20);
            SelectObject(dc, knob);
            int kd = (rcT.bottom - rcT.top) - 6;   /* knob diameter, 3px inset */
            int kx = on ? rcT.right - 3 - kd : rcT.left + 3;
            Ellipse(dc, kx, rcT.top + 3, kx + kd, rcT.top + 3 + kd);
            SelectObject(dc, ob);
            DeleteObject(track);
            DeleteObject(knob);
        } else {
            RECT rcMinus, rcValue, rcPlus;
            SetControlRects(y, &rcMinus, &rcValue, &rcPlus);

            HBRUSH btn = CreateSolidBrush(HexToColorRef(CLR_TRACK));
            HBRUSH ob = (HBRUSH)SelectObject(dc, btn);
            RoundRect(dc, rcMinus.left, rcMinus.top, rcMinus.right, rcMinus.bottom, 6, 6);
            RoundRect(dc, rcPlus.left, rcPlus.top, rcPlus.right, rcPlus.bottom, 6, 6);
            SelectObject(dc, ob);
            DeleteObject(btn);

            SelectObject(dc, hFontSmall);
            SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
            DrawTextW(dc, L"\x2013", -1, &rcMinus, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DrawTextW(dc, L"+", -1, &rcPlus, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            WCHAR val[16];
            int v = r->ival ? *r->ival : 0;
            if (r->unit == SET_UNIT_MINUTES)      wsprintfW(val, L"%dm", v);
            else if (r->unit == SET_UNIT_PERCENT) wsprintfW(val, L"%d%%", v);
            else                                  wsprintfW(val, L"%d", v);
            SelectObject(dc, hFont);
            SetTextColor(dc, HexToColorRef(CLR_ACCENT));
            DrawTextW(dc, val, -1, &rcValue, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        y += SET_ROW_H;
    }

    /* Footer: Save on the right, cancel hint on the left */
    RECT rcSave;
    SetSaveRect(d, &rcSave);
    HBRUSH acc = CreateSolidBrush(HexToColorRef(CLR_ACCENT));
    HBRUSH oldBr = (HBRUSH)SelectObject(dc, acc);
    RoundRect(dc, rcSave.left, rcSave.top, rcSave.right, rcSave.bottom, 8, 8);
    SelectObject(dc, oldBr);
    DeleteObject(acc);

    SelectObject(dc, hFont);
    SetTextColor(dc, HexToColorRef(CLR_BG));
    DrawTextW(dc, L"Save", -1, &rcSave, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT rcNote = { 16, rcSave.top, rcSave.left - 8, rcSave.bottom };
    SelectObject(dc, hFontSmall);
    SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
    DrawTextW(dc, L"click outside to cancel", -1, &rcNote,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dc, oldPen);
    DeleteObject(noPen);
    DeleteObject(hFont);
    DeleteObject(hFontBold);
    DeleteObject(hFontSmall);

    ApplyRoundedMask(bits, w, h, SET_CORNER, 245);
    CommitLayered(hwnd, dc, w, h);

    DeleteObject(bmp);
    DeleteDC(dc);
}

/* Copy the working values back into Settings. Only the fields this window owns
   are touched; the schedule points stay with the schedule editor. */
static void SetCommit(SetEditData *d)
{
    Settings *s = d->settings;
    s->step            = d->step;
    s->autostart       = d->autostart;
    s->scheduleEnabled = d->scheduleEnabled;
    s->idleDimEnabled  = d->idleDimEnabled;
    s->idleDimPercent  = d->idleDimPercent;
    s->idleDimMinutes  = d->idleDimMinutes;
    for (int i = 0; i < d->presetCount && i < s->presetCount; i++)
        s->presets[i].brightness = (DWORD)d->presetValues[i];
}

static LRESULT CALLBACK SetWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    SetEditData *d = &g_set;

    switch (msg) {
    case WM_LBUTTONDOWN: {
        int x = LOWORD(lParam), y = HIWORD(lParam);

        RECT rcSave;
        SetSaveRect(d, &rcSave);
        if (x >= rcSave.left && x <= rcSave.right && y >= rcSave.top && y <= rcSave.bottom) {
            SetCommit(d);
            HWND owner = d->owner;
            DestroyWindow(hwnd);
            g_setHwnd = NULL;
            PostMessageW(owner, WM_COMMAND, (WPARAM)IDM_SETTINGS_SAVED, 0);
            return 0;
        }

        int hit;
        int row = SetHitTest(d, x, y, &hit);
        if (row >= 0) {
            SetRow *r = &d->rows[row];
            if (hit == SETHIT_TOGGLE && r->bval) *r->bval = !*r->bval;
            else if (hit == SETHIT_MINUS)        SetAdjust(r, -1);
            else if (hit == SETHIT_PLUS)         SetAdjust(r, +1);
            d->hoverRow = row;
            RenderSettings(hwnd, d);
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        int hit;
        int row = SetHitTest(d, LOWORD(lParam), HIWORD(lParam), &hit);
        if (row != d->hoverRow) {
            d->hoverRow = row;
            RenderSettings(hwnd, d);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int dir = ((short)HIWORD(wParam) > 0) ? 1 : -1;
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        ScreenToClient(hwnd, &pt);
        int hit;
        int row = SetHitTest(d, pt.x, pt.y, &hit);
        if (row >= 0 && d->rows[row].kind == SET_NUMBER) {
            SetAdjust(&d->rows[row], dir);
            d->hoverRow = row;
            RenderSettings(hwnd, d);
        }
        return 0;
    }

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            /* Dismiss without saving on click-outside, like the schedule editor. */
            DestroyWindow(hwnd);
            g_setHwnd = NULL;
        }
        return 0;

    case WM_DESTROY:
        g_setHwnd = NULL;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void UI_ShowSettings(HWND hwndOwner, Settings *s)
{
    if (g_setHwnd && IsWindow(g_setHwnd)) {
        DestroyWindow(g_setHwnd);
        g_setHwnd = NULL;
    }

    memset(&g_set, 0, sizeof(g_set));
    g_set.settings = s;
    g_set.owner = hwndOwner;
    g_set.hoverRow = -1;
    g_set.step = s->step;
    g_set.autostart = Settings_GetAutostart();   /* the registry is the truth here */
    g_set.scheduleEnabled = s->scheduleEnabled;
    g_set.idleDimEnabled = s->idleDimEnabled;
    g_set.idleDimPercent = s->idleDimPercent;
    g_set.idleDimMinutes = s->idleDimMinutes;
    g_set.presetCount = s->presetCount;
    for (int i = 0; i < s->presetCount; i++)
        g_set.presetValues[i] = (int)s->presets[i].brightness;
    BuildSettingsRows(&g_set);

    int w = SET_WIDTH;
    int h = SetHeight(&g_set);

    POINT pt;
    GetCursorPos(&pt);
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hMon, &mi);

    int x = pt.x;
    int y = pt.y - h;
    if (y < mi.rcWork.top) y = pt.y;
    if (x + w > mi.rcWork.right) x = mi.rcWork.right - w;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;
    if (y < mi.rcWork.top) y = mi.rcWork.top;   /* taller than the work area */

    g_setHwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        SET_CLASS, L"", WS_POPUP,
        x, y, w, h, NULL, NULL, g_uiInst, NULL);
    if (!g_setHwnd) return;

    RenderSettings(g_setHwnd, &g_set);
    ShowWindow(g_setHwnd, SW_SHOWNOACTIVATE);
    SetForegroundWindow(g_setHwnd);
}

/* ---- Class registration ---- */

void UiSettings_Init(HINSTANCE hInst)
{
    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SetWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = SET_CLASS;
    RegisterClassExW(&wc);
}

void UiSettings_Shutdown(void)
{
    if (g_setHwnd) {
        DestroyWindow(g_setHwnd);
        g_setHwnd = NULL;
    }
}
