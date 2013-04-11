class SleepyPenguin::Epoll

  # Epoll objects may be watched by IO.select and similar methods
  attr_reader :to_io

  # call-seq:
  #     SleepyPenguin::Epoll.new([flags]) -> Epoll object
  #
  # Creates a new Epoll object with an optional +flags+ argument.
  # +flags+ may currently be +:CLOEXEC+ or +0+ (or +nil+).
  def initialize(create_flags = nil)
    @to_io = SleepyPenguin::Epoll::IO.new(create_flags)
    @events = []
    @marks = []
    @pid = $$
    @create_flags = create_flags
    @copies = { @to_io => self }
  end

  def __ep_reinit # :nodoc:
    @events.clear
    @marks.clear
    @to_io = SleepyPenguin::Epoll::IO.new(@create_flags)
  end

  # auto-reinitialize the Epoll object after forking
  def __ep_check # :nodoc:
    return if @pid == $$
    return if @to_io.closed?
    objects = @copies.values
    @copies.each_key { |epio| epio.close }
    @copies.clear
    __ep_reinit
    objects.each do |obj|
      io_dup = @to_io.dup
      @copies[io_dup] = obj
    end
    @pid = $$
  end

  # Calls epoll_wait(2) and yields Integer +events+ and IO objects watched
  # for.  +maxevents+ is the maximum number of events to process at once,
  # lower numbers may prevent starvation when used by epoll_wait in multiple
  # threads.  Larger +maxevents+ reduces syscall overhead for
  # single-threaded applications. +maxevents+ defaults to 64 events.
  # +timeout+ is specified in milliseconds, +nil+
  # (the default) meaning it will block and wait indefinitely.
  def wait(maxevents = 64, timeout = nil)
    __ep_check
    @to_io.epoll_wait(maxevents, timeout) { |events, io| yield(events, io) }
  end

  # Starts watching a given +io+ object with +events+ which may be an Integer
  # bitmask or Array representing arrays to watch for.
  def add(io, events)
    __ep_check
    fd = io.to_io.fileno
    events = __event_flags(events)
    @to_io.epoll_ctl(CTL_ADD, io, events)
    @marks[fd] = io
    @events[fd] = events
    0
  end

  # call-seq:
  #     ep.del(io) -> 0
  #
  # Disables an IO object from being watched.
  def del(io)
    __ep_check
    fd = io.to_io.fileno
    rv = @to_io.epoll_ctl(CTL_DEL, io, 0)
    @marks[fd] = @events[fd] = nil
    rv
  end

  # call-seq:
  #     ep.delete(io) -> io or nil
  #
  # This method is deprecated and will be removed in sleepy_penguin 4.x
  #
  # Stops an +io+ object from being monitored.  This is like Epoll#del
  # but returns +nil+ on ENOENT instead of raising an error.  This is
  # useful for apps that do not care to track the status of an
  # epoll object itself.
  #
  # This method is deprecated and will be removed in sleepy_penguin 4.x
  def delete(io)
    __ep_check
    fd = io.to_io.fileno
    cur_io = @marks[fd]
    return if nil == cur_io || cur_io.to_io.closed?
    @marks[fd] = @events[fd] = nil
    @to_io.epoll_ctl(CTL_DEL, io, 0)
    io
  rescue Errno::ENOENT, Errno::EBADF
  end

  # call-seq:
  #     epoll.mod(io, flags) -> 0
  #
  # Changes the watch for an existing IO object based on +events+.
  # Returns zero on success, will raise SystemError on failure.
  def mod(io, events)
    __ep_check
    fd = io.to_io.fileno
    events = __event_flags(events)
    rv = @to_io.epoll_ctl(CTL_MOD, io, events)
    @marks[fd] = io
    @events[fd] = events
    rv
  end

  # call-seq:
  #     ep.set(io, flags) -> 0
  #
  # This method is deprecated and will be removed in sleepy_penguin 4.x
  #
  # Used to avoid exceptions when your app is too lazy to check
  # what state a descriptor is in, this sets the epoll descriptor
  # to watch an +io+ with the given +events+
  #
  # +events+ may be an array of symbols or an unsigned Integer bit mask:
  #
  # - events = [ :IN, :ET ]
  # - events = SleepyPenguin::Epoll::IN | SleepyPenguin::Epoll::ET
  #
  # See constants in Epoll for more information.
  #
  # This method is deprecated and will be removed in sleepy_penguin 4.x
  def set(io, events)
    __ep_check
    fd = io.to_io.fileno
    cur_io = @marks[fd]
    if cur_io == io
      cur_events = @events[fd]
      return 0 if (cur_events & ONESHOT) == 0 && cur_events == events
      begin
        @to_io.epoll_ctl(CTL_MOD, io, events)
      rescue Errno::ENOENT
        warn "epoll flag cache failed (mod -> add)"
        @to_io.epoll_ctl(CTL_ADD, io, events)
        @marks[fd] = io
      end
    else
      begin
        @to_io.epoll_ctl(CTL_ADD, io, events)
      rescue Errno::EEXIST
        warn "epoll flag cache failed (add -> mod)"
        @to_io.epoll_ctl(CTL_MOD, io, events)
      end
      @marks[fd] = io
    end
    @events[fd] = events
    0
  end

  # call-seq:
  #     ep.close -> nil
  #
  # Closes an existing Epoll object and returns memory back to the kernel.
  # Raises IOError if object is already closed.
  def close
    __ep_check
    @copies.delete(@to_io)
    @to_io.close
  end

  # call-seq:
  #     ep.closed? -> true or false
  #
  # Returns whether or not an Epoll object is closed.
  def closed?
    __ep_check
    @to_io.closed?
  end

  # we still support integer FDs for some debug functions
  def __fileno(io) # :nodoc:
    Integer === io ? io : io.to_io.fileno
  end

  # call-seq:
  #     ep.io_for(io) -> object
  #
  # Returns the given IO object currently being watched for.  Different
  # IO objects may internally refer to the same process file descriptor.
  # Mostly used for debugging.
  def io_for(io)
    __ep_check
    @marks[__fileno(io)]
  end

  # call-seq:
  #     epoll.events_for(io) -> Integer
  #
  # Returns the events currently watched for in current Epoll object.
  # Mostly used for debugging.
  def events_for(io)
    __ep_check
    @events[__fileno(io)]
  end

  # backwards compatibility, to be removed in 4.x
  alias flags_for events_for

  # call-seq:
  #     epoll.include?(io) -> true or false
  #
  # Returns whether or not a given IO is watched and prevented from being
  # garbage-collected by the current Epoll object.  This may include
  # closed IO objects.
  def include?(io)
    __ep_check
    @marks[__fileno(io)] ? true : nil
  end

  def initialize_copy(src) # :nodoc:
    __ep_check
    rv = super
    unless @to_io.closed?
      @to_io = @to_io.dup
      @copies[@to_io] = self
    end

    rv
  end
end
