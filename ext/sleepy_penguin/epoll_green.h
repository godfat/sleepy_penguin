/* this file is only used by Matz Ruby 1.8 which used green threads */

/*
 * we have to worry about green threads and always pass zero
 * as the timeout for epoll_wait :(
 */
#include <rubysig.h>
#include <sys/time.h>

/* in case _BSD_SOURCE doesn't give us this macro */
#ifndef timersub
#  define timersub(a, b, result) \
do { \
	(result)->tv_sec = (a)->tv_sec - (b)->tv_sec; \
	(result)->tv_usec = (a)->tv_usec - (b)->tv_usec; \
	if ((result)->tv_usec < 0) { \
		--(result)->tv_sec; \
		(result)->tv_usec += 1000000; \
	} \
} while (0)
#endif

static int safe_epoll_wait(struct ep_per_thread *ept)
{
	int n;

	do {
		TRAP_BEG;
		n = epoll_wait(ept->ep->fd, ept->events, ept->maxevents, 0);
		TRAP_END;
	} while (n == -1 && errno == EINTR && ep_fd_check(ept->ep));

	return n;
}

static int epwait_forever(struct ep_per_thread *ept)
{
	int n;

	do {
		(void)rb_io_wait_readable(ept->ep->fd);
		n = safe_epoll_wait(ept);
	} while (n == 0);

	return n;
}

static int epwait_timed(struct ep_per_thread *ept)
{
	struct timeval tv;

	tv.tv_sec = ept->timeout / 1000;
	tv.tv_usec = (ept->timeout % 1000) * 1000;

	for (;;) {
		struct timeval t0, now, diff;
		int n;
		int fd = ept->ep->fd;
		fd_set rfds;

		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);

		gettimeofday(&t0, NULL);
		(void)rb_thread_select(fd + 1, &rfds, NULL, NULL, &tv);
		n = safe_epoll_wait(ept);
		if (n != 0)
			return n;

		/* XXX use CLOCK_MONOTONIC if people care about 1.8... */
		gettimeofday(&now, NULL);
		timersub(&now, &t0, &diff);
		timersub(&tv, &diff, &tv);

		if (tv.tv_usec < 0 || tv.tv_sec < 0)
			return (n == -1) ? 0 : n;
	}

	assert("should never get here (epwait_timed)");
	return -1;
}

static VALUE real_epwait(struct ep_per_thread *ept)
{
	int n;

	if (ept->timeout == -1)
		n = epwait_forever(ept);
	else if (ept->timeout == 0)
		n = safe_epoll_wait(ept);
	else
		n = epwait_timed(ept);

	return epwait_result(ept, n);
}
