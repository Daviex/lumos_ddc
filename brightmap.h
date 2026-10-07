#ifndef BRIGHTMAP_H
#define BRIGHTMAP_H

/* Maps the All Monitors level onto one monitor through the monitor's range.
 *
 * Each monitor has a range [lo, hi]: its level when All Monitors is at 0% and
 * at 100%. All Monitors moves every monitor linearly across its own range, so
 * two monitors whose ranges were matched at both ends stay matched in between,
 * every master step moves every monitor, and no monitor sits at a limit while
 * the others still move. The older per-monitor offset ("master + offset",
 * clamped) kept monitors matched but left one stuck at 0 or 100 for as many
 * steps as the offsets differed.
 *
 * Win32-free and int-only, unit-tested in test_brightmap.c. */

/* The narrowest range allowed. With a 5% master step a range of 20 still moves
   the monitor by one level per step, so no step is dead. */
#define BRIGHTMAP_MIN_SPAN 20

/* The level (0-100) of a monitor with range [lo, hi] at this master level. */
int BrightMap_Level(int master, int lo, int hi);

/* The master level (0-100) that puts a monitor with range [lo, hi] at this
   level. The inverse of BrightMap_Level, up to rounding; a level outside the
   range maps to 0 or 100. */
int BrightMap_Master(int level, int lo, int hi);

/* Clamp a range into 0-100 with at least BRIGHTMAP_MIN_SPAN between its ends.
   The low end gives way first, since the high end is the one set in the popup. */
void BrightMap_Normalize(int *lo, int *hi);

/* Convert the per-monitor offsets of older versions (-40..+40) into ranges
   that keep the monitors matched as they were: every range has the same
   width, and a monitor that was N points brighter starts N points higher.
   A monitor without an offset counts as offset 0, so 0 always takes part. */
void BrightMap_FromOffsets(const int *offsets, int count, int *lo, int *hi);

#endif /* BRIGHTMAP_H */
