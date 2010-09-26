#include "sleepy_penguin.h"
#include <sys/epoll.h>
#include <pthread.h>

#ifndef EPOLL_CLOEXEC
#  define EPOLL_CLOEXEC (int)(02000000)
#endif

#define EP_RECREATE (-2)

#ifndef HAVE_RB_MEMERROR
static void rb_memerror(void)
{
	static const char e[] = "[FATAL] failed to allocate memory\n"
	write(2, e, sizeof(e) - 1);
	abort();
}
#endif

static st_table *active;
static const int step = 64; /* unlikely to grow unless you're huge */
static VALUE cEpoll_IO;
static ID id_for_fd;

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
	int flags;
};

static struct rb_epoll *ep_get(VALUE self)
{
	struct rb_epoll *ep;

	Data_Get_Struct(self, struct rb_epoll, ep);

	return ep;
}

#ifndef HAVE_EPOLL_CREATE1
/*
 * fake epoll_create1() since some systems don't have it.
 * Don't worry about thread-safety since current Ruby 1.9 won't
 * call this without GVL.
 */
static int epoll_create1(int flags)
{
	int fd = epoll_create(1024); /* size ignored since 2.6.8 */

	if (fd < 0 || flags == 0)
		return fd;

	if ((flags & EPOLL_CLOEXEC) && (fcntl(fd, F_SETFD, FD_CLOEXEC) == -1))
		goto err;
	return fd;
err:
	{
		int saved_errno = errno;
		close(fd);
		errno = saved_errno;
		return -1;
	}
}
#endif

static void gcmark(void *ptr)
{
	struct rb_epoll *ep = ptr;

	rb_gc_mark(ep->io);
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
		/* can't raise during GC */
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
	ep->capa = step;
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
	st_insert(active, (st_data_t)ep->fd, (st_data_t)ep);
}

static void ep_check(struct rb_epoll *ep)
{
	if (ep->fd == EP_RECREATE)
		my_epoll_create(ep);
	if (ep->fd == -1)
		rb_raise(rb_eIOError, "closed");
}

/*
 * creates a new Epoll object with an optional +flags+ argument.
 * +flags+ may currently be +Epoll::CLOEXEC+ or 0 (or nil)
 */
static VALUE init(int argc, VALUE *argv, VALUE self)
{
	int flags;
	struct rb_epoll *ep = ep_get(self);
	VALUE fl;

	rb_scan_args(argc, argv, "01", &fl);
	if (NIL_P(fl)) {
		flags = EPOLL_CLOEXEC;
	} else {
		switch (TYPE(fl)) {
		case T_FIXNUM:
		case T_BIGNUM:
			flags = NUM2INT(fl);
			break;
		default:
			rb_raise(rb_eArgError, "flags must be an integer");
		}
	}
	ep->flags = flags;
	my_epoll_create(ep);

	return self;
}

static VALUE ctl(VALUE self, VALUE io, VALUE flags, int op)
{
	struct epoll_event event;
	struct rb_epoll *ep = ep_get(self);
	int fd = my_fileno(io);
	int rv;

	ep_check(ep);
	event.events = NUM2UINT(flags);
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

	return INT2NUM(rv);
}

/*
 */
static VALUE set(VALUE self, VALUE io, VALUE flags)
{
	struct epoll_event event;
	struct rb_epoll *ep = ep_get(self);
	int fd = my_fileno(io);
	int rv;

	ep_check(ep);
	event.events = NUM2UINT(flags);
	pack_event_data(&event, io);

	rv = epoll_ctl(ep->fd, EPOLL_CTL_MOD, fd, &event);
	if (rv == -1) {
		if (errno == ENOENT) {
			rv = epoll_ctl(ep->fd, EPOLL_CTL_ADD, fd, &event);
			if (rv == -1)
				rb_sys_fail("epoll_ctl - add");
			return INT2NUM(rv);
		}
		rb_sys_fail("epoll_ctl - mod");
	}

	return INT2NUM(rv);
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

	do {
		n = (int)rb_thread_blocking_region(nogvl_wait, ep,
		                                   RUBY_UBF_IO, 0);
	} while (n == -1 && errno == EINTR);

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

	TRAP_BEG;
	n = epoll_wait(ep->fd, ep->events, ep->maxevents, 0);
	TRAP_END;

	return n;
}

static int epwait_forever(struct rb_epoll *ep)
{
	int n;

	do {
		(void)rb_io_wait_readable(ep->fd);
		n = safe_epoll_wait(ep);
	} while (n == 0 || (n == -1 && errno == EINTR));

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

		/*
		 * if we got EINTR from epoll_wait /and/ timed out
		 * just consider it a timeout and don't raise an error
		 */

		if (n > 0 || (n == -1 && errno != EINTR))
			return n;

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

/* adds +io+ object the +self+ with +flags+ */
static VALUE add(VALUE self, VALUE io, VALUE flags)
{
	return ctl(self, io, flags, EPOLL_CTL_ADD);
}

/* adds +io+ object the +self+ with +flags+ */
static VALUE del(VALUE self, VALUE io)
{
	return ctl(self, io, INT2NUM(0), EPOLL_CTL_DEL);
}

static VALUE mod(VALUE self, VALUE io, VALUE flags)
{
	return ctl(self, io, flags, EPOLL_CTL_MOD);
}

static VALUE to_io(VALUE self)
{
	struct rb_epoll *ep = ep_get(self);

	ep_check(ep);

	if (NIL_P(ep->io))
		ep->io = rb_funcall(cEpoll_IO, id_for_fd, 1, INT2NUM(ep->fd));

	return ep->io;
}

static VALUE epclose(VALUE self)
{
	struct rb_epoll *ep = ep_get(self);

	if (ep->fd >= 0) {
		st_data_t key = ep->fd;
		st_delete(active, &key, NULL);
	}

	if (NIL_P(ep->io)) {
		if (ep->fd == EP_RECREATE) {
			ep->fd = -1;
		} else if (ep->fd == -1) {
			rb_raise(rb_eIOError, "closed");
		} else {
			int e = close(ep->fd);

			ep->fd = -1;
			if (e == -1)
				rb_sys_fail("close");
		}
	} else {
		ep->fd = -1;
		rb_io_close(ep->io);
	}

	return Qnil;
}

static VALUE epclosed(VALUE self)
{
	struct rb_epoll *ep = ep_get(self);

	return ep->fd == -1 ? Qtrue : Qfalse;
}

static VALUE init_copy(VALUE copy, VALUE orig)
{
	struct rb_epoll *a = ep_get(orig);
	struct rb_epoll *b = ep_get(copy);

	assert(a->events && b->events && a->events != b->events &&
	       NIL_P(b->io) && "Ruby broken?");

	ep_check(a);
	b->fd = dup(a->fd);
	if (b->fd == -1) {
		if (errno == ENFILE || errno == EMFILE) {
			rb_gc();
			b->fd = dup(a->fd);
		}
		if (b->fd == -1)
			rb_sys_fail("dup");
	}
	st_insert(active, (st_data_t)b->fd, (st_data_t)b);

	return copy;
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

	mSleepyPenguin = rb_const_get(rb_cObject, rb_intern("SleepyPenguin"));
	cEpoll = rb_define_class_under(mSleepyPenguin, "Epoll", rb_cObject);
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
	rb_define_method(cEpoll, "set", set, 2);
	rb_define_method(cEpoll, "wait", epwait, -1);
	rb_define_const(cEpoll, "CLOEXEC", INT2NUM(EPOLL_CLOEXEC));
	rb_define_const(cEpoll, "IN", INT2NUM(EPOLLIN));
	rb_define_const(cEpoll, "OUT", INT2NUM(EPOLLOUT));
	rb_define_const(cEpoll, "RDHUP", INT2NUM(EPOLLRDHUP));
	rb_define_const(cEpoll, "PRI", INT2NUM(EPOLLPRI));
	rb_define_const(cEpoll, "ERR", INT2NUM(EPOLLERR));
	rb_define_const(cEpoll, "HUP", INT2NUM(EPOLLHUP));
	rb_define_const(cEpoll, "ET", INT2NUM(EPOLLET));
	rb_define_const(cEpoll, "ONESHOT", INT2NUM(EPOLLONESHOT));
	id_for_fd = rb_intern("for_fd");
	active = st_init_numtable();

	if (pthread_atfork(NULL, NULL, atfork_child) != 0) {
		rb_gc();
		if (pthread_atfork(NULL, NULL, atfork_child) != 0)
			rb_memerror();
	}
}
