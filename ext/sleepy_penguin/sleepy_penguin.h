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

#define get_uflags rb_sp_get_uflags
#define get_flags rb_sp_get_flags
#define my_io_closed rb_sp_io_closed
#define my_fileno rb_sp_fileno

#define NODOC_CONST(klass,name,value) \
  rb_define_const((klass),(name),(value))
#endif /* SLEEPY_PENGUIN_H */
