#ifdef HAVE_SYS_EVENTFD_H
#include "sleepy_penguin.h"
#include <sys/eventfd.h>
static ID id_for_fd;

/*
 * call-seq:
 *	EventFD.new(initial_value [, flags])	-> EventFD IO object
 *
 * Creates an EventFD object.  +initial_value+ is a non-negative Integer
 * to start the internal counter at.
 *
 * Starting with Linux 2.6.27, +flags+ may be a mask that consists of any
 * of the following:
 *
 * - :CLOEXEC - set the close-on-exec flag on the new object
 * - :NONBLOCK - set the non-blocking I/O flag on the new object
 *
 * Since Linux 2.6.30, +flags+ may also include:
 * - :SEMAPHORE - provides semaphore-like semantics (see EventFD#value)
 */
static VALUE s_new(int argc, VALUE *argv, VALUE klass)
{
	VALUE _initval, _flags;
	unsigned initval;
	int flags;
	int fd;

	rb_scan_args(argc, argv, "11", &_initval, &_flags);
	initval = NUM2UINT(_initval);
	flags = rb_sp_get_flags(klass, _flags);

	fd = eventfd(initval, flags);
	if (fd == -1) {
		if (errno == EMFILE || errno == ENFILE || errno == ENOMEM) {
			rb_gc();
			fd = eventfd(initval, flags);
		}
		if (fd == -1)
			rb_sys_fail("eventfd");
	}

	return rb_funcall(klass, id_for_fd, 1, INT2NUM(fd));
}

struct efd_args {
	int fd;
	uint64_t val;
};

static VALUE efd_write(void *_args)
{
	struct efd_args *args = _args;
	ssize_t w = write(args->fd, &args->val, sizeof(uint64_t));

	return (VALUE)w;
}

static VALUE efd_read(void *_args)
{
	struct efd_args *args = _args;
	ssize_t r = read(args->fd, &args->val, sizeof(uint64_t));

	return (VALUE)r;
}

/*
 * call-seq:
 *	efd.incr(integer_value[, nonblock ])	-> true or nil
 *
 * Increments the internal counter by +integer_value+ which is an unsigned
 * Integer value.
 *
 * If +nonblock+ is specified and true, this will return +nil+ if the
 * internal counter will overflow the value of EventFD::MAX.
 * Otherwise it will block until the counter may be incremented without
 * overflowing.
 */
static VALUE incr(int argc, VALUE *argv, VALUE self)
{
	struct efd_args x;
	ssize_t w;
	VALUE value, nonblock;

	rb_scan_args(argc, argv, "11", &value, &nonblock);
	x.fd = rb_sp_fileno(self);
	RTEST(nonblock) ? rb_sp_set_nonblock(x.fd) : blocking_io_prepare(x.fd);
	x.val = (uint64_t)NUM2ULL(value);
retry:
	w = (ssize_t)rb_sp_io_region(efd_write, &x);
	if (w == -1) {
		if (errno == EAGAIN && RTEST(nonblock))
			return Qfalse;
		if (rb_io_wait_writable(x.fd))
			goto retry;
		rb_sys_fail("write(eventfd)");
	}

	return Qtrue;
}

/*
 * call-seq:
 *	efd.value([nonblock])	-> Integer or nil
 *
 * If not created as a semaphore, returns the current value and resets
 * the counter to zero.
 *
 * If created as a semaphore, this decrements the counter value by one
 * and returns +1+.
 *
 * If the counter is zero at the time of the call, this will block until
 * the counter becomes non-zero unless +nonblock+ is +true+, in which
 * case it returns +nil+.
 */
static VALUE getvalue(int argc, VALUE argv, VALUE self)
{
	struct efd_args x;
	ssize_t w;
	VALUE nonblock;

	rb_scan_args(argc, argv, "01", &nonblock);
	x.fd = rb_sp_fileno(self);
	RTEST(nonblock) ? rb_sp_set_nonblock(x.fd) : blocking_io_prepare(x.fd);
retry:
	w = (ssize_t)rb_sp_io_region(efd_read, &x);
	if (w == -1) {
		if (errno == EAGAIN && RTEST(nonblock))
			return Qnil;
		if (rb_io_wait_readable(x.fd))
			goto retry;
		rb_sys_fail("read(eventfd)");
	}

	return ULL2NUM(x.val);
}

void sleepy_penguin_init_eventfd(void)
{
	VALUE mSleepyPenguin, cEventFD;

	mSleepyPenguin = rb_define_module("SleepyPenguin");

	/*
	 * Document-class: SleepyPenguin::EventFD
	 *
	 * Applications may use EventFD instead of a pipe in cases where
	 * a pipe is only used to signal events.  The kernel overhead for
	 * an EventFD descriptor is much lower than that of a pipe.
	 *
	 * As of Linux 2.6.30, an EventFD may also be used as a semaphore.
	 */
	cEventFD = rb_define_class_under(mSleepyPenguin, "EventFD", rb_cIO);
	rb_define_singleton_method(cEventFD, "new", s_new, -1);

	/*
	 * the maximum value that may be stored in an EventFD,
	 * currently 0xfffffffffffffffe
	 */
	rb_define_const(cEventFD, "MAX", ULL2NUM(0xfffffffffffffffe));

#ifdef EFD_NONBLOCK
	NODOC_CONST(cEventFD, "NONBLOCK", INT2NUM(EFD_NONBLOCK));
#endif
#ifdef EFD_CLOEXEC
	NODOC_CONST(cEventFD, "CLOEXEC", INT2NUM(EFD_CLOEXEC));
#endif
#ifdef EFD_SEMAPHORE
	NODOC_CONST(cEventFD, "SEMAPHORE", INT2NUM(EFD_SEMAPHORE));
#endif
	rb_define_method(cEventFD, "value", getvalue, -1);
	rb_define_method(cEventFD, "incr", incr, -1);
	id_for_fd = rb_intern("for_fd");
}
#endif /* HAVE_SYS_EVENTFD_H */
