#ifndef HAVE_RB_THREAD_FD_CLOSE
#define HAVE_RB_THREAD_FD_CLOSE
#  define rb_thread_fd_close(fd) for (;0;)
#endif
