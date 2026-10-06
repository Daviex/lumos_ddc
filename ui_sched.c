#include "ui_internal.h"

static const WCHAR SCHED_CLASS[]   = L"LumosSched";
static HWND g_schedHwnd = NULL;

/* ---- Schedule editor ---- */

enum { SF_HOUR = 0, SF_MIN = 1, SF_BRI = 2, SF_DEL = 3 };  /* field kinds */

typedef struct {
    SchedulePoint pts[MAX_SCHEDULE];
    int      count;
    int      selectedRow;
    Settings *settings;
    HWND     owner;
} SchedEditData;

static SchedEditData g_sched;

static int SchedHeight(SchedEditData *d)
{
    return SCHED_HEADER_H + d->count * SCHED_ROW_H + SCHED_FOOTER_H;
}

/* Field rects for a row. x zones: HH | MM | NN% | [x] */
static void SchedFieldRect(int row, int field, RECT *rc)
{
    int y = SCHED_HEADER_H + row * SCHED_ROW_H;
    rc->top = y + 4;
    rc->bottom = y + SCHED_ROW_H - 4;
    switch (field) {
        case SF_HOUR: rc->left = 16;  rc->right = 56;  break;
        case SF_MIN:  rc->left = 64;  rc->right = 104; break;
        case SF_BRI:  rc->left = 128; rc->right = 196; break;
        case SF_DEL:  rc->left = SCHED_WIDTH - 40; rc->right = SCHED_WIDTH - 16; break;
        default:      rc->left = 0;   rc->right = 0;   break;
    }
}

/* Footer button rects: Add (left), Save (right). */
static void SchedButtonRects(SchedEditData *d, RECT *rcAdd, RECT *rcSave)
{
    int y = SCHED_HEADER_H + d->count * SCHED_ROW_H + 8;
    rcAdd->left = 16;  rcAdd->right = 120;
    rcAdd->top = y;    rcAdd->bottom = y + 28;
    rcSave->right = SCHED_WIDTH - 16; rcSave->left = SCHED_WIDTH - 120;
    rcSave->top = y;   rcSave->bottom = y + 28;
}

/* Returns row index and sets *outField, or -1. */
static int SchedHitField(SchedEditData *d, int x, int y, int *outField)
{
    for (int row = 0; row < d->count; row++) {
        for (int f = SF_HOUR; f <= SF_DEL; f++) {
            RECT rc;
            SchedFieldRect(row, f, &rc);
            if (x >= rc.left && x <= rc.right && y >= rc.top && y <= rc.bottom) {
                *outField = f;
                return row;
            }
        }
    }
    *outField = -1;
    return -1;
}

/* Adjust a field by +/- delta with wrap/clamp. */
static void SchedAdjust(SchedEditData *d, int row, int field, int dir)
{
    if (row < 0 || row >= d->count) return;
    SchedulePoint *p = &d->pts[row];
    int h = p->minutes / 60, m = p->minutes % 60;
    if (field == SF_HOUR) {
        h = (h + dir + 24) % 24;
        p->minutes = h * 60 + m;
    } else if (field == SF_MIN) {
        m += dir * 5;                 /* 5-minute granularity */
        while (m < 0)  { m += 60; }
        while (m >= 60){ m -= 60; }
        p->minutes = h * 60 + m;
    } else if (field == SF_BRI) {
        int b = p->brightness + dir * 5;
        if (b < 0) b = 0;
        if (b > 100) b = 100;
        p->brightness = b;
    }
}

static void RenderSchedEditor(HWND hwnd, SchedEditData *d)
{
    int w = SCHED_WIDTH;
    int h = SchedHeight(d);

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
    SelectObject(dc, hFontBold);
    SetTextColor(dc, HexToColorRef(CLR_TEXT));
    RECT rcTitle = { 16, 12, w - 16, 32 };
    DrawTextW(dc, L"Brightness Schedule", -1, &rcTitle, DT_LEFT | DT_SINGLELINE);
    SelectObject(dc, hFontSmall);
    SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
    RECT rcHint = { 16, 12, w - 16, 32 };
    DrawTextW(dc, L"wheel = adjust", -1, &rcHint, DT_RIGHT | DT_SINGLELINE);

    HPEN noPen = CreatePen(PS_NULL, 0, 0);
    HPEN oldPen = (HPEN)SelectObject(dc, noPen);

    for (int row = 0; row < d->count; row++) {
        int y = SCHED_HEADER_H + row * SCHED_ROW_H;

        if (row == d->selectedRow) {
            HBRUSH hb = CreateSolidBrush(HexToColorRef(CLR_SURFACE));
            HBRUSH ob = (HBRUSH)SelectObject(dc, hb);
            RoundRect(dc, 8, y + 2, w - 8, y + SCHED_ROW_H - 2, 8, 8);
            SelectObject(dc, ob);
            DeleteObject(hb);
        }

        SchedulePoint *p = &d->pts[row];
        WCHAR s[16];
        RECT rc;

        SelectObject(dc, hFont);
        SetTextColor(dc, HexToColorRef(CLR_TEXT));
        SchedFieldRect(row, SF_HOUR, &rc);
        wsprintfW(s, L"%02d", p->minutes / 60);
        DrawTextW(dc, s, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        RECT rcColon = rc; rcColon.left = rc.right; rcColon.right = rc.right + 8;
        DrawTextW(dc, L":", -1, &rcColon, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SchedFieldRect(row, SF_MIN, &rc);
        wsprintfW(s, L"%02d", p->minutes % 60);
        DrawTextW(dc, s, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SchedFieldRect(row, SF_BRI, &rc);
        SetTextColor(dc, HexToColorRef(CLR_ACCENT));
        wsprintfW(s, L"%d%%", p->brightness);
        DrawTextW(dc, s, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SchedFieldRect(row, SF_DEL, &rc);
        SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
        DrawTextW(dc, L"\x2715", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE); /* x */
    }

    /* Footer buttons */
    RECT rcAdd, rcSave;
    SchedButtonRects(d, &rcAdd, &rcSave);
    HBRUSH btn = CreateSolidBrush(HexToColorRef(CLR_SURFACE));
    HBRUSH acc = CreateSolidBrush(HexToColorRef(CLR_ACCENT));
    HBRUSH ob = (HBRUSH)SelectObject(dc, btn);
    RoundRect(dc, rcAdd.left, rcAdd.top, rcAdd.right, rcAdd.bottom, 8, 8);
    SelectObject(dc, acc);
    RoundRect(dc, rcSave.left, rcSave.top, rcSave.right, rcSave.bottom, 8, 8);
    SelectObject(dc, ob);
    DeleteObject(btn);
    DeleteObject(acc);

    SelectObject(dc, hFont);
    SetTextColor(dc, HexToColorRef(CLR_TEXT));
    DrawTextW(dc, L"+ Add", -1, &rcAdd, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(dc, HexToColorRef(CLR_BG));
    DrawTextW(dc, L"Save", -1, &rcSave, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dc, oldPen);
    DeleteObject(noPen);
    DeleteObject(hFont);
    DeleteObject(hFontBold);
    DeleteObject(hFontSmall);

    ApplyRoundedMask(bits, w, h, SCHED_CORNER, 245);
    CommitLayered(hwnd, dc, w, h);

    DeleteObject(bmp);
    DeleteDC(dc);
}

static void SchedResize(HWND hwnd, SchedEditData *d)
{
    SetWindowPos(hwnd, NULL, 0, 0, SCHED_WIDTH, SchedHeight(d),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static LRESULT CALLBACK SchedWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    SchedEditData *d = &g_sched;

    switch (msg) {
    case WM_LBUTTONDOWN: {
        int x = LOWORD(lParam), y = HIWORD(lParam);

        /* Footer buttons */
        RECT rcAdd, rcSave;
        SchedButtonRects(d, &rcAdd, &rcSave);
        if (x >= rcAdd.left && x <= rcAdd.right && y >= rcAdd.top && y <= rcAdd.bottom) {
            if (d->count < MAX_SCHEDULE) {
                d->pts[d->count].minutes = 12 * 60;  /* default new point 12:00 = 50 */
                d->pts[d->count].brightness = 50;
                d->selectedRow = d->count;
                d->count++;
                SchedResize(hwnd, d);
                RenderSchedEditor(hwnd, d);
            }
            return 0;
        }
        if (x >= rcSave.left && x <= rcSave.right && y >= rcSave.top && y <= rcSave.bottom) {
            Schedule_Sort(d->pts, d->count);
            for (int i = 0; i < d->count; i++)
                d->settings->schedule[i] = d->pts[i];
            d->settings->scheduleCount = d->count;
            HWND owner = d->owner;
            DestroyWindow(hwnd);
            g_schedHwnd = NULL;
            PostMessageW(owner, WM_COMMAND, (WPARAM)IDM_SCHEDULE_SAVED, 0);
            return 0;
        }

        int field;
        int row = SchedHitField(d, x, y, &field);
        if (row >= 0) {
            d->selectedRow = row;
            if (field == SF_DEL) {
                for (int i = row; i < d->count - 1; i++)
                    d->pts[i] = d->pts[i + 1];
                d->count--;
                if (d->selectedRow >= d->count) d->selectedRow = d->count - 1;
                SchedResize(hwnd, d);
            }
            RenderSchedEditor(hwnd, d);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int dir = ((short)HIWORD(wParam) > 0) ? 1 : -1;
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        ScreenToClient(hwnd, &pt);
        int field;
        int row = SchedHitField(d, pt.x, pt.y, &field);
        if (row >= 0 && field != SF_DEL) {
            d->selectedRow = row;
            SchedAdjust(d, row, field, dir);
            RenderSchedEditor(hwnd, d);
        }
        return 0;
    }

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            /* Dismiss without saving on click-outside. */
            DestroyWindow(hwnd);
            g_schedHwnd = NULL;
        }
        return 0;

    case WM_DESTROY:
        g_schedHwnd = NULL;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void UI_ShowScheduleEditor(HWND hwndOwner, Settings *s)
{
    if (g_schedHwnd && IsWindow(g_schedHwnd)) {
        DestroyWindow(g_schedHwnd);
        g_schedHwnd = NULL;
    }

    memset(&g_sched, 0, sizeof(g_sched));
    g_sched.settings = s;
    g_sched.owner = hwndOwner;
    g_sched.count = s->scheduleCount;
    g_sched.selectedRow = (s->scheduleCount > 0) ? 0 : -1;
    for (int i = 0; i < s->scheduleCount; i++)
        g_sched.pts[i] = s->schedule[i];

    int w = SCHED_WIDTH;
    int h = SchedHeight(&g_sched);

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

    g_schedHwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        SCHED_CLASS, L"", WS_POPUP,
        x, y, w, h, NULL, NULL, g_uiInst, NULL);
    if (!g_schedHwnd) return;

    RenderSchedEditor(g_schedHwnd, &g_sched);
    ShowWindow(g_schedHwnd, SW_SHOWNOACTIVATE);
    SetForegroundWindow(g_schedHwnd);
}

/* ---- Class registration ---- */

void UiSched_Init(HINSTANCE hInst)
{
    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SchedWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = SCHED_CLASS;
    RegisterClassExW(&wc);
}

void UiSched_Shutdown(void)
{
    if (g_schedHwnd) {
        DestroyWindow(g_schedHwnd);
        g_schedHwnd = NULL;
    }
}
