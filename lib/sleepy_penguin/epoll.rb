require 'thread'
class SleepyPenguin::Epoll

  # call-seq:
  #     SleepyPenguin::Epoll.new([flags]) -> Epoll object
  #
  # Creates a new Epoll object with an optional +flags+ argument.
  # +flags+ may currently be +:CLOEXEC+ or +0+ (or +nil+).
  def initialize(create_flags = nil)
    @io = SleepyPenguin::Epoll::IO.new(create_flags)
    @mtx = Mutex.new
    @events = []
    @marks = []
    @pid = $$
    @create_flags = create_flags
    @copies = { @io => self }
  end

  def __ep_reinit # :nodoc:
    @events.clear
    @marks.clear
    @io = SleepyPenguin::Epoll::IO.new(@create_flags)
  end

  # auto-reinitialize the Epoll object after forking
  def __ep_check # :nodoc:
    return if @pid == $$
    return if @io.closed?
    objects = @copies.values
    @copies.each_key { |epio| epio.close }
    @copies.clear
    __ep_reinit
    objects.each do |obj|
      io_dup = @io.dup
      @copies[io_dup] = obj
    end
    @pid = $$
  end

  # Epoll objects may be watched by IO.select and similar methods
  def to_io
    @mtx.synchronize do
      __ep_check
      @io
    end
  end

  # Calls epoll_wait(2) and yields Integer +events+ and IO objects watched
  # for.  +maxevents+ is the maximum number of events to process at once,
  # lower numbers may prevent starvation when used by epoll_wait in multiple
  # threads.  Larger +maxevents+ reduces syscall overhead for
  # single-threaded applications. +maxevents+ defaults to 64 events.
  # +timeout+ is specified in milliseconds, +nil+
  # (the default) meaning it will block and wait indefinitely.
  def wait(maxevents = 64, timeout = nil)
    # snapshot the marks so we do can sit this thread on epoll_wait while other
    # threads may call epoll_ctl.  People say RCU is a poor man's GC, but our
    # (ab)use of GC here is inspired by RCU...
    snapshot = @mtx.synchronize do
      __ep_check
      @marks.dup
    end

    # we keep a snapshot of @marks around in case another thread closes
    # the IO while it is being transferred to userspace.  We release mtx
    # so another thread may add events to us while we're sleeping.
    @io.epoll_wait(maxevents, timeout) { |events, io| yield(events, io) }
  ensure
    # hopefully Ruby does not optimize this array away...
    snapshot[0]
  end

  # Starts watching a given +io+ object with +events+ which may be an Integer
  # bitmask or Array representing arrays to watch for.
  def add(io, events)
    fd = io.to_io.fileno
    events = __event_flags(events)
    @mtx.synchronize do
      __ep_check
      @io.epoll_ctl(CTL_ADD, io, events)
      @events[fd] = events
      @marks[fd] = io
    end
    0
  end

  # call-seq:
  #     ep.del(io) -> 0
  #
  # Disables an IO object from being watched.
  def del(io)
    fd = io.to_io.fileno
    @mtx.synchronize do
      __ep_check
      @io.epoll_ctl(CTL_DEL, io, 0)
      @events[fd] = @marks[fd] = nil
    end
    0
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
    fd = io.to_io.fileno
    @mtx.synchronize do
      __ep_check
      cur_io = @marks[fd]
      return if nil == cur_io || cur_io.to_io.closed?
      @io.epoll_ctl(CTL_DEL, io, 0)
      @events[fd] = @marks[fd] = nil
    end
    io
  rescue Errno::ENOENT, Errno::EBADF
  end

  # call-seq:
  #     epoll.mod(io, flags) -> 0
  #
  # Changes the watch for an existing IO object based on +events+.
  # Returns zero on success, will raise SystemError on failure.
  def mod(io, events)
    events = __event_flags(events)
    fd = io.to_io.fileno
    @mtx.synchronize do
      __ep_check
      @io.epoll_ctl(CTL_MOD, io, events)
      @marks[fd] = io # may be a different object with same fd/file
      @events[fd] = events
    end
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
    fd = io.to_io.fileno
    @mtx.synchronize do
      __ep_check
      cur_io = @marks[fd]
      if cur_io == io
        cur_events = @events[fd]
        return 0 if (cur_events & ONESHOT) == 0 && cur_events == events
        begin
          @io.epoll_ctl(CTL_MOD, io, events)
        rescue Errno::ENOENT
          warn "epoll event cache failed (mod -> add)"
          @io.epoll_ctl(CTL_ADD, io, events)
          @marks[fd] = io
        end
      else
        begin
          @io.epoll_ctl(CTL_ADD, io, events)
        rescue Errno::EEXIST
          warn "epoll event cache failed (add -> mod)"
          @io.epoll_ctl(CTL_MOD, io, events)
        end
        @marks[fd] = io
      end
      @events[fd] = events
    end
    0
  end

  # call-seq:
  #     ep.close -> nil
  #
  # Closes an existing Epoll object and returns memory back to the kernel.
  # Raises IOError if object is already closed.
  def close
    @mtx.synchronize do
      @copies.delete(@io)
      @io.close
    end
  end

  # call-seq:
  #     ep.closed? -> true or false
  #
  # Returns whether or not an Epoll object is closed.
  def closed?
    @mtx.synchronize do
      @io.closed?
    end
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
    fd = __fileno(io)
    @mtx.synchronize do
      __ep_check
      @marks[fd]
    end
  end

  # call-seq:
  #     epoll.events_for(io) -> Integer
  #
  # Returns the events currently watched for in current Epoll object.
  # Mostly used for debugging.
  def events_for(io)
    fd = __fileno(io)
    @mtx.synchronize do
      __ep_check
      @events[fd]
    end
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
    fd = __fileno(io)
    @mtx.synchronize do
      __ep_check
      @marks[fd] ? true : false
    end
  end

  def initialize_copy(src) # :nodoc:
    @mtx.synchronize do
      __ep_check
      rv = super
      unless @io.closed?
        @io = @io.dup
        @copies[@io] = self
      end
      rv
    end
  end
end
