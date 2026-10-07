#include "brightmap.h"

static int Clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* The level of the middle point of the curve. The offset bound keeps it inside
   10..90, so neither segment is flat and the inverse never divides by zero. */
static int MidLevel(int offset)
{
    return 50 + Clamp(offset, -BRIGHTMAP_OFFSET_MAX, BRIGHTMAP_OFFSET_MAX);
}

/* a * b / c rounded to the nearest integer, for non-negative a, b and positive c. */
static int MulDivRound(int a, int b, int c)
{
    return (a * b + c / 2) / c;
}

int BrightMap_Level(int master, int offset)
{
    int m = Clamp(master, 0, 100);
    int mid = MidLevel(offset);
    if (m <= 50)
        return MulDivRound(m, mid, 50);
    return mid + MulDivRound(m - 50, 100 - mid, 50);
}

int BrightMap_Master(int level, int offset)
{
    int l = Clamp(level, 0, 100);
    int mid = MidLevel(offset);
    if (l <= mid)
        return MulDivRound(l, 50, mid);
    return 50 + MulDivRound(l - mid, 50, 100 - mid);
}
