/*
 * build.h — what kind of firmware this is.
 *
 * A debug build is any with measurement compiled in: serial statistics
 * (VEETEE_SERIAL_STATS), the render profile (VEETEE_RENDER_PROFILE) or
 * the render experiments (VEETEE_RENDER_EXPERIMENTS), or none of those
 * (VEETEE_DEBUG, env:pizero-debug); see the env:pizero-* environments in
 * platformio.ini. Debug builds show what they measure by default: the
 * profiler bar comes up on (whatever was saved), and the boot banner
 * lists what's compiled in. Every debug build has the automation HID
 * (include/automation_hid.h), which also carries their debug text, so
 * the line port is the terminal's alone.
 */
#ifndef BUILD_H
#define BUILD_H

#if defined(VEETEE_SERIAL_STATS) || defined(VEETEE_RENDER_PROFILE) || \
    defined(VEETEE_RENDER_EXPERIMENTS) || defined(VEETEE_DEBUG)
#define VEETEE_DEBUG_BUILD	1
#else
#define VEETEE_DEBUG_BUILD	0
#endif

/* "stats, profile, experiments" -- what this build measures, or "". */
#ifdef VEETEE_SERIAL_STATS
#define VEETEE_DEBUG_STATS	" serial-stats"
#else
#define VEETEE_DEBUG_STATS	""
#endif
#ifdef VEETEE_RENDER_PROFILE
#define VEETEE_DEBUG_PROFILE	" render-profile"
#else
#define VEETEE_DEBUG_PROFILE	""
#endif
#ifdef VEETEE_RENDER_EXPERIMENTS
#define VEETEE_DEBUG_EXPERIMENTS " render-experiments"
#else
#define VEETEE_DEBUG_EXPERIMENTS ""
#endif
#define VEETEE_DEBUG_FEATURES	" automation" VEETEE_DEBUG_STATS \
	VEETEE_DEBUG_PROFILE VEETEE_DEBUG_EXPERIMENTS

#endif /* BUILD_H */
