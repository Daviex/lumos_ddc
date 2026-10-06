#include "ui_internal.h"
#include <shellapi.h>

static const WCHAR ABOUT_CLASS[]   = L"LumosAbout";
static HWND g_aboutHwnd = NULL;
static RECT g_aboutLinkRect = { 0, 0, 0, 0 };  /* hit rect for the repo link */

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
    DrawTextW(dc, L"Esc to close", -1, &rc, DT_CENTER | DT_SINGLELINE);

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

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
            DestroyWindow(hwnd);
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE)
            DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
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

    RenderAbout(g_aboutHwnd);
    ShowWindow(g_aboutHwnd, SW_SHOWNOACTIVATE);
    SetForegroundWindow(g_aboutHwnd);
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
