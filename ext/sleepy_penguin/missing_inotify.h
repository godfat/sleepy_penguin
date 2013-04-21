#ifndef IN_CLOEXEC
#  define IN_CLOEXEC 02000000
#endif
#ifndef IN_NONBLOCK
#  define IN_NONBLOCK O_NONBLOCK
#endif
#ifndef IN_ATTRIB
#  define IN_ATTRIB 0x00000004
#endif
#ifndef IN_ONLYDIR
#  define IN_ONLYDIR 0x01000000
#endif
#ifndef IN_DONT_FOLLOW
#  define IN_DONT_FOLLOW 0x02000000
#endif
#ifndef IN_EXCL_UNLINK
#  define IN_EXCL_UNLINK 0x04000000
#endif
#ifndef IN_MASK_ADD
#  define IN_MASK_ADD 0x20000000
#endif
#ifndef IN_ONESHOT
#  define IN_ONESHOT 0x80000000
#endif

#ifndef HAVE_INOTIFY_INIT1
/*
 * fake inotify_init1() since some systems don't have it
 * Don't worry about thread-safety since current Ruby 1.9 won't
 * call this without GVL.
 */
static int my_inotify_init1(int flags)
{
	int fd = inotify_init();
	int tmp;

	if (fd < 0)
		return fd;
	if ((flags & IN_CLOEXEC) && (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0))
		goto fcntl_err;
	if (flags & IN_NONBLOCK) {
		tmp = fcntl(fd, F_GETFL);
		if (tmp == -1)
			goto fcntl_err;
		if ((fcntl(fd, F_SETFL, tmp | O_NONBLOCK) < 0))
			goto fcntl_err;
	}

	return fd;
fcntl_err:
	tmp = errno;
	close(fd);
	errno = tmp;
	rb_sys_fail("fcntl");
}
# define inotify_init1 my_inotify_init1
#endif /* HAVE_INOTIFY_INIT1 */
