require 'mkmf'
have_header('sys/epoll.h') or abort 'sys/epoll.h not found'
have_header('sys/select.h')
have_func('epoll_create1', %w(sys/epoll.h))
have_func('rb_thread_blocking_region')
dir_config('sleepy_penguin')
create_makefile('sleepy_penguin_ext')
