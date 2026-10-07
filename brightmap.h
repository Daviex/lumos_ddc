#ifndef BRIGHTMAP_H
#define BRIGHTMAP_H

/* Maps the All Monitors level onto one monitor through its offset.
 *
 * The curve is piecewise linear through (0, 0), (50, 50 + offset) and
 * (100, 100). The offset applies in full at the middle and shrinks towards the
 * ends, so every monitor reaches 0 and 100 at the same master level and every
 * master step moves every monitor. A plain "master + offset" with clamping
 * instead leaves one monitor stuck at 0 or 100 for as many steps as the
 * offsets differ.
 *
 * Win32-free and int-only, unit-tested in test_brightmap.c. */

#define BRIGHTMAP_OFFSET_MAX 40   /* offsets are clamped to -40..+40 */

/* The level (0-100) of a monitor with this offset at this master level. */
int BrightMap_Level(int master, int offset);

/* The master level (0-100) that puts a monitor with this offset at this
   level. The inverse of BrightMap_Level, up to rounding. */
int BrightMap_Master(int level, int offset);

#endif /* BRIGHTMAP_H */
