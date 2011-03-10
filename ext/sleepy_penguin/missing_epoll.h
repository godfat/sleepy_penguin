#ifndef EPOLL_CLOEXEC
#  define EPOLL_CLOEXEC (int)(02000000)
#endif

#ifndef HAVE_EPOLL_CREATE1
/*
 * fake epoll_create1() since some systems don't have it.
 * Don't worry about thread-safety since current Ruby 1.9 won't
 * call this without GVL.
 */
static int my_epoll_create1(int flags)
{
	int fd = epoll_create(1024); /* size ignored since 2.6.8 */

	if (fd < 0 || flags == 0)
		return fd;

	if ((flags & EPOLL_CLOEXEC) && (fcntl(fd, F_SETFD, FD_CLOEXEC) == -1))
		goto err;
	return fd;
err:
	{
		int saved_errno = errno;
		close(fd);
		errno = saved_errno;
		return -1;
	}
}
#  define epoll_create1 my_epoll_create1
#endif
