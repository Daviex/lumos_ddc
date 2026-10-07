#include "brightmap.h"
#include <stdio.h>
#include <stdlib.h>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } } while (0)

int main(void)
{
    char msg[128];

    /* The example from the design: laptop 40-100, Dell 0-60 */
    CHECK(BrightMap_Level(100, 40, 100) == 100 && BrightMap_Level(100, 0, 60) == 60, "top");
    CHECK(BrightMap_Level(75, 40, 100) == 85 && BrightMap_Level(75, 0, 60) == 45, "75");
    CHECK(BrightMap_Level(50, 40, 100) == 70 && BrightMap_Level(50, 0, 60) == 30, "50");
    CHECK(BrightMap_Level(0, 40, 100) == 40 && BrightMap_Level(0, 0, 60) == 0, "bottom");

    /* The default range is the identity in both directions */
    for (int m = 0; m <= 100; m++) {
        snprintf(msg, sizeof(msg), "identity at %d", m);
        CHECK(BrightMap_Level(m, 0, 100) == m && BrightMap_Master(m, 0, 100) == m, msg);
    }

    /* Out-of-range input is clamped */
    CHECK(BrightMap_Level(130, 0, 60) == 60 && BrightMap_Level(-10, 40, 100) == 40, "master clamped");
    CHECK(BrightMap_Master(80, 0, 60) == 100 && BrightMap_Master(10, 40, 100) == 0, "level outside the range");

    /* Normalize keeps the range inside 0-100 and at least the minimum width */
    int lo, hi;
    lo = 70; hi = 80;  BrightMap_Normalize(&lo, &hi);
    CHECK(lo == 60 && hi == 80, "narrow range: low end gives way");
    lo = 0; hi = 5;    BrightMap_Normalize(&lo, &hi);
    CHECK(lo == 0 && hi == BRIGHTMAP_MIN_SPAN, "high end below the minimum width");
    lo = -10; hi = 140; BrightMap_Normalize(&lo, &hi);
    CHECK(lo == 0 && hi == 100, "range clamped to 0-100");

    /* Old offsets: laptop +10 and Dell -30 become 40-100 and 0-60 */
    int offs[3] = { 10, -30, 0 }, los[3], his[3];
    BrightMap_FromOffsets(offs, 3, los, his);
    CHECK(los[0] == 40 && his[0] == 100, "laptop from +10");
    CHECK(los[1] == 0 && his[1] == 60, "Dell from -30");
    CHECK(los[2] == 30 && his[2] == 90, "offset 0 in between");
    /* The relation between the monitors is kept at every master level */
    for (int m = 0; m <= 100; m += 5) {
        snprintf(msg, sizeof(msg), "matched like the offsets at %d", m);
        CHECK(BrightMap_Level(m, los[0], his[0]) - BrightMap_Level(m, los[1], his[1]) == 40, msg);
    }
    /* A lone negative offset still counts against the implicit 0 */
    int one = -30;
    BrightMap_FromOffsets(&one, 1, los, his);
    CHECK(los[0] == 0 && his[0] == 70, "lone offset -30");
    /* The widest spread still leaves the minimum width */
    int wide[2] = { 40, -40 };
    BrightMap_FromOffsets(wide, 2, los, his);
    CHECK(his[0] - los[0] == BRIGHTMAP_MIN_SPAN && his[1] == BRIGHTMAP_MIN_SPAN, "widest spread");

    for (lo = 0; lo <= 100 - BRIGHTMAP_MIN_SPAN; lo += 5) {
        for (hi = lo + BRIGHTMAP_MIN_SPAN; hi <= 100; hi += 5) {
            /* Ends are fixed, every master step of 5 moves the monitor, and
               the curve never goes down. */
            snprintf(msg, sizeof(msg), "ends, range %d-%d", lo, hi);
            CHECK(BrightMap_Level(0, lo, hi) == lo && BrightMap_Level(100, lo, hi) == hi, msg);
            for (int m = 5; m <= 100; m += 5) {
                snprintf(msg, sizeof(msg), "step of 5 moves, range %d-%d master %d", lo, hi, m);
                CHECK(BrightMap_Level(m, lo, hi) > BrightMap_Level(m - 5, lo, hi), msg);
            }
            for (int m = 1; m <= 100; m++) {
                snprintf(msg, sizeof(msg), "monotonic, range %d-%d master %d", lo, hi, m);
                CHECK(BrightMap_Level(m, lo, hi) >= BrightMap_Level(m - 1, lo, hi), msg);
            }
            /* Reading a level back gives a master level that maps to it again,
               and a master level survives the round trip within rounding (a
               range of 20 maps 5 master levels onto one monitor level). */
            for (int l = lo; l <= hi; l++) {
                int back = BrightMap_Level(BrightMap_Master(l, lo, hi), lo, hi);
                snprintf(msg, sizeof(msg), "inverse, range %d-%d level %d -> %d", lo, hi, l, back);
                CHECK(back == l, msg);
            }
            for (int m = 0; m <= 100; m++) {
                int back = BrightMap_Master(BrightMap_Level(m, lo, hi), lo, hi);
                snprintf(msg, sizeof(msg), "round trip, range %d-%d master %d -> %d", lo, hi, m, back);
                CHECK(abs(back - m) <= 3, msg);
            }
        }
    }

    if (failures == 0)
        printf("All brightmap tests passed.\n");
    return failures ? 1 : 0;
}
