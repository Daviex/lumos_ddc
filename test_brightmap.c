#include "brightmap.h"
#include <stdio.h>
#include <stdlib.h>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } } while (0)

int main(void)
{
    char msg[128];

    /* The example from the design: laptop +10, Dell -30 */
    CHECK(BrightMap_Level(100, 10) == 100 && BrightMap_Level(100, -30) == 100, "both reach 100");
    CHECK(BrightMap_Level(95, 10) == 96 && BrightMap_Level(95, -30) == 92, "first step down moves both");
    CHECK(BrightMap_Level(90, 10) == 92 && BrightMap_Level(90, -30) == 84, "master 90");
    CHECK(BrightMap_Level(50, 10) == 60 && BrightMap_Level(50, -30) == 20, "full offset at 50");
    CHECK(BrightMap_Level(0, 10) == 0 && BrightMap_Level(0, -30) == 0, "both reach 0");

    /* No offset is the identity in both directions */
    for (int m = 0; m <= 100; m++) {
        snprintf(msg, sizeof(msg), "identity at %d", m);
        CHECK(BrightMap_Level(m, 0) == m && BrightMap_Master(m, 0) == m, msg);
    }

    /* Out-of-range input is clamped */
    CHECK(BrightMap_Level(130, -30) == 100 && BrightMap_Level(-10, 10) == 0, "master clamped");
    CHECK(BrightMap_Level(50, 90) == 90 && BrightMap_Level(50, -90) == 10, "offset clamped to 40");
    CHECK(BrightMap_Master(120, 10) == 100 && BrightMap_Master(-5, 10) == 0, "level clamped");

    for (int off = -BRIGHTMAP_OFFSET_MAX; off <= BRIGHTMAP_OFFSET_MAX; off++) {
        /* Ends are fixed, the curve never goes down, and it rises on every
           master step of 5 (the default step), so no step is dead. */
        snprintf(msg, sizeof(msg), "ends fixed, offset %d", off);
        CHECK(BrightMap_Level(0, off) == 0 && BrightMap_Level(100, off) == 100, msg);
        for (int m = 1; m <= 100; m++) {
            snprintf(msg, sizeof(msg), "monotonic, offset %d master %d", off, m);
            CHECK(BrightMap_Level(m, off) >= BrightMap_Level(m - 1, off), msg);
        }
        for (int m = 5; m <= 100; m += 5) {
            snprintf(msg, sizeof(msg), "step of 5 moves, offset %d master %d", off, m);
            CHECK(BrightMap_Level(m, off) > BrightMap_Level(m - 5, off), msg);
        }

        /* The inverse lands on a master level that maps back to the same
           monitor level, give or take one for rounding. */
        for (int l = 0; l <= 100; l++) {
            int back = BrightMap_Level(BrightMap_Master(l, off), off);
            snprintf(msg, sizeof(msg), "inverse, offset %d level %d -> %d", off, l, back);
            CHECK(abs(back - l) <= 1, msg);
        }
        for (int m = 0; m <= 100; m++) {
            int back = BrightMap_Master(BrightMap_Level(m, off), off);
            /* A flat segment (slope 1/5 at offset -40) maps up to 3 master
               levels onto one monitor level, so the round trip can be off by 2. */
            snprintf(msg, sizeof(msg), "round trip, offset %d master %d -> %d", off, m, back);
            CHECK(abs(back - m) <= 2, msg);
        }
    }

    if (failures == 0)
        printf("All brightmap tests passed.\n");
    return failures ? 1 : 0;
}
