#include "brightmap.h"

static int Clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* a * b / c rounded to the nearest integer, for non-negative a, b and positive c. */
static int MulDivRound(int a, int b, int c)
{
    return (a * b + c / 2) / c;
}

void BrightMap_Normalize(int *lo, int *hi)
{
    *hi = Clamp(*hi, BRIGHTMAP_MIN_SPAN, 100);
    *lo = Clamp(*lo, 0, *hi - BRIGHTMAP_MIN_SPAN);
}

int BrightMap_Level(int master, int lo, int hi)
{
    BrightMap_Normalize(&lo, &hi);
    return lo + MulDivRound(Clamp(master, 0, 100), hi - lo, 100);
}

int BrightMap_Master(int level, int lo, int hi)
{
    BrightMap_Normalize(&lo, &hi);
    return MulDivRound(Clamp(level, lo, hi) - lo, 100, hi - lo);
}

void BrightMap_FromOffsets(const int *offsets, int count, int *lo, int *hi)
{
    int minOff = 0, maxOff = 0;
    for (int i = 0; i < count; i++) {
        int d = Clamp(offsets[i], -40, 40);
        if (d < minOff) minOff = d;
        if (d > maxOff) maxOff = d;
    }
    /* Offsets span at most 80 points, so the width never drops below 20. */
    int span = 100 - (maxOff - minOff);
    for (int i = 0; i < count; i++) {
        lo[i] = Clamp(offsets[i], -40, 40) - minOff;
        hi[i] = lo[i] + span;
    }
}
