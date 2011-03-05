#ifdef HAVE_SYS_INOTIFY_H
#include "sleepy_penguin.h"
#include "nonblock.h"
#include <sys/inotify.h>
#include <sys/ioctl.h>
static ID id_for_fd, id_inotify_buf, id_inotify_tmp, id_mask;
static VALUE cEvent, checks;

#ifndef IN_CLOEXEC
#  define IN_CLOEXEC 02000000
#endif
#ifndef IN_NONBLOCK
#  define IN_NONBLOCK O_NONBLOCK
#endif
#ifndef IN_ATTRIB
#  define IN_ATTRIB 0x00000004
#endif
#ifndef IN_ONLYDIR
#  define IN_ONLYDIR 0x01000000
#endif
#ifndef IN_DONT_FOLLOW
#  define IN_DONT_FOLLOW 0x02000000
#endif
#ifndef IN_EXCL_UNLINK
#  define IN_EXCL_UNLINK 0x04000000
#endif
#ifndef IN_MASK_ADD
#  define IN_MASK_ADD 0x20000000
#endif
#ifndef IN_ONESHOT
#  define IN_ONESHOT 0x80000000
#endif

#ifndef HAVE_INOTIFY_INIT1
/*
 * fake inotify_init1() since some systems don't have it
 * Don't worry about thread-safety since current Ruby 1.9 won't
 * call this without GVL.
 */
static int my_inotify_init1(int flags)
{
	int fd = inotify_init();
	int tmp;

	if (fd < 0)
		return fd;
	if ((flags & IN_CLOEXEC) && (fcntl(fd, F_SETFD, FD_CLOEXEC) == -1))
		goto fcntl_err;
	if (flags & IN_NONBLOCK) {
		tmp = fcntl(fd, F_GETFL);
		if (tmp == -1)
			goto fcntl_err;
		if ((fcntl(fd, F_SETFL, tmp | O_NONBLOCK) != 0))
			goto fcntl_err;
	}

	return fd;
fcntl_err:
	tmp = errno;
	close(fd);
	errno = tmp;
	rb_sys_fail("fcntl");
}
# define inotify_init1 my_inotify_init1
#endif /* HAVE_INOTIFY_INIT1 */

static VALUE s_init(int argc, VALUE *argv, VALUE klass)
{
	VALUE _flags, rv;
	int flags;
	int fd;

	rb_scan_args(argc, argv, "01", &_flags);
	flags = NIL_P(_flags) ? 0 : NUM2INT(_flags);

	fd = inotify_init1(flags);
	if (fd == -1) {
		if (errno == EMFILE || errno == ENFILE || errno == ENOMEM) {
			rb_gc();
			fd = inotify_init1(flags);
		}
		if (fd == -1)
			rb_sys_fail("inotify_init1");
	}

	rv = rb_funcall(klass, id_for_fd, 1, INT2NUM(fd));
	rb_ivar_set(rv, id_inotify_buf, rb_str_new(0, 128));
	rb_ivar_set(rv, id_inotify_tmp, rb_ary_new());

	return rv;
}

static VALUE add_watch(VALUE self, VALUE path, VALUE vmask)
{
	int fd = my_fileno(self);
	const char *pathname = StringValueCStr(path);
	uint32_t mask = NUM2UINT(vmask);
	int rc = inotify_add_watch(fd, pathname, mask);

	if (rc == -1) {
		if (errno == ENOMEM) {
			rb_gc();
			rc = inotify_add_watch(fd, pathname, mask);
		}
		if (rc == -1)
			rb_sys_fail("inotify_add_watch");
	}
	return UINT2NUM((uint32_t)rc);
}

static VALUE rm_watch(VALUE self, VALUE vwd)
{
	uint32_t wd = NUM2UINT(vwd);
	int fd = my_fileno(self);
	int rc = inotify_rm_watch(fd, wd);

	if (rc == -1)
		rb_sys_fail("inotify_rm_watch");
	return INT2NUM(rc);
}

static size_t event_len(struct inotify_event *e)
{
	return sizeof(struct inotify_event) + e->len;
}

static VALUE event_new(struct inotify_event *e)
{
	VALUE wd = INT2NUM(e->wd);
	VALUE mask = UINT2NUM(e->mask);
	VALUE cookie = UINT2NUM(e->cookie);
	VALUE name;

	/* name may be zero-padded, so we do strlen() */
	name = e->len ? rb_str_new(e->name, strlen(e->name)) : Qnil;

	return rb_struct_new(cEvent, wd, mask, cookie, name);
}

static VALUE take(int argc, VALUE *argv, VALUE self)
{
	int fd = my_fileno(self);
	VALUE buf = rb_ivar_get(self, id_inotify_buf);
	VALUE tmp = rb_ivar_get(self, id_inotify_tmp);
	struct inotify_event *ptr;
	struct inotify_event *e, *end;
	long len;
	ssize_t r;
	VALUE rv = Qnil;
	VALUE nonblock;

	if (RARRAY_LEN(tmp) > 0)
		return rb_ary_shift(tmp);

	rb_scan_args(argc, argv, "01", &nonblock);

	len = RSTRING_LEN(buf);
	ptr = (struct inotify_event *)RSTRING_PTR(buf);
	do {
		set_nonblock(fd);
		r = read(fd, ptr, len);
		if (r == 0 || (r < 0 && errno == EINVAL)) {
			int newlen;
			if (len > 0x10000)
				rb_raise(rb_eRuntimeError, "path too long");
			if (ioctl(fd, FIONREAD, &newlen) != 0)
				rb_sys_fail("ioctl(inotify,FIONREAD)");
			rb_str_resize(buf, newlen);
			ptr = (struct inotify_event *)RSTRING_PTR(buf);
			len = newlen;
		} else if (r < 0) {
			if (errno == EAGAIN) {
				if (RTEST(nonblock))
					return Qnil;
				rb_io_wait_readable(fd);
			} else {
				rb_sys_fail("read(inotify)");
			}
		} else {
			end = (struct inotify_event *)((char *)ptr + r);
			for (e = ptr; e < end; ) {
				VALUE event = event_new(e);
				if (NIL_P(rv))
					rv = event;
				else
					rb_ary_push(tmp, event);
				e = (struct inotify_event *)
				    ((char *)e + event_len(e));
			}
		}
	} while (NIL_P(rv));

	return rv;
}

static VALUE events(VALUE self)
{
	long len = RARRAY_LEN(checks);
	VALUE *ptr = RARRAY_PTR(checks);
	VALUE pair;
	VALUE sym;
	VALUE rv = rb_ary_new();
	uint32_t mask;
	uint32_t event_mask = NUM2UINT(rb_funcall(self, id_mask, 0));

	for (; (len -= 2) >= 0;) {
		sym = *ptr++;
		mask = NUM2UINT(*ptr++);
		if ((event_mask & mask) == mask)
			rb_ary_push(rv, sym);
	}

	return rv;
}

static VALUE init_copy(VALUE dest, VALUE orig)
{
	VALUE tmp;

	dest = rb_call_super(1, &orig);
	rb_ivar_set(dest, id_inotify_buf, rb_str_new(0, 128));

	return dest;
}

void sleepy_penguin_init_inotify(void)
{
	VALUE mSleepyPenguin, cInotify;

	mSleepyPenguin = rb_define_module("SleepyPenguin");
	cInotify = rb_define_class_under(mSleepyPenguin, "Inotify", rb_cIO);
	rb_define_method(cInotify, "add_watch", add_watch, 2);
	rb_define_method(cInotify, "rm_watch", rm_watch, 1);
	rb_define_method(cInotify, "initialize_copy", init_copy, 1);
	rb_define_method(cInotify, "take", take, -1);
	cEvent = rb_struct_define(NULL, "wd", "mask", "cookie", "name", NULL);
	rb_define_const(cInotify, "Event", cEvent);
	rb_define_method(cEvent, "events", events, 0);
	rb_define_singleton_method(cInotify, "new", s_init, -1);
	id_for_fd = rb_intern("for_fd");
	id_inotify_buf = rb_intern("@inotify_buf");
	id_inotify_tmp = rb_intern("@inotify_tmp");
	id_mask = rb_intern("mask");
	checks = rb_ary_new();
	rb_global_variable(&checks);
#define IN(x) rb_define_const(cInotify,#x,UINT2NUM(IN_##x))
#define IN2(x) do { \
	VALUE val = UINT2NUM(IN_##x); \
	rb_define_const(cInotify,#x,val); \
	rb_ary_push(checks, ID2SYM(rb_intern(#x))); \
	rb_ary_push(checks, val); \
} while (0)

	rb_define_const(cInotify, "FIONREAD", INT2NUM(FIONREAD));

	IN(ALL_EVENTS);

/* events a user can watch for */
	IN2(ACCESS);
	IN2(MODIFY);
	IN2(ATTRIB);
	IN2(CLOSE_WRITE);
	IN2(CLOSE_NOWRITE);
	IN2(OPEN);
	IN2(MOVED_FROM);
	IN2(MOVED_TO);
	IN2(CREATE);
	IN2(DELETE);
	IN2(DELETE_SELF);
	IN2(MOVE_SELF);

/* sent as needed to any watch */
	IN2(UNMOUNT);
	IN2(Q_OVERFLOW);
	IN2(IGNORED);
	IN2(ISDIR);

/* helpers */
	IN(CLOSE);
	IN(MOVE);

/* special flags */
	IN(ONLYDIR);
	IN(DONT_FOLLOW);
	IN(EXCL_UNLINK);
	IN(MASK_ADD);
	IN(ONESHOT);

/* for inotify_init1() */
	IN(NONBLOCK);
	IN(CLOEXEC);
}
#endif /* HAVE_SYS_INOTIFY_H */
