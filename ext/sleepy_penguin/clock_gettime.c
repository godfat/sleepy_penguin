#ifdef __MACH__
/*
 * Mac OS X does not support clock_gettime
 * ref: http://stackoverflow.com/questions/11680461/monotonic-clock-on-osx
 */
#include "clock_gettime.h"

void clock_gettime_mach(struct timespec *now){
	clock_serv_t cclock;
	mach_timespec_t mts;
	host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
	clock_get_time(cclock, &mts);
	mach_port_deallocate(mach_task_self(), cclock);
	now->tv_sec = mts.tv_sec;
	now->tv_nsec = mts.tv_nsec;
}
#endif
