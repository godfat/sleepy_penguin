require 'thread'

# The high-level Kqueue interface.  This provides fork-safety under Ruby 1.9
# and later (but not Ruby 1.8).
# This also provides memory protection from bugs due to not storing an
# external reference to an object, but still requires the user to store
# their own object references.
# Events registered to a Kqueue object cannot be shared across fork
# due to the underlying implementation of kqueue in *BSDs.
class SleepyPenguin::Kqueue
  # Kqueue objects may be watched by IO.select and similar methods
  attr_reader :to_io

  def initialize
    @to_io = SleepyPenguin::Kqueue::IO.new
    @mtx = Mutex.new
    @pid = $$
    @copies = { @to_io => self }
  end

  def __kq_reinit # :nodoc:
    @to_io = SleepyPenguin::Kqueue::IO.new
  end

  def __kq_check # :nodoc:
    return if @pid == $$ || @to_io.closed?
    unless @to_io.respond_to?(:autoclose=)
      raise RuntimeError,
       "Kqueue is not safe to use without IO#autoclose=, upgrade to Ruby 1.9+"
    end

    # kqueue has (strange) close-on-fork behavior
    objects = @copies.values
    @copies.each_key { |kqio| kqio.autoclose = false }
    @copies.clear
    __kq_reinit
    objects.each do |obj|
      io_dup = @to_io.dup
      @copies[io_dup] = obj
    end
    @pid = $$
  end

  # Users are responsible for ensuring udata objects remain visible to the
  # Ruby GC.
  def kevent(changelist = nil, *args)
    @mtx.synchronize { __kq_check }
    if changelist
      changelist = [ changelist ] if Struct === changelist

      # store the object_id instead of the raw VALUE itself in kqueue and
      # use _id2ref to safely recover the object without the possibility of
      # invalid memory acccess.
      #
      # We may still raise and drop events due to user error
      changelist = changelist.map do |item|
        item = item.dup
        item[5] = item[5].object_id
        item
      end
    end

    if block_given?
      n = @to_io.kevent(changelist, *args) do |ident,filter,flags,
                                               fflags,data,udata|
        # This may raise and cause events to be lost,
        # that's the users' fault/problem
        udata = ObjectSpace._id2ref(udata)
        yield SleepyPenguin::Kevent.new(ident, filter, flags,
                                        fflags, data, udata)
      end
    else
      n = @to_io.kevent(changelist, *args)
    end
  end

  def initialize_copy(src) # :nodoc:
    @mtx.synchronize do
      __kq_check
      rv = super
      unless @to_io.closed?
        @to_io = @to_io.dup
        @copies[@to_io] = self
      end
      rv
    end
  end

  # call-seq:
  #     kq.close -> nil
  #
  # Closes an existing Kqueue object and returns memory back to the kernel.
  # Raises IOError if object is already closed.
  def close
    @mtx.synchronize do
      @copies.delete(@to_io)
      @to_io.close
    end
  end

  # call-seq:
  #     kq.closed? -> true or false
  #
  # Returns whether or not an Kqueue object is closed.
  def closed?
    @mtx.synchronize do
      @to_io.closed?
    end
  end
end
