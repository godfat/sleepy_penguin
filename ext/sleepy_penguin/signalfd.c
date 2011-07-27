#ifdef HAVE_SYS_SIGNALFD_H
#include "sleepy_penguin.h"
#include <signal.h>
#include <sys/signalfd.h>
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
 *	sfd.update!(signals[, flags])	-> sfd
 *
 * Updates the signal mask watched for by the given +sfd+.
 * Takes the same arguments as SignalFD.new.
 */
static VALUE update_bang(int argc, VALUE *argv, VALUE self)
{
	VALUE vmask, vflags;
	sigset_t mask;
	int flags;
	int fd = rb_sp_fileno(self);
	int rc;

	rb_scan_args(argc, argv, "02", &vmask, &vflags);
	flags = NIL_P(vflags) ? cur_flags(fd) : rb_sp_get_flags(self, vflags);
	value2sigset(&mask, vmask);

	rc = signalfd(fd, &mask, flags);
	if (rc == -1)
		rb_sys_fail("signalfd");
	return self;
}

/*
 * call-seq:
 *	SignalFD.new(signals[, flags])	-> SignalFD IO object
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
 * Starting with Linux 2.6.27, +flags+ may be a mask that consists of any
 * of the following:
 *
 * - :CLOEXEC - set the close-on-exec flag on the new object
 * - :NONBLOCK - set the non-blocking I/O flag on the new object
 */
static VALUE s_new(int argc, VALUE *argv, VALUE klass)
{
	VALUE vmask, vflags, rv;
	sigset_t mask;
	int flags;
	int fd;

	rb_scan_args(argc, argv, "02", &vmask, &vflags);
	flags = rb_sp_get_flags(klass, vflags);
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


	rv = INT2FIX(fd);
	return rb_call_super(1, &rv);
}

static VALUE ssi_alloc(VALUE klass)
{
	struct signalfd_siginfo *ssi = ALLOC(struct signalfd_siginfo);

	return Data_Wrap_Struct(klass, NULL, -1, ssi);
}

/* :nodoc: */
static VALUE ssi_init(VALUE self)
{
	struct signalfd_siginfo *ssi = DATA_PTR(self);

	memset(ssi, 0, sizeof(struct signalfd_siginfo));
	return self;
}

static VALUE sfd_read(void *args)
{
	struct signalfd_siginfo *ssi = args;
	int fd = ssi->ssi_fd;
	ssize_t r = read(fd, ssi, sizeof(struct signalfd_siginfo));

	return (VALUE)r;
}

/*
 * call-seq:
 *	sfd.take([nonblock]) -> SignalFD::SigInfo object or +nil+
 *
 * Returns the next SigInfo object representing a received signal.
 * If +nonblock+ is specified and true, this may return +nil+
 */
static VALUE sfd_take(int argc, VALUE *argv, VALUE self)
{
	VALUE rv = ssi_alloc(cSigInfo);
	struct signalfd_siginfo *ssi = DATA_PTR(rv);
	ssize_t r;
	int fd;
	VALUE nonblock;

	rb_scan_args(argc, argv, "01", &nonblock);
	fd = rb_sp_fileno(self);
	if (RTEST(nonblock))
		rb_sp_set_nonblock(fd);
	else
		blocking_io_prepare(fd);
retry:
	ssi->ssi_fd = fd;
	r = (ssize_t)rb_sp_fd_region(sfd_read, ssi, fd);
	if (r == -1) {
		if (errno == EAGAIN && RTEST(nonblock))
			return Qnil;
		if (rb_io_wait_readable(fd = rb_sp_fileno(self)))
			goto retry;
		rb_sys_fail("read(signalfd)");
	}
	if (r == 0)
		rb_eof_error(); /* does this ever happen? */
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

	/*
	 * Document-class: SleepyPenguin::SignalFD
	 *
	 * Use of this class is NOT recommended.  Ruby itself has a great
	 * signal handling API and its implementation conflicts with this.
	 *
	 * This class is currently disabled and the documentation is only
	 * provided to describe what it would look like.
	 *
	 * A SignalFD is an IO object for accepting signals.  It provides
	 * an alternative to Signal.trap that may be monitored using
	 * IO.select or Epoll.
	 *
	 * SignalFD appears interact unpredictably with YARV (Ruby 1.9) signal
	 * handling and has been unreliable in our testing. Since Ruby has a
	 * decent signal handling interface anyways, this class is less useful
	 * than signalfd() in a C-only environment.
	 *
	 * It is not supported at all.
	 */
	cSignalFD = rb_define_class_under(mSleepyPenguin, "SignalFD", rb_cIO);

	/*
	 * Document-class: SleepyPenguin::SignalFD::SigInfo
	 *
	 * This class is returned by SignalFD#take.  It consists of the
	 * following read-only members:
	 *
	 * - signo - signal number
	 * - errno - error number
	 * - code - signal code
	 * - pid - PID of sender
	 * - uid - real UID of sender
	 * - fd - file descriptor (SIGIO)
	 * - tid - kernel timer ID (POSIX timers)
	 * - band - band event (SIGIO)
	 * - overrun - POSIX timer overrun count
	 * - trapno - trap number that caused hardware-generated signal
	 * - exit status or signal (SIGCHLD)
	 * - int - integer sent by sigqueue(2)
	 * - ptr - Pointer sent by sigqueue(2)
	 * - utime - User CPU time consumed (SIGCHLD)
	 * - stime - System CPU time consumed (SIGCHLD)
	 * - addr - address that generated a hardware-generated signal
	 */
	cSigInfo = rb_define_class_under(cSignalFD, "SigInfo", rb_cObject);
	rb_define_alloc_func(cSigInfo, ssi_alloc);
	rb_define_private_method(cSigInfo, "initialize", ssi_init, 0);

	/* TODO:  si_code values */

	rb_define_singleton_method(cSignalFD, "new", s_new, -1);
#ifdef SFD_NONBLOCK
	NODOC_CONST(cSignalFD, "NONBLOCK", INT2NUM(SFD_NONBLOCK));
#endif
#ifdef SFD_CLOEXEC
	NODOC_CONST(cSignalFD, "CLOEXEC", INT2NUM(SFD_CLOEXEC));
#endif

	rb_define_method(cSignalFD, "take", sfd_take, -1);
	rb_define_method(cSignalFD, "update!", update_bang, -1);
	ssi_members = rb_ary_new();

	NODOC_CONST(cSigInfo, "MEMBERS", ssi_members);

	/*
	 * the minimum signal number for real-time signals,
	 * 34 on NPTL-based systems
	 */
	rb_define_const(cSignalFD, "RTMIN", INT2NUM(SIGRTMIN));

	/*
	 * the maximum signal number for real-time signals,
	 * 64 on NPTL-based systems
	 */
	rb_define_const(cSignalFD, "RTMAX", INT2NUM(SIGRTMAX));

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
