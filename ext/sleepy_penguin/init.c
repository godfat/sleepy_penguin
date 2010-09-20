void sleepy_penguin_init_epoll(void);

#ifdef HAVE_SYS_TIMERFD_H
void sleepy_penguin_init_timerfd(void);
#else
#  define sleepy_penguin_init_timerfd() if(0)
#endif

void Init_sleepy_penguin_ext(void)
{
	sleepy_penguin_init_epoll();
	sleepy_penguin_init_timerfd();
}
