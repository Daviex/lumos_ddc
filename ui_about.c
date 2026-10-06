#include "ui_internal.h"
#include <shellapi.h>

static const WCHAR ABOUT_CLASS[]   = L"LumosAbout";
static HWND g_aboutHwnd = NULL;
static RECT g_aboutLinkRect = { 0, 0, 0, 0 };  /* hit rect for the repo link */
static BOOL g_aboutFocusVisible = FALSE;         /* ring on the link once a key is used */

/* ---- Screen reader model ----
   The text lines are read-only items, so NVDA reads them as the dialog text;
   the repository link is the one focusable item. */

typedef struct {
    LONG         role;
    const WCHAR *text;
    RECT         rect;
} AboutLine;

static const AboutLine kAboutLines[] = {
    { ROLE_SYSTEM_STATICTEXT, APP_NAME,                               { 0, 18, ABOUT_WIDTH, 50 } },
    { ROLE_SYSTEM_STATICTEXT, L"Monitor brightness for DDC/CI + WMI", { 0, 54, ABOUT_WIDTH, 72 } },
    { ROLE_SYSTEM_STATICTEXT, L"Version " APP_VERSION,                { 0, 84, ABOUT_WIDTH, 104 } },
    { ROLE_SYSTEM_STATICTEXT, L"by " APP_AUTHOR,                      { 0, 106, ABOUT_WIDTH, 124 } },
};
#define ABOUT_LINES ((int)(sizeof(kAboutLines) / sizeof(kAboutLines[0])))
#define ABOUT_LINK_ITEM ABOUT_LINES   /* the link follows the text lines */

static int AboutA11yCount(void *ctx) { (void)ctx; return ABOUT_LINES + 1; }
static int AboutA11yFocused(void *ctx) { (void)ctx; return ABOUT_LINK_ITEM; }

static void AboutA11yDescribe(void *ctx, int index, A11yItem *out)
{
    (void)ctx;
    if (index < 0) {
        out->role = ROLE_SYSTEM_DIALOG;
        out->state = STATE_SYSTEM_FOCUSABLE;
        wcscpy(out->name, L"About " APP_NAME);
    } else if (index < ABOUT_LINES) {
        out->role = kAboutLines[index].role;
        out->state = STATE_SYSTEM_READONLY;
        out->rect = kAboutLines[index].rect;
        wcsncpy(out->name, kAboutLines[index].text, 159);
    } else {
        out->role = ROLE_SYSTEM_LINK;
        out->state = STATE_SYSTEM_FOCUSABLE | STATE_SYSTEM_LINKED;
        out->rect = g_aboutLinkRect;
        wcscpy(out->name, APP_REPO_DISPLAY);
        wcscpy(out->value, APP_REPO_URL);
        wcscpy(out->action, L"Jump");
    }
}

static void AboutOpenLink(HWND hwnd)
{
    DestroyWindow(hwnd);
    ShellExecuteW(NULL, L"open", APP_REPO_URL, NULL, NULL, SW_SHOWNORMAL);
}

/* Posted so the link opens after an MSAA call has returned. */
#define WM_ABOUT_OPEN_LINK (WM_APP + 1)

static BOOL AboutA11yInvoke(void *ctx, int index)
{
    (void)ctx;
    if (index != ABOUT_LINK_ITEM || !g_aboutHwnd)
        return FALSE;
    PostMessageW(g_aboutHwnd, WM_ABOUT_OPEN_LINK, 0, 0);
    return TRUE;
}

static const A11yModel g_aboutModel = {
    AboutA11yCount, AboutA11yDescribe, AboutA11yFocused, AboutA11yInvoke, NULL
};

/* ---- About window ---- */

static void RenderAbout(HWND hwnd)
{
    int w = ABOUT_WIDTH, h = ABOUT_HEIGHT;

    BYTE *bits = NULL;
    HBITMAP bmp = NULL;
    HDC dc = CreateAlphaDC(w, h, &bmp, &bits);

    HBRUSH bgBrush = CreateSolidBrush(HexToColorRef(CLR_BG));
    RECT rcAll = { 0, 0, w, h };
    FillRect(dc, &rcAll, bgBrush);
    DeleteObject(bgBrush);

    SetBkMode(dc, TRANSPARENT);

    HFONT hTitle = CreateFontW(-24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hBody  = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hSmall = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hLink  = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, TRUE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    RECT rc;

    /* Title */
    SelectObject(dc, hTitle);
    SetTextColor(dc, HexToColorRef(CLR_TEXT));
    rc = (RECT){ 0, 18, w, 50 };
    DrawTextW(dc, APP_NAME, -1, &rc, DT_CENTER | DT_SINGLELINE);

    /* Subtitle */
    SelectObject(dc, hSmall);
    SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
    rc = (RECT){ 0, 54, w, 72 };
    DrawTextW(dc, L"Monitor brightness for DDC/CI + WMI", -1, &rc, DT_CENTER | DT_SINGLELINE);

    /* Version */
    SelectObject(dc, hBody);
    SetTextColor(dc, HexToColorRef(CLR_TEXT));
    rc = (RECT){ 0, 84, w, 104 };
    DrawTextW(dc, L"Version " APP_VERSION, -1, &rc, DT_CENTER | DT_SINGLELINE);

    /* Author */
    SelectObject(dc, hSmall);
    SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
    rc = (RECT){ 0, 106, w, 124 };
    DrawTextW(dc, L"by " APP_AUTHOR, -1, &rc, DT_CENTER | DT_SINGLELINE);

    /* Clickable repo link (measured so the hit rect is exact) */
    SelectObject(dc, hLink);
    SetTextColor(dc, HexToColorRef(CLR_ACCENT));
    const WCHAR *link = APP_REPO_DISPLAY;
    int linkLen = lstrlenW(link);
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32W(dc, link, linkLen, &sz);
    int linkX = (w - sz.cx) / 2;
    int linkY = 138;
    TextOutW(dc, linkX, linkY, link, linkLen);
    g_aboutLinkRect.left   = linkX - 6;
    g_aboutLinkRect.top    = linkY - 3;
    g_aboutLinkRect.right  = linkX + sz.cx + 6;
    g_aboutLinkRect.bottom = linkY + sz.cy + 3;

    /* Hint */
    SelectObject(dc, hSmall);
    SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
    rc = (RECT){ 0, 160, w, 178 };
    DrawTextW(dc, L"Enter opens the link, Esc closes", -1, &rc, DT_CENTER | DT_SINGLELINE);

    if (g_aboutFocusVisible) {
        RECT rcFocus = g_aboutLinkRect;
        InflateRect(&rcFocus, 2, 2);
        DrawFocusRing(dc, &rcFocus, 8);
    }

    /* Release fonts (deselect first so none is active) */
    SelectObject(dc, (HFONT)GetStockObject(SYSTEM_FONT));
    DeleteObject(hTitle);
    DeleteObject(hBody);
    DeleteObject(hSmall);
    DeleteObject(hLink);

    ApplyRoundedMask(bits, w, h, ABOUT_CORNER, 245);
    CommitLayered(hwnd, dc, w, h);

    DeleteObject(bmp);
    DeleteDC(dc);
}

static LRESULT CALLBACK AboutWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_LBUTTONUP: {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        BOOL onLink = PtInRect(&g_aboutLinkRect, pt);
        DestroyWindow(hwnd);
        if (onLink)
            ShellExecuteW(NULL, L"open", APP_REPO_URL, NULL, NULL, SW_SHOWNORMAL);
        return 0;
    }

    case WM_SETCURSOR: {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        SetCursor(LoadCursor(NULL, PtInRect(&g_aboutLinkRect, pt) ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }

    case WM_GETOBJECT: {
        LRESULT r;
        if (A11y_HandleGetObject(hwnd, wParam, lParam, &r))
            return r;
        break;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            DestroyWindow(hwnd);
        } else if (wParam == VK_RETURN || wParam == VK_SPACE) {
            AboutOpenLink(hwnd);
        } else if (!g_aboutFocusVisible) {
            g_aboutFocusVisible = TRUE;   /* Tab or any other key shows where focus is */
            RenderAbout(hwnd);
        }
        return 0;

    case WM_ABOUT_OPEN_LINK:
        AboutOpenLink(hwnd);
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE)
            DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        A11y_Detach(hwnd);
        g_aboutHwnd = NULL;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void UI_ShowAbout(HWND hwndOwner)
{
    if (g_aboutHwnd && IsWindow(g_aboutHwnd)) {
        DestroyWindow(g_aboutHwnd);
        g_aboutHwnd = NULL;
    }

    int w = ABOUT_WIDTH, h = ABOUT_HEIGHT;

    POINT pt;
    GetCursorPos(&pt);
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hMon, &mi);

    int x = pt.x - w / 2;
    int y = pt.y - h / 2;
    if (x + w > mi.rcWork.right)  x = mi.rcWork.right - w;
    if (x < mi.rcWork.left)       x = mi.rcWork.left;
    if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;
    if (y < mi.rcWork.top)        y = mi.rcWork.top;

    g_aboutHwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        ABOUT_CLASS, L"", WS_POPUP,
        x, y, w, h, hwndOwner, NULL, g_uiInst, NULL);
    if (!g_aboutHwnd) return;
    A11y_Attach(g_aboutHwnd, &g_aboutModel);
    g_aboutFocusVisible = FALSE;

    RenderAbout(g_aboutHwnd);
    ShowWindow(g_aboutHwnd, SW_SHOWNOACTIVATE);
    SetForegroundWindow(g_aboutHwnd);
    A11y_NotifyFocus(g_aboutHwnd, ABOUT_LINK_ITEM);
}


/* ---- Class registration ---- */

void UiAbout_Init(HINSTANCE hInst)
{
    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = AboutWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = ABOUT_CLASS;
    RegisterClassExW(&wc);
}
