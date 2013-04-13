#ifndef SLEEPY_PENGUIN_H
#define SLEEPY_PENGUIN_H

#include <ruby.h>
#ifdef HAVE_RUBY_IO_H
#  include <ruby/io.h>
#else
#  include <rubyio.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <assert.h>
#include <unistd.h>

extern size_t rb_sp_l1_cache_line_size;
unsigned rb_sp_get_uflags(VALUE klass, VALUE flags);
int rb_sp_get_flags(VALUE klass, VALUE flags, int default_flags);
int rb_sp_io_closed(VALUE io);
int rb_sp_fileno(VALUE io);
void rb_sp_set_nonblock(int fd);

#if defined(HAVE_RB_THREAD_BLOCKING_REGION) || \
    defined(HAVE_RB_THREAD_IO_BLOCKING_REGION) || \
    defined(HAVE_RB_THREAD_CALL_WITHOUT_GVL)
#  define RB_SP_GREEN_THREAD 0
#  define blocking_io_prepare(fd) ((void)(fd))
#else
#  define RB_SP_GREEN_THREAD 1
#  define blocking_io_prepare(fd) rb_sp_set_nonblock((fd))
#endif

#ifdef HAVE_RB_THREAD_IO_BLOCKING_REGION
/* Ruby 1.9.3 and 2.0.0 */
VALUE rb_thread_io_blocking_region(rb_blocking_function_t *, void *, int);
#  define rb_sp_fd_region(fn,data,fd) \
	rb_thread_io_blocking_region((fn),(data),(fd))
#elif defined(HAVE_RB_THREAD_CALL_WITHOUT_GVL) && \
	defined(HAVE_RUBY_THREAD_H) && HAVE_RUBY_THREAD_H
/* in case Ruby 2.0+ ever drops rb_thread_io_blocking_region: */
#  include <ruby/thread.h>
#  define COMPAT_FN (void *(*)(void *))
#  define rb_sp_fd_region(fn,data,fd) \
	rb_thread_call_without_gvl(COMPAT_FN(fn),(data),RUBY_UBF_IO,NULL)
#elif defined(HAVE_RB_THREAD_BLOCKING_REGION)
/* Ruby 1.9.2 */
#  define rb_sp_fd_region(fn,data,fd) \
	rb_thread_blocking_region((fn),(data),RUBY_UBF_IO,NULL)
#else
/*
 * Ruby 1.8 does not have a GVL, we'll just enable signal interrupts
 * here in case we make interruptible syscalls.
 *
 * Note: epoll_wait with timeout=0 was interruptible until Linux 2.6.39
 */
#  include <rubysig.h>
static inline VALUE fake_blocking_region(VALUE (*fn)(void *), void *data)
{
	VALUE rv;

	TRAP_BEG;
	rv = fn(data);
	TRAP_END;

	return rv;
}
#  define rb_sp_fd_region(fn,data,fd) fake_blocking_region((fn),(data))
#endif

#define NODOC_CONST(klass,name,value) \
  rb_define_const((klass),(name),(value))

#ifdef HAVE_RB_FD_FIX_CLOEXEC
#  define RB_SP_CLOEXEC(flag) (flag)
#else
#  define RB_SP_CLOEXEC(flag) (0)
#endif

typedef int rb_sp_waitfn(int fd);
int rb_sp_wait(rb_sp_waitfn waiter, VALUE obj, int *fd);
#endif /* SLEEPY_PENGUIN_H */
