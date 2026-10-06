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
