#ifndef CLOCK_GETTIME_H
#define CLOCK_GETTIME_H

#include <time.h>
#include <sys/time.h>

#ifdef __MACH__
#include <mach/clock.h>
#include <mach/mach.h>
#endif

#ifdef __MACH__
/*
 * Mac OS X does not support clock_gettime
 * ref: http://stackoverflow.com/questions/11680461/monotonic-clock-on-osx
 */
void clock_gettime_mach(struct timespec*);
#define CLOCK_GETTIME(now) clock_gettime_mach(now);
#else
#define CLOCK_GETTIME(now) clock_gettime(CLOCK_MONOTONIC, now);
#endif

#endif /* CLOCK_GETTIME_H */
