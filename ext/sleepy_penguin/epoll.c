#include "sleepy_penguin.h"
#ifdef HAVE_SYS_EPOLL_H
#include <sys/epoll.h>
#include <unistd.h>
#include <time.h>
#include "missing_clock_gettime.h"
#include "missing_epoll.h"
#include "missing_rb_thread_fd_close.h"
#include "missing_rb_update_max_fd.h"

static ID id_for_fd;
static VALUE cEpoll;

static uint64_t now_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);

	return now.tv_sec * 1000 + (now.tv_nsec + 500000) / 1000000;
}

static void pack_event_data(struct epoll_event *event, VALUE obj)
{
	event->data.ptr = (void *)obj;
}

static VALUE unpack_event_data(struct epoll_event *event)
{
	return (VALUE)event->data.ptr;
}

struct ep_per_thread {
	VALUE io;
	int fd;
	int timeout;
	int maxevents;
	int capa;
	struct epoll_event events[FLEX_ARRAY];
};

/* this will raise if the IO is closed */
static int ep_fd_check(struct ep_per_thread *ept)
{
	int save_errno = errno;

	ept->fd = rb_sp_fileno(ept->io);
	errno = save_errno;

	return 1;
}

static struct ep_per_thread *ept_get(VALUE self, int maxevents)
{
	static __thread struct ep_per_thread *ept;
	size_t size;
	int err;
	void *ptr;

	/* error check here to prevent OOM from posix_memalign */
	if (maxevents <= 0) {
		errno = EINVAL;
		rb_sys_fail("epoll_wait maxevents <= 0");
	}

	if (ept && ept->capa >= maxevents)
		goto out;

	size = sizeof(struct ep_per_thread) +
	       sizeof(struct epoll_event) * maxevents;

	free(ept); /* free(NULL) is POSIX and works on glibc */
	err = posix_memalign(&ptr, rb_sp_l1_cache_line_size, size);
	if (err) {
		errno = err;
		rb_memerror();
	}
	ept = ptr;
	ept->capa = maxevents;
out:
	ept->maxevents = maxevents;
	ept->io = self;
	ept->fd = rb_sp_fileno(ept->io);

	return ept;
}

/*
 * call-seq:
 *	SleepyPenguin::Epoll::IO.new(flags)	-> Epoll::IO object
 *
 * Creates a new Epoll::IO object with the given +flags+ argument.
 * +flags+ may currently be +CLOEXEC+ or +0+.
 */
static VALUE s_new(VALUE klass, VALUE _flags)
{
	int default_flags = RB_SP_CLOEXEC(EPOLL_CLOEXEC);
	int flags = rb_sp_get_flags(klass, _flags, default_flags);
	int fd = epoll_create1(flags);
	VALUE rv;

	if (fd < 0) {
		if (errno == EMFILE || errno == ENFILE || errno == ENOMEM) {
			rb_gc();
			fd = epoll_create1(flags);
		}
		if (fd < 0)
			rb_sys_fail("epoll_create1");
	}

	rv = INT2FIX(fd);
	return rb_call_super(1, &rv);
}

/*
 * call-seq:
 * 	epoll_io.epoll_ctl(op, io, events)	-> nil
 *
 * Register, modify, or register a watch for a given +io+ for events.
 *
 * +op+ may be one of +EPOLL_CTL_ADD+, +EPOLL_CTL_MOD+, or +EPOLL_CTL_DEL+
 * +io+ is an IO object or one which proxies via the +to_io+ method.
 * +events+ is an integer mask of events to watch for.
 *
 * Returns nil on success.
 */
static VALUE epctl(VALUE self, VALUE _op, VALUE io, VALUE events)
{
	struct epoll_event event;
	int epfd = rb_sp_fileno(self);
	int fd = rb_sp_fileno(io);
	int op = NUM2INT(_op);
	int rv;

	event.events = NUM2UINT(events);
	pack_event_data(&event, io);

	rv = epoll_ctl(epfd, op, fd, &event);
	if (rv < 0)
		rb_sys_fail("epoll_ctl");

	return Qnil;
}

static VALUE epwait_result(struct ep_per_thread *ept, int n)
{
	int i;
	struct epoll_event *epoll_event = ept->events;
	VALUE obj_events, obj;

	if (n < 0) {
		if (errno == EINTR)
			n = 0;
		else
			rb_sys_fail("epoll_wait");
	}

	for (i = n; --i >= 0; epoll_event++) {
		obj_events = UINT2NUM(epoll_event->events);
		obj = unpack_event_data(epoll_event);
		rb_yield_values(2, obj_events, obj);
	}

	return INT2NUM(n);
}

static int epoll_resume_p(uint64_t expire_at, struct ep_per_thread *ept)
{
	uint64_t now;

	ep_fd_check(ept); /* may raise IOError */

	if (errno != EINTR)
		return 0;
	if (ept->timeout < 0)
		return 1;
	now = now_ms();
	ept->timeout = now > expire_at ? 0 : (int)(expire_at - now);
	return 1;
}

static VALUE nogvl_wait(void *args)
{
	struct ep_per_thread *ept = args;
	int n = epoll_wait(ept->fd, ept->events, ept->maxevents, ept->timeout);

	return (VALUE)n;
}

static VALUE real_epwait(struct ep_per_thread *ept)
{
	long n;
	uint64_t expire_at = ept->timeout > 0 ? now_ms() + ept->timeout : 0;

	do {
		n = (long)rb_sp_fd_region(nogvl_wait, ept, ept->fd);
	} while (n < 0 && epoll_resume_p(expire_at, ept));

	return epwait_result(ept, (int)n);
}

/*
 * call-seq:
 *	ep_io.epoll_wait([maxevents[, timeout]]) { |events, io| ... }
 *
 * Calls epoll_wait(2) and yields Integer +events+ and IO objects watched
 * for.  +maxevents+ is the maximum number of events to process at once,
 * lower numbers may prevent starvation when used by epoll_wait in multiple
 * threads.  Larger +maxevents+ reduces syscall overhead for
 * single-threaded applications. +maxevents+ defaults to 64 events.
 * +timeout+ is specified in milliseconds, +nil+
 * (the default) meaning it will block and wait indefinitely.
 */
static VALUE epwait(int argc, VALUE *argv, VALUE self)
{
	VALUE timeout, maxevents;
	struct ep_per_thread *ept;

	rb_need_block();
	rb_scan_args(argc, argv, "02", &maxevents, &timeout);

	ept = ept_get(self, NIL_P(maxevents) ? 64 : NUM2INT(maxevents));
	ept->timeout = NIL_P(timeout) ? -1 : NUM2INT(timeout);

	return real_epwait(ept);
}

/* :nodoc: */
static VALUE event_flags(VALUE self, VALUE flags)
{
	return UINT2NUM(rb_sp_get_uflags(self, flags));
}

void sleepy_penguin_init_epoll(void)
{
	VALUE mSleepyPenguin, cEpoll_IO;

	/*
	 * Document-module: SleepyPenguin
	 *
	 *	require "sleepy_penguin"
	 *	include SleepyPenguin
	 *
	 * The SleepyPenguin namespace includes the Epoll, Inotify,
	 * TimerFD, EventFD classes in its top level and no other constants.
	 *
	 * If you are uncomfortable including SleepyPenguin, you may also
	 * use the "SP" alias if it doesn't conflict with existing code:
	 *
	 *	require "sleepy_penguin/sp"
	 *
	 * And then access classes via:
	 *
	 * - SP::Epoll
	 * - SP::Epoll::IO
	 * - SP::EventFD
	 * - SP::Inotify
	 * - SP::TimerFD
	 */
	mSleepyPenguin = rb_define_module("SleepyPenguin");

	/*
	 * Document-class: SleepyPenguin::Epoll
	 *
	 * The Epoll class provides high-level access to epoll(7)
	 * functionality in the Linux 2.6 and later kernels.  It provides
	 * fork and GC-safety for Ruby objects stored within the IO object
	 * and may be passed as an argument to IO.select.
	 */
	cEpoll = rb_define_class_under(mSleepyPenguin, "Epoll", rb_cObject);

	/*
	 * Document-class: SleepyPenguin::Epoll::IO
	 *
	 * Epoll::IO is a low-level class.  It does not provide fork nor
	 * GC-safety, so Ruby IO objects added via epoll_ctl must be retained
	 * by the application until IO#close is called.
	 */
	cEpoll_IO = rb_define_class_under(cEpoll, "IO", rb_cIO);
	rb_define_singleton_method(cEpoll_IO, "new", s_new, 1);

	rb_define_method(cEpoll_IO, "epoll_ctl", epctl, 3);
	rb_define_method(cEpoll_IO, "epoll_wait", epwait, -1);

	rb_define_method(cEpoll, "__event_flags", event_flags, 1);

	/* registers an IO object via epoll_ctl */
	rb_define_const(cEpoll, "CTL_ADD", INT2NUM(EPOLL_CTL_ADD));

	/* unregisters an IO object via epoll_ctl */
	rb_define_const(cEpoll, "CTL_DEL", INT2NUM(EPOLL_CTL_DEL));

	/* modifies the registration of an IO object via epoll_ctl */
	rb_define_const(cEpoll, "CTL_MOD", INT2NUM(EPOLL_CTL_MOD));

	/* specifies whether close-on-exec flag is set for Epoll.new */
	rb_define_const(cEpoll, "CLOEXEC", INT2NUM(EPOLL_CLOEXEC));

	/* watch for read/recv operations */
	rb_define_const(cEpoll, "IN", UINT2NUM(EPOLLIN));

	/* watch for write/send operations */
	rb_define_const(cEpoll, "OUT", UINT2NUM(EPOLLOUT));

#ifdef EPOLLRDHUP
	/*
	 * Watch a specified io for shutdown(SHUT_WR) on the remote-end.
	 * Available since Linux 2.6.17.
	 */
	rb_define_const(cEpoll, "RDHUP", UINT2NUM(EPOLLRDHUP));
#endif

#ifdef EPOLLWAKEUP
	/*
	 * This prevents system suspend while event is ready.
	 * This requires the caller to have the CAP_BLOCK_SUSPEND capability
	 * Available since Linux 3.5
	 */
	rb_define_const(cEpoll, "WAKEUP", UINT2NUM(EPOLLWAKEUP));
#endif

	/* watch for urgent read(2) data */
	rb_define_const(cEpoll, "PRI", UINT2NUM(EPOLLPRI));

	/*
	 * watch for errors, there is no need to specify this,
	 * it is always monitored when an IO is watched
	 */
	rb_define_const(cEpoll, "ERR", UINT2NUM(EPOLLERR));

	/*
	 * watch for hangups, there is no need to specify this,
	 * it is always monitored when an IO is watched
	 */
	rb_define_const(cEpoll, "HUP", UINT2NUM(EPOLLHUP));

	/* notifications are only Edge Triggered, see epoll(7) */
	rb_define_const(cEpoll, "ET", UINT2NUM((uint32_t)EPOLLET));

	/* unwatch the descriptor once any event has fired */
	rb_define_const(cEpoll, "ONESHOT", UINT2NUM(EPOLLONESHOT));

	id_for_fd = rb_intern("for_fd");

	if (RB_SP_GREEN_THREAD)
		rb_require("sleepy_penguin/epoll/io");

	/* the high-level interface is implemented in Ruby: */
	rb_require("sleepy_penguin/epoll");
}
#endif /* HAVE_SYS_EPOLL_H */
