#define _GNU_SOURCE
#include <ruby.h>
#include <unistd.h>
#include <sys/types.h>
#include "git_version.h"
#define L1_CACHE_LINE_MAX 128 /* largest I've seen (Pentium 4) */
size_t rb_sp_l1_cache_line_size;

#ifdef HAVE_SYS_EVENT_H
void sleepy_penguin_init_kqueue(void);
#else
#  define sleepy_penguin_init_kqueue() for(;0;)
#endif

#ifdef HAVE_SYS_EPOLL_H
void sleepy_penguin_init_epoll(void);
#else
#  define sleepy_penguin_init_epoll() for(;0;)
#endif

#ifdef HAVE_SYS_TIMERFD_H
void sleepy_penguin_init_timerfd(void);
#else
#  define sleepy_penguin_init_timerfd() for(;0;)
#endif

#ifdef HAVE_SYS_EVENTFD_H
void sleepy_penguin_init_eventfd(void);
#else
#  define sleepy_penguin_init_eventfd() for(;0;)
#endif

#ifdef HAVE_SYS_INOTIFY_H
void sleepy_penguin_init_inotify(void);
#else
#  define sleepy_penguin_init_inotify() for(;0;)
#endif

#ifdef HAVE_SYS_SIGNALFD_H
void sleepy_penguin_init_signalfd(void);
#else
#  define sleepy_penguin_init_signalfd() for(;0;)
#endif

static size_t l1_cache_line_size_detect(void)
{
#ifdef _SC_LEVEL1_DCACHE_LINESIZE
	long tmp = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);

	if (tmp > 0 && tmp <= L1_CACHE_LINE_MAX)
		return (size_t)tmp;
#endif /* _SC_LEVEL1_DCACHE_LINESIZE */
	return L1_CACHE_LINE_MAX;
}

void Init_sleepy_penguin_ext(void)
{
	VALUE mSleepyPenguin;

	rb_sp_l1_cache_line_size = l1_cache_line_size_detect();

	mSleepyPenguin = rb_define_module("SleepyPenguin");
	rb_define_const(mSleepyPenguin, "SLEEPY_PENGUIN_VERSION",
			rb_str_new2(MY_GIT_VERSION));

	sleepy_penguin_init_kqueue();
	sleepy_penguin_init_epoll();
	sleepy_penguin_init_timerfd();
	sleepy_penguin_init_eventfd();
	sleepy_penguin_init_inotify();
	sleepy_penguin_init_signalfd();
}
