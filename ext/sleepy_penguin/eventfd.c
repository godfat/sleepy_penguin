#ifdef HAVE_SYS_EVENTFD_H
#include "sleepy_penguin.h"
#include <sys/eventfd.h>
#include "nonblock.h"
static ID id_for_fd;

static VALUE create(int argc, VALUE *argv, VALUE klass)
{
	VALUE _initval, _flags;
	unsigned initval;
	int flags = 0;
	int fd;

	rb_scan_args(argc, argv, "11", &_initval, &_flags);
	initval = NUM2UINT(_initval);
	flags = NIL_P(_flags) ? 0 : NUM2INT(_flags);

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

#ifdef HAVE_RB_THREAD_BLOCKING_REGION
struct efd_args {
	int fd;
	uint64_t val;
};

static VALUE efd_write(void *_args)
{
	struct efd_args *args = _args;
	ssize_t w = write(args->fd, &args->buf, sizeof(uint64_t));

	return (VALUE)w;
}

static VALUE efd_read(void *_args)
{
	struct efd_args *args = _args;
	ssize_t r = read(args->fd, &args->buf, sizeof(uint64_t));

	return (VALUE)r;
}

static VALUE incr(VALUE self, VALUE value)
{
	struct efd_args x;
	ssize_t w;

	x.fd = my_fileno(self);
	x.val = (uint64_t)NUM2ULL(value);

retry:
	w = (ssize_t)rb_thread_blocking_region(efd_write, &x, RUBY_UBF_IO, 0);
	if (w == -1) {
		if (rb_io_wait_writable(x.fd))
			goto retry;
		rb_sys_fail("write(eventfd)");
	}

	return Qnil;
}

static VALUE getvalue(VALUE self)
{
	struct efd_args x;

	x.fd = my_fileno(self);

retry:
	w = (ssize_t)rb_thread_blocking_region(efd_read, &x, RUBY_UBF_IO, 0);
	if (w == -1) {
		if (rb_io_wait_readable(x.fd))
			goto retry;
		rb_sys_fail("read(eventfd)");
	}

	return ULL2NUM(x.buf);
}
#else  /* !HAVE_RB_THREAD_BLOCKING_REGION */

static VALUE incr(VALUE self, VALUE value)
{
	int fd = my_fileno(self);
	uint64_t val = (uint64_t)NUM2ULL(value);
	ssize_t w;

	set_nonblock(fd);
retry:
	w = write(fd, &val, sizeof(uint64_t));
	if (w == -1) {
		if (rb_io_wait_writable(fd))
			goto retry;
		rb_sys_fail("write(eventfd)");
	}

	return Qnil;
}

static VALUE getvalue(VALUE self)
{
	int fd = my_fileno(self);
	uint64_t val;
	ssize_t r;

	set_nonblock(fd);
retry:
	r = read(fd, &val, sizeof(uint64_t));
	if (r == -1) {
		if (rb_io_wait_readable(fd))
			goto retry;
		rb_sys_fail("read(eventfd)");
	}

	return ULL2NUM(val);
}
#endif /* !HAVE_RB_THREAD_BLOCKING_REGION */

static VALUE getvalue_nonblock(VALUE self)
{
	int fd = my_fileno(self);
	uint64_t val;
	ssize_t r;

	set_nonblock(fd);
	r = read(fd, &val, sizeof(uint64_t));
	if (r == -1)
		rb_sys_fail("read(eventfd)");

	return ULL2NUM(val);
}

static VALUE incr_nonblock(VALUE self, VALUE value)
{
	int fd = my_fileno(self);
	uint64_t val = (uint64_t)NUM2ULL(value);
	ssize_t w;

	set_nonblock(fd);
	w = write(fd, &val, sizeof(uint64_t));
	if (w == -1)
		rb_sys_fail("write(eventfd)");

	return Qnil;
}

void sleepy_penguin_init_eventfd(void)
{
	VALUE mSleepyPenguin, cEventFD;

	mSleepyPenguin = rb_const_get(rb_cObject, rb_intern("SleepyPenguin"));
	cEventFD = rb_define_class_under(mSleepyPenguin, "EventFD", rb_cIO);
	rb_define_singleton_method(cEventFD, "new", create, -1);
#ifdef EFD_NONBLOCK
	rb_define_const(cEventFD, "NONBLOCK", UINT2NUM(EFD_NONBLOCK));
#endif
#ifdef EFD_CLOEXEC
	rb_define_const(cEventFD, "CLOEXEC", UINT2NUM(EFD_CLOEXEC));
#endif
#ifdef EFD_SEMAPHORE
	rb_define_const(cEventFD, "SEMAPHORE", UINT2NUM(EFD_SEMAPHORE));
#endif
	rb_define_method(cEventFD, "value", getvalue, 0);
	rb_define_method(cEventFD, "incr", incr, 1);
	rb_define_method(cEventFD, "value_nonblock", getvalue_nonblock, 0);
	rb_define_method(cEventFD, "incr_nonblock", incr_nonblock, 1);
	id_for_fd = rb_intern("for_fd");
}
#endif /* HAVE_SYS_EVENTFD_H */
