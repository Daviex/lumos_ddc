#include "ui_internal.h"
#include <wctype.h>

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

/* Posted to the menu itself, so an item chosen through MSAA (inside a COM
   call) closes the menu after the call has returned. wParam = item index. */
#define WM_CTX_INVOKE (WM_APP + 1)

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

/* ---- Keyboard and screen reader ----
   Separators are drawing only: screen readers see the selectable items, which
   is why the model index and the item index are told apart below. */

static int CtxSelectableCount(CtxMenuData *d)
{
    int n = 0;
    for (int i = 0; i < d->count; i++)
        if (d->items[i].type == CTX_ITEM_NORMAL) n++;
    return n;
}

static int CtxItemFromModel(CtxMenuData *d, int index)
{
    for (int i = 0; i < d->count; i++)
        if (d->items[i].type == CTX_ITEM_NORMAL && index-- == 0)
            return i;
    return -1;
}

static int CtxModelFromItem(CtxMenuData *d, int item)
{
    if (item < 0 || item >= d->count || d->items[item].type != CTX_ITEM_NORMAL)
        return -1;
    int n = 0;
    for (int i = 0; i < item; i++)
        if (d->items[i].type == CTX_ITEM_NORMAL) n++;
    return n;
}

static void CtxItemRect(CtxMenuData *d, int item, RECT *rc)
{
    int cy = CTXMENU_PAD;
    for (int i = 0; i < item; i++)
        cy += (d->items[i].type == CTX_ITEM_SEPARATOR) ? CTXMENU_SEP_H : CTXMENU_ITEM_H;
    SetRect(rc, 4, cy, CTXMENU_WIDTH - 4, cy + CTXMENU_ITEM_H);
}

static int CtxA11yCount(void *ctx)
{
    return CtxSelectableCount((CtxMenuData *)ctx);
}

static void CtxA11yDescribe(void *ctx, int index, A11yItem *out)
{
    CtxMenuData *d = (CtxMenuData *)ctx;
    if (index < 0) {
        out->role = ROLE_SYSTEM_MENUPOPUP;
        wcscpy(out->name, APP_NAME);
        return;
    }
    int item = CtxItemFromModel(d, index);
    if (item < 0)
        return;
    out->role = ROLE_SYSTEM_MENUITEM;
    out->state = STATE_SYSTEM_FOCUSABLE;
    if (d->items[item].checked)
        out->state |= STATE_SYSTEM_CHECKED;
    CtxItemRect(d, item, &out->rect);
    wcsncpy(out->name, d->items[item].label, 159);
    wcscpy(out->action, L"Execute");
}

static int CtxA11yFocused(void *ctx)
{
    CtxMenuData *d = (CtxMenuData *)ctx;
    return CtxModelFromItem(d, d->hoverIndex);
}

static BOOL CtxA11yInvoke(void *ctx, int index)
{
    int item = CtxItemFromModel((CtxMenuData *)ctx, index);
    if (item < 0 || !g_ctxHwnd)
        return FALSE;
    PostMessageW(g_ctxHwnd, WM_CTX_INVOKE, (WPARAM)item, 0);
    return TRUE;
}

static const A11yModel g_ctxModel = {
    CtxA11yCount, CtxA11yDescribe, CtxA11yFocused, CtxA11yInvoke, &g_ctxData
};

static void CtxSetHover(HWND hwnd, CtxMenuData *d, int item)
{
    if (item == d->hoverIndex)
        return;
    d->hoverIndex = item;
    RenderContextMenu(hwnd, d);
    int index = CtxModelFromItem(d, item);
    if (index >= 0)
        A11y_NotifyFocus(hwnd, index);
}

/* Next selectable item from 'from' in direction dir (+1/-1), wrapping. */
static int CtxStep(CtxMenuData *d, int from, int dir)
{
    for (int n = 0; n < d->count; n++) {
        from = (from + dir + d->count) % d->count;
        if (d->items[from].type == CTX_ITEM_NORMAL)
            return from;
    }
    return -1;
}

static void CtxInvoke(HWND hwnd, CtxMenuData *d, int item)
{
    if (item < 0 || item >= d->count || d->items[item].type != CTX_ITEM_NORMAL)
        return;
    int id = d->items[item].id;
    HWND owner = d->hwndOwner;
    DestroyWindow(hwnd);
    g_ctxHwnd = NULL;
    PostMessageW(owner, WM_COMMAND, (WPARAM)id, 0);
}

/* Standard menu keys: Up and Down move (wrapping), Home and End jump, Enter
   and Space choose, Escape closes. A letter jumps to the next item starting
   with it, as in a native menu. */
static void CtxKeyDown(HWND hwnd, CtxMenuData *d, WPARAM vk)
{
    switch (vk) {
    case VK_DOWN:   CtxSetHover(hwnd, d, CtxStep(d, d->hoverIndex < 0 ? -1 : d->hoverIndex, 1)); return;
    case VK_UP:     CtxSetHover(hwnd, d, CtxStep(d, d->hoverIndex < 0 ? 0 : d->hoverIndex, -1)); return;
    case VK_HOME:   CtxSetHover(hwnd, d, CtxStep(d, -1, 1)); return;
    case VK_END:    CtxSetHover(hwnd, d, CtxStep(d, 0, -1)); return;
    case VK_RETURN:
    case VK_SPACE:  CtxInvoke(hwnd, d, d->hoverIndex); return;
    case VK_ESCAPE:
        DestroyWindow(hwnd);
        g_ctxHwnd = NULL;
        return;
    default:
        break;
    }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        int i = d->hoverIndex < 0 ? -1 : d->hoverIndex;
        for (int n = 0; n < d->count; n++) {
            i = CtxStep(d, i, 1);
            if (i >= 0 && (WCHAR)towupper(d->items[i].label[0]) == (WCHAR)vk) {
                CtxSetHover(hwnd, d, i);
                return;
            }
        }
    }
}

static LRESULT CALLBACK CtxMenuWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    CtxMenuData *d = &g_ctxData;

    switch (msg) {
    case WM_GETOBJECT: {
        LRESULT r;
        if (A11y_HandleGetObject(hwnd, wParam, lParam, &r))
            return r;
        break;
    }

    case WM_KEYDOWN:
        CtxKeyDown(hwnd, d, wParam);
        return 0;

    case WM_CTX_INVOKE:
        CtxInvoke(hwnd, d, (int)wParam);
        return 0;

    case WM_MOUSEMOVE: {
        int y = (short)HIWORD(lParam);
        int idx = CtxMenuHitTest(d, y);
        if (idx >= 0)
            CtxSetHover(hwnd, d, idx);
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
        CtxInvoke(hwnd, d, CtxMenuHitTest(d, y));
        return 0;
    }

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            DestroyWindow(hwnd);
            g_ctxHwnd = NULL;
        }
        return 0;

    case WM_DESTROY:
        A11y_NotifyMenuPopup(hwnd, FALSE);
        A11y_Detach(hwnd);
        g_ctxHwnd = NULL;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void UI_ShowContextMenu(HWND hwndOwner, Settings *s, const POINT *anchor, BOOL fromKeyboard)
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

    /* Position at the anchor (the tray icon) or the cursor, adjusted to stay on-screen */
    POINT pt;
    if (anchor)
        pt = *anchor;
    else
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
    A11y_Attach(g_ctxHwnd, &g_ctxModel);

    /* From the keyboard the first item starts highlighted, as in a native menu.
       A mouse user gets the highlight when the pointer moves over an item. */
    if (fromKeyboard)
        g_ctxData.hoverIndex = CtxStep(&g_ctxData, -1, 1);

    RenderContextMenu(g_ctxHwnd, &g_ctxData);
    ShowWindow(g_ctxHwnd, SW_SHOWNOACTIVATE);
    SetForegroundWindow(g_ctxHwnd);
    A11y_NotifyMenuPopup(g_ctxHwnd, TRUE);
    if (g_ctxData.hoverIndex >= 0)
        A11y_NotifyFocus(g_ctxHwnd, CtxModelFromItem(&g_ctxData, g_ctxData.hoverIndex));
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
