#include "sleepy_penguin.h"

static VALUE klass_for(VALUE klass)
{
	return (TYPE(klass) == T_CLASS) ? klass : CLASS_OF(klass);
}

int rb_sp_get_flags(VALUE klass, VALUE flags, int default_flags)
{
	switch (TYPE(flags)) {
	case T_NIL: return default_flags;
	case T_FIXNUM: return FIX2INT(flags);
	case T_BIGNUM: return NUM2INT(flags);
	case T_SYMBOL:
		return NUM2INT(rb_const_get(klass_for(klass), SYM2ID(flags)));
	case T_ARRAY: {
		VALUE *ptr = RARRAY_PTR(flags);
		long len = RARRAY_LEN(flags);
		int rv = 0;

		klass = klass_for(klass);
		while (--len >= 0) {
			VALUE tmp = *ptr++;

			Check_Type(tmp, T_SYMBOL);
			tmp = rb_const_get(klass, SYM2ID(tmp));
			rv |= NUM2INT(tmp);
		}
		return rv;
		}
	}
	rb_raise(rb_eTypeError, "invalid flags");
	return 0;
}

unsigned rb_sp_get_uflags(VALUE klass, VALUE flags)
{
	switch (TYPE(flags)) {
	case T_NIL: return 0;
	case T_FIXNUM: return FIX2UINT(flags);
	case T_BIGNUM: return NUM2UINT(flags);
	case T_SYMBOL:
		return NUM2UINT(rb_const_get(klass_for(klass), SYM2ID(flags)));
	case T_ARRAY: {
		VALUE *ptr = RARRAY_PTR(flags);
		long len = RARRAY_LEN(flags);
		unsigned rv = 0;

		klass = klass_for(klass);
		while (--len >= 0) {
			VALUE tmp = *ptr++;

			Check_Type(tmp, T_SYMBOL);
			tmp = rb_const_get(klass, SYM2ID(tmp));
			rv |= NUM2UINT(tmp);
		}
		return rv;
		}
	}
	rb_raise(rb_eTypeError, "invalid flags");
	return 0;
}

#if ! HAVE_RB_IO_T
#  define rb_io_t OpenFile
#endif

#ifdef GetReadFile
#  define FPTR_TO_FD(fptr) (fileno(GetReadFile(fptr)))
#else
#  if !HAVE_RB_IO_T || (RUBY_VERSION_MAJOR == 1 && RUBY_VERSION_MINOR == 8)
#    define FPTR_TO_FD(fptr) fileno(fptr->f)
#  else
#    define FPTR_TO_FD(fptr) fptr->fd
#  endif
#endif

static int fixint_closed_p(VALUE io)
{
	return (fcntl(FIX2INT(io), F_GETFD) == -1 && errno == EBADF);
}

#if defined(RFILE) && defined(HAVE_ST_FD)
static int my_rb_io_closed(VALUE io)
{
	return RFILE(io)->fptr->fd < 0;
}
#else
static int my_rb_io_closed(VALUE io)
{
	return rb_funcall(io, rb_intern("closed?"), 0) == Qtrue;
}
#endif

int rb_sp_io_closed(VALUE io)
{
	switch (TYPE(io)) {
	case T_FIXNUM:
		return fixint_closed_p(io);
	case T_FILE:
		break;
	default:
		io = rb_convert_type(io, T_FILE, "IO", "to_io");
	}

	return my_rb_io_closed(io);
}

int rb_sp_fileno(VALUE io)
{
	rb_io_t *fptr;

	io = rb_convert_type(io, T_FILE, "IO", "to_io");
	GetOpenFile(io, fptr);
	return FPTR_TO_FD(fptr);
}

void rb_sp_set_nonblock(int fd)
{
	int flags = fcntl(fd, F_GETFL);

	if (flags == -1)
		rb_sys_fail("fcntl(F_GETFL)");
	if ((flags & O_NONBLOCK) == O_NONBLOCK)
		return;
	/*
	 * Note: while this is Linux-only and we could safely rely on
	 * ioctl(FIONBIO), needing F_SETFL is an uncommon path, and
	 * F_GETFL is lockless.  ioctl(FIONBIO) always acquires a spin
	 * lock, so it's more expensive even if we do not need to change
	 * anything.
	 */
	flags = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	if (flags == -1)
		rb_sys_fail("fcntl(F_SETFL)");
}

int rb_sp_wait(rb_sp_waitfn waiter, VALUE obj, int *fd)
{
	/*
	 * we need to check the fileno before and after waiting, a close()
	 * could've happened at any time (especially when outside of GVL).
	 */
	int rc = waiter(rb_sp_fileno(obj));
	*fd = rb_sp_fileno(obj);
	return rc;
}
