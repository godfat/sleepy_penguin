#include "sleepy_penguin.h"
#include <sys/epoll.h>
#include <pthread.h>
#include <time.h>
#include "missing_epoll.h"
#ifdef HAVE_RUBY_ST_H
#  include <ruby/st.h>
#else
#  include <st.h>
#endif
#include "missing_rb_thread_fd_close.h"
#include "missing_rb_update_max_fd.h"
#define EP_RECREATE (-2)

static st_table *active;
static const int step = 64; /* unlikely to grow unless you're huge */
static VALUE cEpoll_IO;
static ID id_for_fd;

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

struct rb_epoll {
	int fd;
	int timeout;
	int maxevents;
	int capa;
	struct epoll_event *events;
	VALUE io;
	VALUE marks;
	VALUE flag_cache;
	int flags;
};

static struct rb_epoll *ep_get(VALUE self)
{
	struct rb_epoll *ep;

	Data_Get_Struct(self, struct rb_epoll, ep);

	return ep;
}

static void gcmark(void *ptr)
{
	struct rb_epoll *ep = ptr;

	rb_gc_mark(ep->io);
	rb_gc_mark(ep->marks);
	rb_gc_mark(ep->flag_cache);
}

static void gcfree(void *ptr)
{
	struct rb_epoll *ep = ptr;

	xfree(ep->events);
	if (ep->fd >= 0) {
		st_data_t key = ep->fd;
		st_delete(active, &key, NULL);
	}
	if (NIL_P(ep->io) && ep->fd >= 0) {
		/* can't raise during GC, and close() never fails in Linux */
		(void)close(ep->fd);
		errno = 0;
	}
	/* let GC take care of the underlying IO object if there is one */

	xfree(ep);
}

static VALUE alloc(VALUE klass)
{
	struct rb_epoll *ep;
	VALUE self;

	self = Data_Make_Struct(klass, struct rb_epoll, gcmark, gcfree, ep);
	ep->fd = -1;
	ep->io = Qnil;
	ep->marks = Qnil;
	ep->flag_cache = Qnil;
	ep->capa = step;
	ep->flags = 0;
	ep->events = xmalloc(sizeof(struct epoll_event) * ep->capa);

	return self;
}

static void my_epoll_create(struct rb_epoll *ep)
{
	ep->fd = epoll_create1(ep->flags);

	if (ep->fd == -1) {
		if (errno == EMFILE || errno == ENFILE || errno == ENOMEM) {
			rb_gc();
			ep->fd = epoll_create1(ep->flags);
		}
		if (ep->fd == -1)
			rb_sys_fail("epoll_create1");
	}
	rb_update_max_fd(ep->fd);
	st_insert(active, (st_data_t)ep->fd, (st_data_t)ep);
	ep->marks = rb_ary_new();
	ep->flag_cache = rb_ary_new();
}

static int ep_fd_check(struct rb_epoll *ep)
{
	if (ep->fd == -1)
		rb_raise(rb_eIOError, "closed epoll descriptor");
	return 1;
}

static void ep_check(struct rb_epoll *ep)
{
	if (ep->fd == EP_RECREATE)
		my_epoll_create(ep);
	ep_fd_check(ep);
	assert(TYPE(ep->marks) == T_ARRAY && "marks not initialized");
	assert(TYPE(ep->flag_cache) == T_ARRAY && "flag_cache not initialized");
}

/*
 * call-seq:
 *	SleepyPenguin::Epoll.new([flags])	-> Epoll object
 *
 * Creates a new Epoll object with an optional +flags+ argument.
 * +flags+ may currently be +:CLOEXEC+ or +0+ (or +nil+).
 */
static VALUE init(int argc, VALUE *argv, VALUE self)
{
	struct rb_epoll *ep = ep_get(self);
	VALUE fl;

	rb_scan_args(argc, argv, "01", &fl);
	ep->flags = rb_sp_get_flags(self, fl);
	my_epoll_create(ep);

	return self;
}

static VALUE ctl(VALUE self, VALUE io, VALUE flags, int op)
{
	struct epoll_event event;
	struct rb_epoll *ep = ep_get(self);
	int fd = rb_sp_fileno(io);
	int rv;

	ep_check(ep);
	event.events = rb_sp_get_uflags(self, flags);
	pack_event_data(&event, io);

	rv = epoll_ctl(ep->fd, op, fd, &event);
	if (rv == -1) {
		if (errno == ENOMEM) {
			rb_gc();
			rv = epoll_ctl(ep->fd, op, fd, &event);
		}
		if (rv == -1)
			rb_sys_fail("epoll_ctl");
	}
	switch (op) {
	case EPOLL_CTL_ADD:
		rb_ary_store(ep->marks, fd, io);
		/* fall-through */
	case EPOLL_CTL_MOD:
		flags = UINT2NUM(event.events);
		rb_ary_store(ep->flag_cache, fd, flags);
		break;
	case EPOLL_CTL_DEL:
		rb_ary_store(ep->marks, fd, Qnil);
		rb_ary_store(ep->flag_cache, fd, Qnil);
	}

	return INT2NUM(rv);
}

/*
 * call-seq:
 *	ep.set(io, flags)	-> 0
 *
 * Used to avoid exceptions when your app is too lazy to check
 * what state a descriptor is in, this sets the epoll descriptor
 * to watch an +io+ with the given +flags+
 *
 * +flags+ may be an array of symbols or an unsigned Integer bit mask:
 *
 * - flags = [ :IN, :ET ]
 * - flags = SleepyPenguin::Epoll::IN | SleepyPenguin::Epoll::ET
 *
 * See constants in Epoll for more information.
 */
static VALUE set(VALUE self, VALUE io, VALUE flags)
{
	struct epoll_event event;
	struct rb_epoll *ep = ep_get(self);
	int fd = rb_sp_fileno(io);
	int rv;
	VALUE cur_io = rb_ary_entry(ep->marks, fd);

	ep_check(ep);
	event.events = rb_sp_get_uflags(self, flags);
	pack_event_data(&event, io);

	if (cur_io == io) {
		VALUE cur_flags = rb_ary_entry(ep->flag_cache, fd);
		uint32_t cur_events;

		assert(!NIL_P(cur_flags) && "cur_flags nil but cur_io is not");
		cur_events = NUM2UINT(cur_flags);

		if (!(cur_events & EPOLLONESHOT) && cur_events == event.events)
			return Qnil;

fallback_mod:
		rv = epoll_ctl(ep->fd, EPOLL_CTL_MOD, fd, &event);
		if (rv == -1) {
			if (errno != ENOENT)
				rb_sys_fail("epoll_ctl - mod");
			errno = 0;
			rb_warn("epoll flag_cache failed (mod -> add)");
			goto fallback_add;
		}
	} else {
fallback_add:
		rv = epoll_ctl(ep->fd, EPOLL_CTL_ADD, fd, &event);
		if (rv == -1) {
			if (errno != EEXIST)
				rb_sys_fail("epoll_ctl - add");
			errno = 0;
			rb_warn("epoll flag_cache failed (add -> mod)");
			goto fallback_mod;
		}
		rb_ary_store(ep->marks, fd, io);
	}
	flags = UINT2NUM(event.events);
	rb_ary_store(ep->flag_cache, fd, flags);

	return INT2NUM(rv);
}

/*
 * call-seq:
 *	epoll.delete(io) -> io or nil
 *
 * Stops an +io+ object from being monitored.  This is like Epoll#del
 * but returns +nil+ on ENOENT instead of raising an error.  This is
 * useful for apps that do not care to track the status of an
 * epoll object itself.
 */
static VALUE delete(VALUE self, VALUE io)
{
	struct rb_epoll *ep = ep_get(self);
	int fd = rb_sp_fileno(io);
	int rv;
	VALUE cur_io;

	ep_check(ep);
	if (rb_sp_io_closed(io))
		goto out;

	cur_io = rb_ary_entry(ep->marks, fd);
	if (NIL_P(cur_io) || rb_sp_io_closed(cur_io))
		return Qnil;

	rv = epoll_ctl(ep->fd, EPOLL_CTL_DEL, fd, NULL);
	if (rv == -1) {
		/* beware of IO.for_fd-created descriptors */
		if (errno == ENOENT || errno == EBADF) {
			errno = 0;
			io = Qnil;
		} else {
			rb_sys_fail("epoll_ctl - del");
		}
	}
out:
	rb_ary_store(ep->marks, fd, Qnil);
	rb_ary_store(ep->flag_cache, fd, Qnil);

	return io;
}

static VALUE epwait_result(struct rb_epoll *ep, int n)
{
	int i;
	struct epoll_event *epoll_event = ep->events;
	VALUE obj_events, obj;

	if (n == -1)
		rb_sys_fail("epoll_wait");

	for (i = n; --i >= 0; epoll_event++) {
		obj_events = UINT2NUM(epoll_event->events);
		obj = unpack_event_data(epoll_event);
		rb_yield_values(2, obj_events, obj);
	}

	/* grow our event buffer for the next epoll_wait call */
	if (n == ep->capa) {
		xfree(ep->events);
		ep->capa += step;
		ep->events = xmalloc(sizeof(struct epoll_event) * ep->capa);
	}

	return INT2NUM(n);
}

static int epoll_resume_p(uint64_t expire_at, struct rb_epoll *ep)
{
	uint64_t now;

	ep_fd_check(ep);

	if (errno != EINTR)
		return 0;
	if (ep->timeout < 0)
		return 1;
	now = now_ms();
	ep->timeout = now > expire_at ? 0 : (int)(expire_at - now);
	return 1;
}

#if defined(HAVE_RB_THREAD_BLOCKING_REGION)
static VALUE nogvl_wait(void *args)
{
	struct rb_epoll *ep = args;
	int n = epoll_wait(ep->fd, ep->events, ep->maxevents, ep->timeout);

	return (VALUE)n;
}

static VALUE real_epwait(struct rb_epoll *ep)
{
	int n;
	uint64_t expire_at = ep->timeout > 0 ? now_ms() + ep->timeout : 0;

	do
		n = (int)rb_sp_fd_region(nogvl_wait, ep, ep->fd);
	while (n == -1 && epoll_resume_p(expire_at, ep));

	return epwait_result(ep, n);
}
#else /* 1.8 Green thread compatible code */
/*
 * we have to worry about green threads and always pass zero
 * as the timeout for epoll_wait :(
 */
# include <rubysig.h>
# include <sys/time.h>

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

static int safe_epoll_wait(struct rb_epoll *ep)
{
	int n;

	do {
		TRAP_BEG;
		n = epoll_wait(ep->fd, ep->events, ep->maxevents, 0);
		TRAP_END;
	} while (n == -1 && errno == EINTR && ep_fd_check(ep));

	return n;
}

static int epwait_forever(struct rb_epoll *ep)
{
	int n;

	do {
		(void)rb_io_wait_readable(ep->fd);
		n = safe_epoll_wait(ep);
	} while (n == 0);

	return n;
}

static int epwait_timed(struct rb_epoll *ep)
{
	struct timeval tv;

	tv.tv_sec = ep->timeout / 1000;
	tv.tv_usec = (ep->timeout % 1000) * 1000;

	for (;;) {
		struct timeval t0, now, diff;
		int n;
		fd_set rfds;

		FD_ZERO(&rfds);
		FD_SET(ep->fd, &rfds);

		gettimeofday(&t0, NULL);
		(void)rb_thread_select(ep->fd + 1, &rfds, NULL, NULL, &tv);
		n = safe_epoll_wait(ep);
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

static VALUE real_epwait(struct rb_epoll *ep)
{
	int n;

	if (ep->timeout == -1)
		n = epwait_forever(ep);
	else if (ep->timeout == 0)
		n = safe_epoll_wait(ep);
	else
		n = epwait_timed(ep);

	return epwait_result(ep, n);
}
#endif /* 1.8 Green thread compatibility code */

/*
 * call-seq:
 *	epoll.wait([maxevents[, timeout]]) { |flags, io| ... }
 *
 * Calls epoll_wait(2) and yields Integer +flags+ and IO objects watched
 * for.  +maxevents+ is the maximum number of events to process at once,
 * lower numbers may prevent starvation when used by dup-ed Epoll objects
 * in multiple threads. +timeout+ is specified in milliseconds, +nil+
 * (the default) meaning it will block and wait indefinitely.
 */
static VALUE epwait(int argc, VALUE *argv, VALUE self)
{
	VALUE timeout, maxevents;
	struct rb_epoll *ep = ep_get(self);

	ep_check(ep);
	rb_need_block();
	rb_scan_args(argc, argv, "02", &maxevents, &timeout);
	ep->timeout = NIL_P(timeout) ? -1 : NUM2INT(timeout);
	ep->maxevents = NIL_P(maxevents) ? ep->capa : NUM2INT(maxevents);

	if (ep->maxevents > ep->capa) {
		xfree(ep->events);
		ep->capa = ep->maxevents;
		ep->events = xmalloc(sizeof(struct epoll_event) * ep->capa);
	}

	return real_epwait(ep);
}

/*
 * call-seq:
 *	epoll.add(io, flags)	->  0
 *
 * Starts watching a given +io+ object with +flags+ which may be an Integer
 * bitmask or Array representing arrays to watch for.  Consider Epoll#set
 * instead as it is easier to use.
 */
static VALUE add(VALUE self, VALUE io, VALUE flags)
{
	return ctl(self, io, flags, EPOLL_CTL_ADD);
}

/*
 * call-seq:
 *	epoll.del(io)	-> 0
 *
 * Disables an IO object from being watched.  Consider Epoll#delete as
 * it is easier to use.
 */
static VALUE del(VALUE self, VALUE io)
{
	return ctl(self, io, INT2FIX(0), EPOLL_CTL_DEL);
}

/*
 * call-seq:
 *	epoll.mod(io, flags)	-> 0
 *
 * Changes the watch for an existing IO object based on +flags+.
 * Consider Epoll#set instead as it is easier to use.
 */
static VALUE mod(VALUE self, VALUE io, VALUE flags)
{
	return ctl(self, io, flags, EPOLL_CTL_MOD);
}

/*
 * call-seq:
 *	epoll.to_io	-> Epoll::IO object
 *
 * Used to expose the given Epoll object as an Epoll::IO object for IO.select
 * or IO#stat.  This is unlikely to be useful directly, but is used internally
 * by IO.select.
 */
static VALUE to_io(VALUE self)
{
	struct rb_epoll *ep = ep_get(self);

	ep_check(ep);

	if (NIL_P(ep->io))
		ep->io = rb_funcall(cEpoll_IO, id_for_fd, 1, INT2NUM(ep->fd));

	return ep->io;
}

/*
 * call-seq:
 *	epoll.close	-> nil
 *
 * Closes an existing Epoll object and returns memory back to the kernel.
 * Raises IOError if object is already closed.
 */
static VALUE epclose(VALUE self)
{
	struct rb_epoll *ep = ep_get(self);

	if (ep->fd >= 0) {
		st_data_t key = ep->fd;
		st_delete(active, &key, NULL);
	}

	if (NIL_P(ep->io)) {
		ep_fd_check(ep);

		if (ep->fd == EP_RECREATE) {
			ep->fd = -1; /* success */
		} else {
			int err;
			int fd = ep->fd;

			ep->fd = -1;
			rb_thread_fd_close(fd);
			err = close(fd);
			if (err == -1)
				rb_sys_fail("close");
		}
	} else {
		ep->fd = -1;
		rb_io_close(ep->io);
	}

	return Qnil;
}

/*
 * call-seq:
 *	epoll.closed?	-> true or false
 *
 * Returns whether or not an Epoll object is closed.
 */
static VALUE epclosed(VALUE self)
{
	struct rb_epoll *ep = ep_get(self);

	return ep->fd == -1 ? Qtrue : Qfalse;
}

static int cloexec_dup(struct rb_epoll *ep)
{
#ifdef F_DUPFD_CLOEXEC
	int flags = ep->flags & EPOLL_CLOEXEC ? F_DUPFD_CLOEXEC : F_DUPFD;
	int fd = fcntl(ep->fd, flags, 0);
#else /* potentially racy on GVL-free systems: */
	int fd = dup(ep->fd);
	if (fd >= 0)
		(void)fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
	return fd;
}

/*
 * call-seq:
 *	epoll.dup	-> another Epoll object
 *
 * Duplicates an Epoll object and userspace buffers related to this library.
 * This allows the same epoll object in the Linux kernel to be safely used
 * across multiple native threads as long as there is one SleepyPenguin::Epoll
 * object per-thread.
 */
static VALUE init_copy(VALUE copy, VALUE orig)
{
	struct rb_epoll *a = ep_get(orig);
	struct rb_epoll *b = ep_get(copy);

	assert(a->events && b->events && a->events != b->events &&
	       NIL_P(b->io) && "Ruby broken?");

	ep_check(a);
	assert(NIL_P(b->marks) && "mark array not nil");
	assert(NIL_P(b->flag_cache) && "flag_cache not nil");
	b->marks = a->marks;
	b->flag_cache = a->flag_cache;
	assert(TYPE(b->marks) == T_ARRAY && "mark array not initialized");
	assert(TYPE(b->flag_cache) == T_ARRAY && "flag_cache not initialized");
	b->flags = a->flags;
	b->fd = cloexec_dup(a);
	if (b->fd == -1) {
		if (errno == ENFILE || errno == EMFILE) {
			rb_gc();
			b->fd = cloexec_dup(a);
		}
		if (b->fd == -1)
			rb_sys_fail("dup");
	}
	st_insert(active, (st_data_t)b->fd, (st_data_t)b);

	return copy;
}

/* occasionally it's still useful to lookup aliased IO objects
 * based on for debugging */
static int my_fileno(VALUE obj)
{
	if (T_FIXNUM == TYPE(obj))
		return FIX2INT(obj);
	return rb_sp_fileno(obj);
}

/*
 * call-seq:
 *	epoll.io_for(io)	-> object
 *
 * Returns the given IO object currently being watched for.  Different
 * IO objects may internally refer to the same process file descriptor.
 * Mostly used for debugging.
 */
static VALUE io_for(VALUE self, VALUE obj)
{
	struct rb_epoll *ep = ep_get(self);

	return rb_ary_entry(ep->marks, my_fileno(obj));
}

/*
 * call-seq:
 *	epoll.flags_for(io)	-> Integer
 *
 * Returns the flags currently watched for in current Epoll object.
 * Mostly used for debugging.
 */
static VALUE flags_for(VALUE self, VALUE obj)
{
	struct rb_epoll *ep = ep_get(self);

	return rb_ary_entry(ep->flag_cache, my_fileno(obj));
}

/*
 * call-seq:
 *	epoll.include?(io) => true or false
 *
 * Returns whether or not a given IO is watched and prevented from being
 * garbage-collected by the current Epoll object.  This may include
 * closed IO objects.
 */
static VALUE include_p(VALUE self, VALUE obj)
{
	struct rb_epoll *ep = ep_get(self);

	return NIL_P(rb_ary_entry(ep->marks, my_fileno(obj))) ? Qfalse : Qtrue;
}

/*
 * we close (or lose to GC) epoll descriptors at fork to avoid leakage
 * and invalid objects being referenced later in the child
 */
static int ep_atfork(st_data_t key, st_data_t value, void *ignored)
{
	struct rb_epoll *ep = (struct rb_epoll *)value;

	if (NIL_P(ep->io)) {
		if (ep->fd >= 0)
			(void)close(ep->fd);
	} else {
		ep->io = Qnil; /* must let GC take care of it later :< */
	}
	ep->fd = EP_RECREATE;

	return ST_CONTINUE;
}

static void atfork_child(void)
{
	st_table *old = active;

	active = st_init_numtable();
	st_foreach(old, ep_atfork, (st_data_t)NULL);
	st_free_table(old);
}

void sleepy_penguin_init_epoll(void)
{
	VALUE mSleepyPenguin, cEpoll;

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
	 * - SP::EventFD
	 * - SP::Inotify
	 * - SP::TimerFD
	 */
	mSleepyPenguin = rb_define_module("SleepyPenguin");

	/*
	 * Document-class: SleepyPenguin::Epoll
	 *
	 * The Epoll class provides access to epoll(7) functionality in the
	 * Linux 2.6 kernel.  It provides fork and GC-safety for Ruby
	 * objects stored within the IO object and may be passed as an
	 * argument to IO.select.
	 */
	cEpoll = rb_define_class_under(mSleepyPenguin, "Epoll", rb_cObject);

	/*
	 * Document-class: SleepyPenguin::Epoll::IO
	 *
	 * Epoll::IO is an internal class.  Its only purpose is to be
	 * compatible with IO.select and related methods and should
	 * never be used directly, use Epoll instead.
	 */
	cEpoll_IO = rb_define_class_under(cEpoll, "IO", rb_cIO);
	rb_define_method(cEpoll, "initialize", init, -1);
	rb_define_method(cEpoll, "initialize_copy", init_copy, 1);
	rb_define_alloc_func(cEpoll, alloc);
	rb_define_method(cEpoll, "to_io", to_io, 0);
	rb_define_method(cEpoll, "close", epclose, 0);
	rb_define_method(cEpoll, "closed?", epclosed, 0);
	rb_define_method(cEpoll, "add", add, 2);
	rb_define_method(cEpoll, "mod", mod, 2);
	rb_define_method(cEpoll, "del", del, 1);
	rb_define_method(cEpoll, "delete", delete, 1);
	rb_define_method(cEpoll, "io_for", io_for, 1);
	rb_define_method(cEpoll, "flags_for", flags_for, 1);
	rb_define_method(cEpoll, "include?", include_p, 1);
	rb_define_method(cEpoll, "set", set, 2);
	rb_define_method(cEpoll, "wait", epwait, -1);

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
	active = st_init_numtable();

	if (pthread_atfork(NULL, NULL, atfork_child) != 0) {
		rb_gc();
		if (pthread_atfork(NULL, NULL, atfork_child) != 0)
			rb_memerror();
	}
}
