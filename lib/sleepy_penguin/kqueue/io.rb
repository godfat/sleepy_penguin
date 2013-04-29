class SleepyPenguin::Kqueue::IO
  # :stopdoc:
  # this file is only for Ruby 1.8 green threads compatibility
  alias __kevent kevent
  undef_method :kevent

  def __update_timeout(expire_at)
    now = Time.now
    diff = expire_at - now
    diff > 0 ? diff : 0
  end

  def kevent(changelist = nil, nevents = nil, timeout = nil)
    if block_given?
      expire_at = timeout ? Time.now + timeout : nil
      begin
        IO.select([self], nil, nil, timeout)
        n = __kevent(changelist, nevents, 0) do |*args|
          yield(*args)
        end
      end while n == 0 && timeout != 0 &&
                (expire_at == nil || timeout = __update_timeout(expire_at))
      n
    else
      # nevents should be zero or nil here
      __kevent(changelist, nevents, 0)
    end
  end
  # :startdoc:
end
