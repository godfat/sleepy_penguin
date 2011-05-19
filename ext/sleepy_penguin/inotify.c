#ifdef HAVE_SYS_INOTIFY_H
#include "sleepy_penguin.h"
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include "missing_inotify.h"

static ID id_for_fd, id_inotify_buf, id_inotify_tmp, id_mask;
static VALUE cEvent, checks;

/*
 * call-seq:
 *	Inotify.new([flags])     -> Inotify IO object
 *
 * Flags may be any of the following as an Array of Symbols or Integer mask:
 * - :NONBLOCK - sets the non-blocking flag on the descriptor watched.
 * - :CLOEXEC - sets the close-on-exec flag
 */
static VALUE s_new(int argc, VALUE *argv, VALUE klass)
{
	VALUE _flags, rv;
	int flags;
	int fd;

	rb_scan_args(argc, argv, "01", &_flags);
	flags = rb_sp_get_flags(klass, _flags);

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

/*
 * call-seq:
 *	ino.add_watch(path, flags) -> Integer
 *
 * Adds a watch on an object specified by its +mask+, returns an unsigned
 * Integer watch descriptor.  +flags+ may be a mask of the following
 * Inotify constants or array of their symbolic names.
 *
 * - :ACCESS - File was accessed (read) (*)
 * - :ATTRIB - Metadata changed.
 * - :CLOSE_WRITE - File opened for writing was closed (*)
 * - :CLOSE_NOWRITE - File not opened for writing was closed (*)
 * - :CREATE - File/directory created in watched directory (*)
 * - :DELETE - File/directory deleted from watched directory (*)
 * - :DELETE_SELF - Watched file/directory was itself deleted
 * - :MODIFY - File was modified (*)
 * - :MOVE_SELF - Watched file/directory was itself moved
 * - :MOVED_FROM - File moved out of watched directory (*)
 * - :MOVED_TO - File moved into watched directory (*)
 * - :OPEN - File was opened (*)
 *
 * When monitoring a directory, the events marked with an asterisk (*)
 * above can occur for files in the directory, in which case the name
 * field in the Event structure identifies the name of the file in the
 * directory.
 *
 * Shortcut flags:
 *
 * - :ALL_EVENTS - a bitmask of all the above events
 * - :MOVE - :MOVED_FROM or :MOVED_TO
 * - :CLOSE - :CLOSE_WRITE or :CLOSE_NOWRITE
 *
 * The following watch attributes may also be included in flags:
 *
 * - :DONT_FOLLOW - don't dereference symlinks (since Linux 2.6.15)
 * - :EXCL_UNLINK - don't generate unlink events for children (since 2.6.36)
 * - :MASK_ADD - add events to an existing watch mask if it exists
 * - :ONESHOT - monitor for one event and then remove it from the watch
 * - :ONLYDIR - only watch the pathname if it is a directory
 */
static VALUE add_watch(VALUE self, VALUE path, VALUE vmask)
{
	int fd = rb_sp_fileno(self);
	const char *pathname = StringValueCStr(path);
	uint32_t mask = rb_sp_get_uflags(self, vmask);
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

/*
 * call-seq:
 *	ino.rm_watch(watch_descriptor) -> 0
 *
 * Removes a watch based on a watch descriptor Integer.  The watch
 * descriptor is a return value given by Inotify#add_watch
 */
static VALUE rm_watch(VALUE self, VALUE vwd)
{
	uint32_t wd = NUM2UINT(vwd);
	int fd = rb_sp_fileno(self);
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

struct inread_args {
	int fd;
	struct inotify_event *ptr;
	long len;
};

static VALUE inread(void *ptr)
{
	struct inread_args *args = ptr;

	return (VALUE)read(args->fd, args->ptr, args->len);
}

/*
 * call-seq:
 *	ino.take([nonblock]) -> Inotify::Event or nil
 *
 * Returns the next Inotify::Event processed.  May return +nil+ if +nonblock+
 * is +true+.
 */
static VALUE take(int argc, VALUE *argv, VALUE self)
{
	struct inread_args args;
	VALUE buf;
	VALUE tmp = rb_ivar_get(self, id_inotify_tmp);
	struct inotify_event *e, *end;
	ssize_t r;
	VALUE rv = Qnil;
	VALUE nonblock;

	if (RARRAY_LEN(tmp) > 0)
		return rb_ary_shift(tmp);

	rb_scan_args(argc, argv, "01", &nonblock);

	args.fd = rb_sp_fileno(self);
	buf = rb_ivar_get(self, id_inotify_buf);
	args.len = RSTRING_LEN(buf);
	args.ptr = (struct inotify_event *)RSTRING_PTR(buf);

	if (RTEST(nonblock))
		rb_sp_set_nonblock(args.fd);
	else
		blocking_io_prepare(args.fd);
	do {
		r = rb_sp_io_region(inread, &args);
		if (r == 0 /* Linux < 2.6.21 */
		    ||
		    (r < 0 && errno == EINVAL) /* Linux >= 2.6.21 */
		   ) {
			/* resize internal buffer */
			int newlen;
			if (args.len > 0x10000)
				rb_raise(rb_eRuntimeError, "path too long");
			if (ioctl(args.fd, FIONREAD, &newlen) != 0)
				rb_sys_fail("ioctl(inotify,FIONREAD)");
			rb_str_resize(buf, newlen);
			args.ptr = (struct inotify_event *)RSTRING_PTR(buf);
			args.len = newlen;
		} else if (r < 0) {
			if (errno == EAGAIN && RTEST(nonblock)) {
				return Qnil;
			} else {
				if (!rb_io_wait_readable(args.fd))
					rb_sys_fail("read(inotify)");
			}
		} else {
			/* buffer in userspace to minimize read() calls */
			end = (struct inotify_event *)((char *)args.ptr + r);
			for (e = args.ptr; e < end; ) {
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

/*
 * call-seq:
 *	inotify_event.events => [ :MOVED_TO, ... ]
 *
 * Returns an array of symbolic event names based on the contents of
 * the +mask+ field.
 */
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

/*
 * call-seq:
 *	inotify.dup	-> another Inotify object
 *
 * Duplicates an Inotify object, allowing it to be used in a blocking
 * fashion in another thread.  Ensures duplicated Inotify objects do
 * not share read buffers, but do share the userspace Array buffer.
 */
static VALUE init_copy(VALUE dest, VALUE orig)
{
	VALUE tmp;

	dest = rb_call_super(1, &orig); /* copy all other ivars as-is */
	rb_ivar_set(dest, id_inotify_buf, rb_str_new(0, 128));

	return dest;
}

/*
 * call-seq:
 *	ino.each { |event| ... } -> ino
 *
 * Yields each Inotify::Event received in a blocking fashion.
 */
static VALUE each(VALUE self)
{
	VALUE argv = Qfalse;

	while (1)
		rb_yield(take(0, &argv, self));

	return self;
}

void sleepy_penguin_init_inotify(void)
{
	VALUE mSleepyPenguin, cInotify;

	mSleepyPenguin = rb_define_module("SleepyPenguin");

	/*
	 * Document-class: SleepyPenguin::Inotify
	 *
	 * Inotify objects are used for monitoring file system events,
	 * it can monitor individual files or directories.  When a directory
	 * is monitored it will return events for the directory itself and
	 * all files inside the directory.
	 *
	 * Inotify IO objects can be watched using IO.select or Epoll.
	 * IO#close may be called on the object when it is no longer needed.
	 *
	 * Inotify is available on Linux 2.6.13 or later.
	 *
	 *	require "sleepy_penguin/sp"
	 *	ino = SP::Inotify.new
	 *	ino.add_watch("/path/to/foo", :OPEN)
	 *	ino.each do |event|
	 *	  p event.events # => [ :OPEN ]
	 *	end
	 */
	cInotify = rb_define_class_under(mSleepyPenguin, "Inotify", rb_cIO);
	rb_define_method(cInotify, "add_watch", add_watch, 2);
	rb_define_method(cInotify, "rm_watch", rm_watch, 1);
	rb_define_method(cInotify, "initialize_copy", init_copy, 1);
	rb_define_method(cInotify, "take", take, -1);
	rb_define_method(cInotify, "each", each, 0);

	/*
	 * Document-class: SleepyPenguin::Inotify::Event
	 *
	 * Returned by SleepyPenguin::Inotify#take.  It is a Struct with the
	 * following elements:
	 *
	 * - wd   - watch descriptor (unsigned Integer)
	 * - mask - mask of events (unsigned Integer)
	 * - cookie - unique cookie associated related events (for rename)
	 * - name - optional string name (may be nil)
	 *
	 * The mask is a bitmask of the event flags accepted by
	 * Inotify#add_watch and may also include the following flags:
	 *
	 * - :IGNORED - watch was removed
	 * - :ISDIR - event occured on a directory
	 * - :Q_OVERFLOW - event queue overflowed (wd is -1)
	 * - :UNMOUNT - filesystem containing watched object was unmounted
	 *
	 * Use the Event#events method to get an array of symbols for the
	 * matched events.
	 */
	cEvent = rb_struct_define("Event", "wd", "mask", "cookie", "name", 0);
	cEvent = rb_define_class_under(cInotify, "Event", cEvent);
	rb_define_method(cEvent, "events", events, 0);
	rb_define_singleton_method(cInotify, "new", s_new, -1);
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

	NODOC_CONST(cInotify, "NONBLOCK", INT2NUM(IN_NONBLOCK));
	NODOC_CONST(cInotify, "CLOEXEC", INT2NUM(IN_CLOEXEC));
}
#endif /* HAVE_SYS_INOTIFY_H */
