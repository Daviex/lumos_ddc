#ifndef UI_GRAPHICS_H
#define UI_GRAPHICS_H

#include <windows.h>

/* Shared GDI helpers for Lumos's layered windows. */
COLORREF UI_ColorRef(DWORD rgb);
HDC UI_CreateAlphaDC(int width, int height, HBITMAP *bitmap, BYTE **bits);
void UI_ApplyRoundedMask(BYTE *bits, int width, int height, int radius, BYTE alpha);
BOOL UI_CommitLayered(HWND hwnd, HDC source, int width, int height);

#endif /* UI_GRAPHICS_H */
