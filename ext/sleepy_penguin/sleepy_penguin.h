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
#  define blocking_io_prepare(fd) for(;0;)
#else
typedef VALUE rb_blocking_function_t(void *);
VALUE rb_sp_io_region(rb_blocking_function_t *func, void *data);
#  define blocking_io_prepare(fd) rb_sp_set_nonblock((fd))
#endif

#define NODOC_CONST(klass,name,value) \
  rb_define_const((klass),(name),(value))
#endif /* SLEEPY_PENGUIN_H */
