#ifdef HAVE_SYS_SIGNALFD_H
#include "sleepy_penguin.h"
#include "nonblock.h"
#include <sys/signalfd.h>
static ID id_for_fd, id_list;
static VALUE ssi_members;
static VALUE cSigInfo;

/* converts a Symbol, String, or Fixnum to an integer signal */
static int sig2int(VALUE sig)
{
	static VALUE list;
	const char *ptr;
	long len;

	if (TYPE(sig) == T_FIXNUM)
		return FIX2INT(sig);

	sig = rb_obj_as_string(sig);
	len = RSTRING_LEN(sig);
	ptr = RSTRING_PTR(sig);

	if (len > 3 && !memcmp("SIG", ptr, 3))
		sig = rb_str_new(ptr + 3, len - 3);

	if (!list) {
		VALUE tmp = rb_const_get(rb_cObject, rb_intern("Signal"));

		list = rb_funcall(tmp, rb_intern("list"), 0, 0);
		rb_global_variable(&list);
	}

	sig = rb_hash_aref(list, sig);
	if (NIL_P(sig))
		rb_raise(rb_eArgError, "invalid signal: %s", ptr);

	return NUM2INT(sig);
}

/* fills sigset_t with an Array of signals */
static void value2sigset(sigset_t *mask, VALUE set)
{
	sigemptyset(mask);

	switch (TYPE(set)) {
	case T_NIL: return;
	case T_ARRAY: {
		VALUE *ptr = RARRAY_PTR(set);
		long len = RARRAY_LEN(set);

		while (--len >= 0)
			sigaddset(mask, sig2int(*ptr++));
		}
		break;
	default:
		sigaddset(mask, sig2int(set));
	}
}

static int cur_flags(int fd)
{
	int rv = 0;
#ifdef SFD_CLOEXEC
	{
		int flags = fcntl(fd, F_GETFD);
		if (flags == -1) rb_sys_fail("fcntl(F_GETFD)");
		if (flags & FD_CLOEXEC) rv |= SFD_CLOEXEC;
	}
#endif
#ifdef SFD_NONBLOCK
	{
		int flags = fcntl(fd, F_GETFL);
		if (flags == -1) rb_sys_fail("fcntl(F_GETFL)");
		if (flags & O_NONBLOCK) rv |= SFD_NONBLOCK;
	}
#endif
	return rv;
}

/*
 * call-seq:
 *	include SleepyPenguin
 *	sfd = SignalFD.new
 *	...
 *	sfd.update! signals
 *	sfd.update! signals, flags
 *
 * Updates the signal mask watched for by the given +sigfd+.
 * Takes the same arguments as SignalFD.new.
 */
static VALUE update_bang(int argc, VALUE *argv, VALUE self)
{
	VALUE vmask, vflags;
	sigset_t mask;
	int flags;
	int fd = my_fileno(self);
	int rc;

	rb_scan_args(argc, argv, "02", &vmask, &vflags);
	flags = NIL_P(vflags) ? cur_flags(fd) : NUM2INT(vflags);
	value2sigset(&mask, vmask);

	rc = signalfd(fd, &mask, flags);
	if (rc == -1)
		rb_sys_fail("signalfd");
	return self;
}

/*
 * call-seq:
 *	include SleepyPenguin
 *	SignalFD.new -> SignalFD IO object
 *	SignalFD.new(signals) -> SignalFD IO object
 *	SignalFD.new(signals, flags) -> SignalFD IO object
 *
 * Creates a new SignalFD object to watch given +signals+ with +flags+.
 *
 * +signals+ is an Array of signal names or a single signal name that
 * Signal.trap understands:
 *
 *	signals = [ :USR1, "USR2" ]
 *	signals = :USR1
 *	signals = 15
 *
 * +flags+ is a mask of SignalFD::CLOEXEC and SignalFD::NONBLOCK
 */
static VALUE s_new(int argc, VALUE *argv, VALUE klass)
{
	VALUE vmask, vflags;
	sigset_t mask;
	int flags;
	int fd;

	rb_scan_args(argc, argv, "02", &vmask, &vflags);
	flags = NIL_P(vflags) ? 0 : NUM2INT(vflags);
	value2sigset(&mask, vmask);

	fd = signalfd(-1, &mask, flags);
	if (fd == -1) {
		if (errno == EMFILE || errno == ENFILE || errno == ENOMEM) {
			rb_gc();
			fd = signalfd(-1, &mask, flags);
		}
		if (fd == -1)
			rb_sys_fail("signalfd");
	}

	return rb_funcall(klass, id_for_fd, 1, INT2NUM(fd));
}

static VALUE ssi_alloc(VALUE klass)
{
	struct signalfd_siginfo *ssi = ALLOC(struct signalfd_siginfo);

	return Data_Wrap_Struct(klass, NULL, -1, ssi);
}

static VALUE ssi_init(VALUE self)
{
	struct signalfd_siginfo *ssi = DATA_PTR(self);

	memset(ssi, 0, sizeof(struct signalfd_siginfo));
	return self;
}

#ifdef HAVE_RB_THREAD_BLOCKING_REGION
static VALUE xread(void *args)
{
	struct signalfd_siginfo *ssi = args;
	int fd = ssi->ssi_fd;
	ssize_t r = read(fd, ssi, sizeof(struct signalfd_siginfo));

	return (VALUE)r;
}

static ssize_t do_sfd_read(struct signalfd_siginfo *ssi)
{
	return (ssize_t)rb_thread_blocking_region(xread, ssi, RUBY_UBF_IO, 0);
}
#else /* ! HAVE_RB_THREAD_BLOCKING_REGION */
static ssize_t do_sfd_read(struct signalfd_siginfo *ssi)
{
	int fd = ssi->ssi_fd;
	ssize_t r;

	set_nonblock(fd);

	do
		r = read(fd, ssi, sizeof(struct signalfd_siginfo));
	while (r == -1 && rb_io_wait_readable(fd));

	return r;
}
#endif /* ! HAVE_RB_THREAD_BLOCKING_REGION */

/*
 * call-seq:
 *	sfd.take -> SignalFD::SigInfo object
 *
 * Returns the next SigInfo object representing a received signal.
 */
static VALUE sfd_take(VALUE self)
{
	VALUE rv = ssi_alloc(cSigInfo);
	struct signalfd_siginfo *ssi = DATA_PTR(rv);
	ssize_t r;

	ssi->ssi_fd = my_fileno(self);
	r = do_sfd_read(ssi);
	if (r < 0)
		rb_sys_fail("read(signalfd)");
	if (r == 0)
		rb_eof_error();
	return rv;
}

#define SSI_READER_FUNC(FN, FIELD) \
	static VALUE ssi_##FIELD(VALUE self) { \
		struct signalfd_siginfo *ssi = DATA_PTR(self); \
		return FN(ssi->ssi_##FIELD); \
	}

SSI_READER_FUNC(UINT2NUM,signo)
SSI_READER_FUNC(INT2NUM,errno)
SSI_READER_FUNC(INT2NUM,code)
SSI_READER_FUNC(UINT2NUM,pid)
SSI_READER_FUNC(UINT2NUM,uid)
SSI_READER_FUNC(INT2NUM,fd)
SSI_READER_FUNC(UINT2NUM,tid)
SSI_READER_FUNC(UINT2NUM,band)
SSI_READER_FUNC(UINT2NUM,overrun)
SSI_READER_FUNC(UINT2NUM,trapno)
SSI_READER_FUNC(INT2NUM,status)
SSI_READER_FUNC(INT2NUM,int)
SSI_READER_FUNC(ULL2NUM,ptr)
SSI_READER_FUNC(ULL2NUM,utime)
SSI_READER_FUNC(ULL2NUM,stime)
SSI_READER_FUNC(ULL2NUM,addr)

void sleepy_penguin_init_signalfd(void)
{
	VALUE mSleepyPenguin, cSignalFD;

	mSleepyPenguin = rb_define_module("SleepyPenguin");
	cSignalFD = rb_define_class_under(mSleepyPenguin, "SignalFD", rb_cIO);
	cSigInfo = rb_define_class_under(cSignalFD, "SigInfo", rb_cObject);

	rb_define_alloc_func(cSigInfo, ssi_alloc);
	rb_define_private_method(cSigInfo, "initialize", ssi_init, 0);

	rb_define_singleton_method(cSignalFD, "new", s_new, -1);
#ifdef SFD_NONBLOCK
	rb_define_const(cSignalFD, "NONBLOCK", INT2NUM(SFD_NONBLOCK));
#endif
#ifdef SFD_CLOEXEC
	rb_define_const(cSignalFD, "CLOEXEC", INT2NUM(SFD_CLOEXEC));
#endif

	rb_define_method(cSignalFD, "take", sfd_take, 0);
	rb_define_method(cSignalFD, "update!", update_bang, -1);
	id_for_fd = rb_intern("for_fd");
	ssi_members = rb_ary_new();
	rb_define_const(cSigInfo, "MEMBERS", ssi_members);

#define SSI_READER(FIELD) do { \
	  rb_define_method(cSigInfo, #FIELD, ssi_##FIELD, 0); \
	  rb_ary_push(ssi_members, ID2SYM(rb_intern(#FIELD))); \
	} while (0)

	SSI_READER(signo);
	SSI_READER(errno);
	SSI_READER(code);
	SSI_READER(pid);
	SSI_READER(uid);
	SSI_READER(fd);
	SSI_READER(tid);
	SSI_READER(band);
	SSI_READER(overrun);
	SSI_READER(trapno);
	SSI_READER(status);
	SSI_READER(int);
	SSI_READER(ptr);
	SSI_READER(utime);
	SSI_READER(stime);
	SSI_READER(addr);
	rb_obj_freeze(ssi_members);

	rb_require("sleepy_penguin/signalfd/sig_info");
}
#endif /* HAVE_SYS_SIGNALFD_H */
