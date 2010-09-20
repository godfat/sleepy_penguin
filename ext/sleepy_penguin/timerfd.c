#ifdef HAVE_SYS_TIMERFD_H
#include "sleepy_penguin.h"
#include <sys/timerfd.h>
#include "value2timespec.h"
static ID id_for_fd;

static VALUE create(int argc, VALUE *argv, VALUE klass)
{
	VALUE cid, fl;
	int clockid, flags = 0;
	int fd;

	rb_scan_args(argc, argv, "02", &cid, &fl);
	clockid = NIL_P(cid) ? CLOCK_MONOTONIC : NUM2INT(cid);
	flags = NIL_P(fl) ? 0 : NUM2INT(fl);

	fd = timerfd_create(clockid, flags);
	if (fd == -1) {
		if (errno == EMFILE || errno == ENFILE || errno == ENOMEM) {
			rb_gc();
			fd = timerfd_create(clockid, flags);
		}
		if (fd == -1)
			rb_sys_fail("timerfd_create");
	}

	return rb_funcall(klass, id_for_fd, 1, INT2NUM(fd));
}

static VALUE itimerspec2ary(struct itimerspec *its)
{
	VALUE interval = timespec2num(&its->it_interval);
	VALUE value = timespec2num(&its->it_value);

	return rb_ary_new3(2, interval, value);
}

static VALUE settime(VALUE self, VALUE fl, VALUE interval, VALUE value)
{
	int fd = my_fileno(self);
	int flags = NUM2INT(fl);
	struct itimerspec old, new;

	value2timespec(&new.it_interval, interval);
	value2timespec(&new.it_value, value);

	if (timerfd_settime(fd, flags, &new, &old) == -1)
		rb_sys_fail("timerfd_settime");

	return itimerspec2ary(&old);
}

static VALUE gettime(VALUE self)
{
	int fd = my_fileno(self);
	struct itimerspec curr;

	if (timerfd_gettime(fd, &curr) == -1)
		rb_sys_fail("timerfd_gettime");

	return itimerspec2ary(&curr);
}

#ifdef HAVE_RB_THREAD_BLOCKING_REGION
static VALUE tfd_read(void *args)
{
	uint64_t *buf = args;
	int fd = (int)(*buf);
	ssize_t r = read(fd, buf, sizeof(uint64_t));

	return (VALUE)r;
}

static VALUE expirations(VALUE self)
{
	ssize_t r;
	uint64_t buf = (int)my_fileno(self);

	r = (VALUE)rb_thread_blocking_region(tfd_read, &buf, RUBY_UBF_IO, 0);
	if (r == -1)
		rb_sys_fail("read(timerfd)");

	return ULL2NUM(buf);
}
#else /* ! HAVE_RB_THREAD_BLOCKING_REGION */
#include "nonblock.h"
static VALUE expirations(VALUE self)
{
	int fd = my_fileno(self);
	uint64_t buf;
	ssize_t r;

	set_nonblock(fd);
retry:
	r = read(fd, &buf, sizeof(uint64_t));
	if (r == -1) {
		if (rb_io_wait_readable(fd))
			goto retry;
		rb_sys_fail("read(timerfd)");
	}

	return ULL2NUM(buf);
}
#endif

void sleepy_penguin_init_timerfd(void)
{
	VALUE mSleepyPenguin, cTimerFD;

	mSleepyPenguin = rb_const_get(rb_cObject, rb_intern("SleepyPenguin"));
	cTimerFD = rb_define_class_under(mSleepyPenguin, "TimerFD", rb_cIO);
	rb_define_singleton_method(cTimerFD, "create", create, -1);
	rb_define_singleton_method(cTimerFD, "new", create, -1);
	rb_define_const(cTimerFD, "REALTIME", UINT2NUM(CLOCK_REALTIME));
	rb_define_const(cTimerFD, "MONOTONIC", UINT2NUM(CLOCK_MONOTONIC));
	rb_define_const(cTimerFD, "ABSTIME", UINT2NUM(TFD_TIMER_ABSTIME));
#ifdef TFD_NONBLOCK
	rb_define_const(cTimerFD, "NONBLOCK", UINT2NUM(TFD_NONBLOCK));
#endif
#ifdef TFD_CLOEXEC
	rb_define_const(cTimerFD, "CLOEXEC", UINT2NUM(TFD_CLOEXEC));
#endif

	rb_define_method(cTimerFD, "settime", settime, 3);
	rb_define_method(cTimerFD, "expirations", expirations, 0);
	id_for_fd = rb_intern("for_fd");
}
#endif /* HAVE_SYS_TIMERFD_H */
