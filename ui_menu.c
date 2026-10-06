#include "ui_internal.h"

static const WCHAR CTXMENU_CLASS[] = L"LumosCtxMenu";
static HWND g_ctxHwnd = NULL;

/* ---- Context Menu ---- */

#define CTX_ITEM_NORMAL    0
#define CTX_ITEM_SEPARATOR 1

typedef struct {
    int   type;
    int   id;
    WCHAR label[80];
    BOOL  checked;
} CtxMenuItem;

#define MAX_CTX_ITEMS 20

typedef struct {
    CtxMenuItem items[MAX_CTX_ITEMS];
    int count;
    int hoverIndex;
    HWND hwndOwner;
} CtxMenuData;

static CtxMenuData g_ctxData;

static void BuildContextMenu(CtxMenuData *d, Settings *s)
{
    d->count = 0;
    d->hoverIndex = -1;

    /* Presets as flat items */
    for (int i = 0; i < s->presetCount && d->count < MAX_CTX_ITEMS; i++) {
        CtxMenuItem *it = &d->items[d->count++];
        it->type = CTX_ITEM_NORMAL;
        it->id = IDM_PRESET_BASE + i;
        wsprintfW(it->label, L"%s (%u%%)", s->presets[i].name, s->presets[i].brightness);
        it->checked = FALSE;
    }

    /* Separator */
    if (d->count < MAX_CTX_ITEMS) {
        CtxMenuItem *it = &d->items[d->count++];
        it->type = CTX_ITEM_SEPARATOR;
        it->id = 0;
        it->label[0] = 0;
        it->checked = FALSE;
    }

    /* Re-scan */
    if (d->count < MAX_CTX_ITEMS) {
        CtxMenuItem *it = &d->items[d->count++];
        it->type = CTX_ITEM_NORMAL;
        it->id = IDM_RESCAN;
        wcscpy(it->label, L"Re-scan Monitors");
        it->checked = FALSE;
    }

    /* Settings */
    if (d->count < MAX_CTX_ITEMS) {
        CtxMenuItem *it = &d->items[d->count++];
        it->type = CTX_ITEM_NORMAL;
        it->id = IDM_SETTINGS;
        wcscpy(it->label, L"Settings...");
        it->checked = FALSE;
    }

    /* Autostart */
    if (d->count < MAX_CTX_ITEMS) {
        CtxMenuItem *it = &d->items[d->count++];
        it->type = CTX_ITEM_NORMAL;
        it->id = IDM_AUTOSTART;
        wcscpy(it->label, L"Start with Windows");
        it->checked = Settings_GetAutostart();
    }

    /* Schedule toggle */
    if (d->count < MAX_CTX_ITEMS) {
        CtxMenuItem *it = &d->items[d->count++];
        it->type = CTX_ITEM_NORMAL;
        it->id = IDM_SCHEDULE_TOGGLE;
        wcscpy(it->label, L"Brightness Schedule");
        it->checked = s->scheduleEnabled;
    }

    /* Edit schedule */
    if (d->count < MAX_CTX_ITEMS) {
        CtxMenuItem *it = &d->items[d->count++];
        it->type = CTX_ITEM_NORMAL;
        it->id = IDM_SCHEDULE_EDIT;
        wcscpy(it->label, L"Edit Schedule...");
        it->checked = FALSE;
    }

    /* Idle auto-dim toggle. The level and the timeout live in config.ini. */
    if (d->count < MAX_CTX_ITEMS) {
        CtxMenuItem *it = &d->items[d->count++];
        it->type = CTX_ITEM_NORMAL;
        it->id = IDM_IDLEDIM_TOGGLE;
        wsprintfW(it->label, L"Dim When Idle (%d%%/%dm)",
                  s->idleDimPercent, s->idleDimMinutes);
        it->checked = s->idleDimEnabled;
    }

    /* Separator */
    if (d->count < MAX_CTX_ITEMS) {
        CtxMenuItem *it = &d->items[d->count++];
        it->type = CTX_ITEM_SEPARATOR;
        it->id = 0;
        it->label[0] = 0;
        it->checked = FALSE;
    }

    /* About */
    if (d->count < MAX_CTX_ITEMS) {
        CtxMenuItem *it = &d->items[d->count++];
        it->type = CTX_ITEM_NORMAL;
        it->id = IDM_ABOUT;
        wcscpy(it->label, L"About " APP_NAME);
        it->checked = FALSE;
    }

    /* Exit */
    if (d->count < MAX_CTX_ITEMS) {
        CtxMenuItem *it = &d->items[d->count++];
        it->type = CTX_ITEM_NORMAL;
        it->id = IDM_EXIT;
        wcscpy(it->label, L"Exit");
        it->checked = FALSE;
    }
}

static int GetCtxMenuHeight(CtxMenuData *d)
{
    int h = CTXMENU_PAD * 2;
    for (int i = 0; i < d->count; i++)
        h += (d->items[i].type == CTX_ITEM_SEPARATOR) ? CTXMENU_SEP_H : CTXMENU_ITEM_H;
    return h;
}

static int CtxMenuHitTest(CtxMenuData *d, int y)
{
    int cy = CTXMENU_PAD;
    for (int i = 0; i < d->count; i++) {
        int ih = (d->items[i].type == CTX_ITEM_SEPARATOR) ? CTXMENU_SEP_H : CTXMENU_ITEM_H;
        if (y >= cy && y < cy + ih) {
            return (d->items[i].type == CTX_ITEM_SEPARATOR) ? -1 : i;
        }
        cy += ih;
    }
    return -1;
}

static void RenderContextMenu(HWND hwnd, CtxMenuData *d)
{
    int w = CTXMENU_WIDTH;
    int h = GetCtxMenuHeight(d);

    BYTE *bits = NULL;
    HBITMAP bmp = NULL;
    HDC dc = CreateAlphaDC(w, h, &bmp, &bits);

    /* Background */
    HBRUSH bgBrush = CreateSolidBrush(HexToColorRef(CLR_BG));
    RECT rcAll = { 0, 0, w, h };
    FillRect(dc, &rcAll, bgBrush);
    DeleteObject(bgBrush);

    SetBkMode(dc, TRANSPARENT);
    HFONT hFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hFontCheck = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT oldFont = (HFONT)SelectObject(dc, hFont);

    int cy = CTXMENU_PAD;
    for (int i = 0; i < d->count; i++) {
        CtxMenuItem *it = &d->items[i];

        if (it->type == CTX_ITEM_SEPARATOR) {
            /* Horizontal line */
            int lineY = cy + CTXMENU_SEP_H / 2;
            HBRUSH sepBrush = CreateSolidBrush(HexToColorRef(CLR_TRACK));
            RECT rcSep = { 12, lineY, w - 12, lineY + 1 };
            FillRect(dc, &rcSep, sepBrush);
            DeleteObject(sepBrush);
            cy += CTXMENU_SEP_H;
            continue;
        }

        /* Hover highlight */
        if (i == d->hoverIndex) {
            HBRUSH hoverBrush = CreateSolidBrush(HexToColorRef(CLR_SURFACE));
            HPEN noPen = CreatePen(PS_NULL, 0, 0);
            HPEN oldPen = (HPEN)SelectObject(dc, noPen);
            HBRUSH oldBr = (HBRUSH)SelectObject(dc, hoverBrush);
            RoundRect(dc, 4, cy + 2, w - 4, cy + CTXMENU_ITEM_H - 2, 8, 8);
            SelectObject(dc, oldBr);
            SelectObject(dc, oldPen);
            DeleteObject(noPen);
            DeleteObject(hoverBrush);
        }

        int textX = 14;

        /* Checkmark */
        if (it->checked) {
            SelectObject(dc, hFontCheck);
            SetTextColor(dc, HexToColorRef(CLR_ACCENT));
            RECT rcCheck = { textX - 2, cy, textX + 14, cy + CTXMENU_ITEM_H };
            DrawTextW(dc, L"\x2713", 1, &rcCheck, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            textX += 18;
        }

        /* Label */
        SelectObject(dc, hFont);
        SetTextColor(dc, HexToColorRef(CLR_TEXT));
        RECT rcLabel = { textX, cy, w - 12, cy + CTXMENU_ITEM_H };
        DrawTextW(dc, it->label, -1, &rcLabel,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        cy += CTXMENU_ITEM_H;
    }

    SelectObject(dc, oldFont);
    DeleteObject(hFont);
    DeleteObject(hFontCheck);

    ApplyRoundedMask(bits, w, h, CTXMENU_CORNER, 245);
    CommitLayered(hwnd, dc, w, h);

    DeleteObject(bmp);
    DeleteDC(dc);
}

static LRESULT CALLBACK CtxMenuWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    CtxMenuData *d = &g_ctxData;

    switch (msg) {
    case WM_MOUSEMOVE: {
        int y = (short)HIWORD(lParam);
        int idx = CtxMenuHitTest(d, y);
        if (idx != d->hoverIndex) {
            d->hoverIndex = idx;
            RenderContextMenu(hwnd, d);
        }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
        if (d->hoverIndex != -1) {
            d->hoverIndex = -1;
            RenderContextMenu(hwnd, d);
        }
        return 0;

    case WM_LBUTTONUP: {
        int y = (short)HIWORD(lParam);
        int idx = CtxMenuHitTest(d, y);
        if (idx >= 0 && idx < d->count && d->items[idx].type == CTX_ITEM_NORMAL) {
            int id = d->items[idx].id;
            HWND owner = d->hwndOwner;
            DestroyWindow(hwnd);
            g_ctxHwnd = NULL;
            PostMessageW(owner, WM_COMMAND, (WPARAM)id, 0);
        }
        return 0;
    }

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            DestroyWindow(hwnd);
            g_ctxHwnd = NULL;
        }
        return 0;

    case WM_DESTROY:
        g_ctxHwnd = NULL;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void UI_ShowContextMenu(HWND hwndOwner, Settings *s)
{
    /* Destroy previous if still open */
    if (g_ctxHwnd && IsWindow(g_ctxHwnd)) {
        DestroyWindow(g_ctxHwnd);
        g_ctxHwnd = NULL;
    }

    BuildContextMenu(&g_ctxData, s);
    g_ctxData.hwndOwner = hwndOwner;

    int w = CTXMENU_WIDTH;
    int h = GetCtxMenuHeight(&g_ctxData);

    /* Position at cursor, adjusted to stay on-screen */
    POINT pt;
    GetCursorPos(&pt);

    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hMon, &mi);

    int x = pt.x;
    int y = pt.y - h;  /* prefer above cursor (tray is usually at bottom) */

    /* Adjust if off-screen */
    if (y < mi.rcWork.top)
        y = pt.y;  /* flip below cursor */
    if (x + w > mi.rcWork.right)
        x = mi.rcWork.right - w;
    if (x < mi.rcWork.left)
        x = mi.rcWork.left;
    if (y + h > mi.rcWork.bottom)
        y = mi.rcWork.bottom - h;

    g_ctxHwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        CTXMENU_CLASS, L"",
        WS_POPUP,
        x, y, w, h,
        NULL, NULL, g_uiInst, NULL);

    if (!g_ctxHwnd) return;

    RenderContextMenu(g_ctxHwnd, &g_ctxData);
    ShowWindow(g_ctxHwnd, SW_SHOWNOACTIVATE);
    SetForegroundWindow(g_ctxHwnd);
}

/* ---- Class registration ---- */

void UiMenu_Init(HINSTANCE hInst)
{
    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = CtxMenuWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = CTXMENU_CLASS;
    RegisterClassExW(&wc);
}

void UiMenu_Shutdown(void)
{
    if (g_ctxHwnd) {
        DestroyWindow(g_ctxHwnd);
        g_ctxHwnd = NULL;
    }
}
