#ifdef HAVE_SYS_TIMERFD_H
#include "sleepy_penguin.h"
#include <sys/timerfd.h>
#include "value2timespec.h"

/*
 * call-seq:
 *	TimerFD.new([clockid[, flags]]) -> TimerFD IO object
 *
 * Creates a new timer as an IO object.
 *
 * If set +clockid+ must be be one of the following:
 * - :REALTIME - use the settable clock
 * - :MONOTONIC - use the non-settable clock unaffected by manual changes
 *
 * +clockid+ defaults to :MONOTONIC if unspecified
 * +flags+ may be any or none of the following:
 *
 * - :CLOEXEC - set the close-on-exec flag on the new object
 * - :NONBLOCK - set the non-blocking I/O flag on the new object
 */
static VALUE s_new(int argc, VALUE *argv, VALUE klass)
{
	VALUE cid, fl, rv;
	int clockid, flags;
	int fd;

	rb_scan_args(argc, argv, "02", &cid, &fl);
	clockid = NIL_P(cid) ? CLOCK_MONOTONIC : rb_sp_get_flags(klass, cid);
	flags = rb_sp_get_flags(klass, fl);

	fd = timerfd_create(clockid, flags);
	if (fd == -1) {
		if (errno == EMFILE || errno == ENFILE || errno == ENOMEM) {
			rb_gc();
			fd = timerfd_create(clockid, flags);
		}
		if (fd == -1)
			rb_sys_fail("timerfd_create");
	}

	rv = INT2FIX(fd);
	return rb_call_super(1, &rv);
}

static VALUE itimerspec2ary(struct itimerspec *its)
{
	VALUE interval = timespec2num(&its->it_interval);
	VALUE value = timespec2num(&its->it_value);

	return rb_ary_new3(2, interval, value);
}

/*
 * call-seq:
 *	tfd.settime(flags, interval, value) -> [ old_interval, old_value ]
 *
 * Arms (starts) or disarms (stops) the timer referred by the TimerFD object
 * and returns the old value of the timer.
 *
 * +flags+ is either zero (or nil) to start a relative timer or :ABSTIME
 * to start an absolute timer.  If the +interval+ is zero, the timer fires
 * only once, otherwise the timer is fired every +interval+ seconds.
 * +value+ is the time of the initial expiration in seconds.
 */
static VALUE settime(VALUE self, VALUE fl, VALUE interval, VALUE value)
{
	int fd = rb_sp_fileno(self);
	int flags = rb_sp_get_flags(self, fl);
	struct itimerspec old, new;

	value2timespec(&new.it_interval, interval);
	value2timespec(&new.it_value, value);

	if (timerfd_settime(fd, flags, &new, &old) == -1)
		rb_sys_fail("timerfd_settime");

	return itimerspec2ary(&old);
}

/*
 * call-seq:
 *	tfd#gettime	-> [ interval, value ]
 *
 * Returns the current +interval+ and +value+ of the timer as an Array.
 */
static VALUE gettime(VALUE self)
{
	int fd = rb_sp_fileno(self);
	struct itimerspec curr;

	if (timerfd_gettime(fd, &curr) == -1)
		rb_sys_fail("timerfd_gettime");

	return itimerspec2ary(&curr);
}

static VALUE tfd_read(void *args)
{
	uint64_t *buf = args;
	int fd = (int)(*buf);
	ssize_t r = read(fd, buf, sizeof(uint64_t));

	return (VALUE)r;
}

/*
 * call-seq:
 *	tfd.expirations([nonblock])		-> Integer
 *
 * Returns the number of expirations that have occurred.  This will block
 * if no expirations have occurred at the time of the call.  Returns +nil+
 * if +nonblock+ is passed and is +true+
 */
static VALUE expirations(int argc, VALUE *argv, VALUE self)
{
	ssize_t r;
	int fd = rb_sp_fileno(self);
	uint64_t buf = (uint64_t)fd;
	VALUE nonblock;

	rb_scan_args(argc, argv, "01", &nonblock);
	if (RTEST(nonblock))
		rb_sp_set_nonblock(fd);
	else
		blocking_io_prepare(fd);
retry:
	r = (ssize_t)rb_sp_fd_region(tfd_read, &buf, fd);
	if (r == -1) {
		if (errno == EAGAIN && RTEST(nonblock))
			return Qnil;
		if (rb_io_wait_readable(fd = rb_sp_fileno(self)))
			goto retry;
		rb_sys_fail("read(timerfd)");
	}

	return ULL2NUM(buf);
}

void sleepy_penguin_init_timerfd(void)
{
	VALUE mSleepyPenguin, cTimerFD;

	mSleepyPenguin = rb_define_module("SleepyPenguin");

	/*
	 * Document-class: SleepyPenguin::TimerFD
	 *
	 * TimerFD exposes kernel timers as IO objects that may be monitored
	 * by IO.select or Epoll.  IO#close disarms the timers and returns
	 * resources back to the kernel.
	 */
	cTimerFD = rb_define_class_under(mSleepyPenguin, "TimerFD", rb_cIO);
	rb_define_singleton_method(cTimerFD, "new", s_new, -1);
	NODOC_CONST(cTimerFD, "REALTIME", UINT2NUM(CLOCK_REALTIME));
	NODOC_CONST(cTimerFD, "MONOTONIC", UINT2NUM(CLOCK_MONOTONIC));
	NODOC_CONST(cTimerFD, "ABSTIME", UINT2NUM(TFD_TIMER_ABSTIME));
#ifdef TFD_NONBLOCK
	NODOC_CONST(cTimerFD, "NONBLOCK", UINT2NUM(TFD_NONBLOCK));
#endif
#ifdef TFD_CLOEXEC
	NODOC_CONST(cTimerFD, "CLOEXEC", UINT2NUM(TFD_CLOEXEC));
#endif

	rb_define_method(cTimerFD, "settime", settime, 3);
	rb_define_method(cTimerFD, "gettime", gettime, 0);
	rb_define_method(cTimerFD, "expirations", expirations, -1);
}
#endif /* HAVE_SYS_TIMERFD_H */
