require 'mkmf'
have_header('sys/epoll.h') or abort 'sys/epoll.h not found'
have_header("pthread.h") or abort 'pthread.h not found'
have_header('sys/eventfd.h')
have_header('sys/signalfd.h')
have_header('sys/timerfd.h')
have_header('sys/inotify.h')
have_header('sys/signalfd.h')
have_header('ruby/io.h') and have_struct_member('rb_io_t', 'fd', 'ruby/io.h')
have_func('epoll_create1', %w(sys/epoll.h))
have_func('rb_thread_blocking_region')
have_library('pthread')
create_makefile('sleepy_penguin_ext')
