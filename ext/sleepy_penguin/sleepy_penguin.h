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
int rb_sp_get_flags(VALUE klass, VALUE flags);
int rb_sp_io_closed(VALUE io);
int rb_sp_fileno(VALUE io);
void rb_sp_set_nonblock(int fd);

#ifdef HAVE_RB_THREAD_BLOCKING_REGION
static inline VALUE rb_sp_io_region(rb_blocking_function_t *func, void *data)
{
	return rb_thread_blocking_region(func, data, RUBY_UBF_IO, 0);
}
#  define blocking_io_prepare(fd) ((void)(fd))
#else
typedef VALUE rb_blocking_function_t(void *);
VALUE rb_sp_io_region(rb_blocking_function_t *func, void *data);
#  define blocking_io_prepare(fd) rb_sp_set_nonblock((fd))
#endif

#ifdef HAVE_RB_THREAD_IO_BLOCKING_REGION
VALUE rb_thread_io_blocking_region(rb_blocking_function_t *, void *, int);
#  define rb_sp_fd_region(fn,data,fd) \
          rb_thread_io_blocking_region((fn),(data),(fd))
#else
#  define rb_sp_fd_region(fn,data,fd) rb_sp_io_region(fn,data)
#endif

#define NODOC_CONST(klass,name,value) \
  rb_define_const((klass),(name),(value))


typedef int rb_sp_waitfn(int fd);
int rb_sp_wait(rb_sp_waitfn waiter, VALUE obj, int *fd);
#endif /* SLEEPY_PENGUIN_H */
