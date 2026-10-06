#include "ui_internal.h"
#include <math.h>

/* ---- Color helpers ---- */

COLORREF HexToColorRef(DWORD rgb)
{
    return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

/* ---- 32-bit DIB helpers for layered windows ---- */

HDC CreateAlphaDC(int w, int h, HBITMAP *outBmp, BYTE **outBits)
{
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;  /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC hdc = CreateCompatibleDC(NULL);
    *outBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, (void **)outBits, NULL, 0);
    SelectObject(hdc, *outBmp);
    return hdc;
}

/* Apply rounded corner mask with anti-aliasing and premultiply alpha */
void ApplyRoundedMask(BYTE *bits, int w, int h, int radius, BYTE baseAlpha)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            BYTE *px = bits + (y * w + x) * 4;
            BYTE alpha = baseAlpha;

            BOOL inCorner = FALSE;
            int cx = 0, cy = 0;
            if      (x < radius && y < radius)              { cx = radius;     cy = radius;     inCorner = TRUE; }
            else if (x >= w - radius && y < radius)          { cx = w - radius; cy = radius;     inCorner = TRUE; }
            else if (x < radius && y >= h - radius)          { cx = radius;     cy = h - radius; inCorner = TRUE; }
            else if (x >= w - radius && y >= h - radius)     { cx = w - radius; cy = h - radius; inCorner = TRUE; }

            if (inCorner) {
                float dx = (float)x - (float)cx + 0.5f;
                float dy = (float)y - (float)cy + 0.5f;
                float dist = sqrtf(dx * dx + dy * dy);
                float r = (float)radius;
                if (dist > r + 0.5f)
                    alpha = 0;
                else if (dist > r - 0.5f)
                    alpha = (BYTE)((r + 0.5f - dist) * baseAlpha);
            }

            /* Premultiply RGB by alpha */
            px[0] = (BYTE)((px[0] * alpha + 127) / 255);
            px[1] = (BYTE)((px[1] * alpha + 127) / 255);
            px[2] = (BYTE)((px[2] * alpha + 127) / 255);
            px[3] = alpha;
        }
    }
}

/* Call UpdateLayeredWindow with a 32-bit surface */
void CommitLayered(HWND hwnd, HDC memDC, int w, int h)
{
    POINT ptSrc = { 0, 0 };
    SIZE sz = { w, h };
    BLENDFUNCTION bf;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(hwnd, NULL, NULL, &sz, memDC, &ptSrc, 0, &bf, ULW_ALPHA);
}

/* Keyboard focus indicator: a 2px accent outline. Every window draws focus the
   same way, so a low-vision user can follow it at any magnification. */
void DrawFocusRing(HDC dc, const RECT *rc, int radius)
{
    HPEN pen = CreatePen(PS_SOLID, 2, HexToColorRef(CLR_ACCENT));
    HPEN oldPen = (HPEN)SelectObject(dc, pen);
    HBRUSH oldBr = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, rc->left, rc->top, rc->right, rc->bottom, radius, radius);
    SelectObject(dc, oldBr);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

/* Start moving a caption-less window with the mouse, as if its title bar had
   been pressed. Called from WM_LBUTTONDOWN on a spot that is not a control;
   returns once the user lets go. */
void BeginWindowDrag(HWND hwnd)
{
    ReleaseCapture();
    SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
}

/* Footer buttons shared by the dialogs. Settings and the schedule editor both
   call these, so the buttons keep the same size, place and look. */
void DialogButtonRects(int w, int y, RECT *rcCancel, RECT *rcSave)
{
    SetRect(rcSave,   w - DLG_MARGIN - DLG_BTN_W, y, w - DLG_MARGIN, y + DLG_BTN_H);
    SetRect(rcCancel, rcSave->left - DLG_BTN_GAP - DLG_BTN_W, y,
                      rcSave->left - DLG_BTN_GAP, y + DLG_BTN_H);
}

/* primary = the accent button (Save); the others use the surface color. */
void DrawDialogButton(HDC dc, const RECT *rc, const WCHAR *label, BOOL primary, HFONT font)
{
    HBRUSH fill = CreateSolidBrush(HexToColorRef(primary ? CLR_ACCENT : CLR_SURFACE));
    HPEN oldPen = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
    HBRUSH oldBr = (HBRUSH)SelectObject(dc, fill);
    RoundRect(dc, rc->left, rc->top, rc->right, rc->bottom, 8, 8);
    SelectObject(dc, oldBr);
    SelectObject(dc, oldPen);
    DeleteObject(fill);

    HFONT oldFont = (HFONT)SelectObject(dc, font);
    SetTextColor(dc, HexToColorRef(primary ? CLR_BG : CLR_TEXT));
    RECT r = *rc;
    DrawTextW(dc, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);
}

/* Whole wheel notches in a WM_MOUSEWHEEL, keeping the remainder in *accum for
   the next message: a touchpad or a high-resolution wheel sends fractions of
   WHEEL_DELTA, and one step per message made a single notch count several times. */
int WheelNotches(int *accum, WPARAM wParam)
{
    *accum += (short)HIWORD(wParam);
    int n = *accum / WHEEL_DELTA;
    *accum -= n * WHEEL_DELTA;
    return n;
}
