class SleepyPenguin::Epoll::IO
  # :stopdoc:
  alias __epoll_wait epoll_wait
  undef_method :epoll_wait
  def epoll_wait(maxevents = 64, timeout = nil)
    begin
      if timeout == nil || timeout < 0 # wait forever
        begin
          IO.select([self])
          n = __epoll_wait(maxevents, 0) { |events,io| yield(events, io) }
        end while n == 0
      elsif timeout == 0
        return __epoll_wait(maxevents, 0) { |events,io| yield(events, io) }
      else
        done = Time.now + (timeout / 1000.0)
        begin
          tout = done - Time.now
          IO.select([self], nil, nil, tout) if tout > 0
          n = __epoll_wait(maxevents, 0) { |events,io| yield(events, io) }
        end while n == 0 && tout > 0
      end
      n
    rescue Errno::EINTR
      retry
    end
  end
  # :startdoc:
end
