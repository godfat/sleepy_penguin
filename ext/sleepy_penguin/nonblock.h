#ifndef SLEEPY_PENGUIN_NONBLOCK_H
#define SLEEPY_PENGUIN_NONBLOCK_H
#include <unistd.h>
#include <fcntl.h>
#include <ruby.h>
static void set_nonblock(int fd)
{
	int flags = fcntl(fd, F_GETFL);

	if (flags == -1)
		rb_sys_fail("fcntl(F_GETFL)");
	if ((flags & O_NONBLOCK) == O_NONBLOCK)
		return;
	flags = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	if (flags == -1)
		rb_sys_fail("fcntl(F_SETFL)");
}

#endif /* SLEEPY_PENGUIN_NONBLOCK_H */
