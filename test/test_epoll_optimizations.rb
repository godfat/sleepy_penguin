require 'test/unit'
begin
  require 'strace'
rescue LoadError
end
$-w = true

require 'sleepy_penguin'

class TestEpollOptimizations < Test::Unit::TestCase
  include SleepyPenguin
  RBX = defined?(RUBY_ENGINE) && (RUBY_ENGINE == 'rbx')
  IO_PURGATORY = []

  def setup
    @rd, @wr = IO.pipe
    @ep = Epoll.new
  end

  def teardown
    [ @ep, @rd, @wr ].each { |io| io.close unless io.closed? }
  end

  def test_set
    io, err = Strace.me do
      @ep.set(@wr, Epoll::OUT)
      @ep.set(@wr, Epoll::OUT)
    end
    assert_nil err
    lines = io.readlines; io.close
    assert_equal 1, lines.grep(/^epoll_ctl/).size
    assert_match(/EPOLL_CTL_ADD/, lines.grep(/^epoll_ctl/)[0])

    io, err = Strace.me { @ep.set(@wr, Epoll::OUT | Epoll::ONESHOT) }
    assert_nil err
    lines = io.readlines; io.close
    assert_equal 1, lines.grep(/^epoll_ctl/).size
    assert_match(/EPOLL_CTL_MOD/, lines.grep(/^epoll_ctl/)[0])

    io, err = Strace.me { @ep.set(@wr, Epoll::OUT) }
    assert_nil err
    lines = io.readlines; io.close
    assert_equal 1, lines.grep(/^epoll_ctl/).size
    assert_match(/EPOLL_CTL_MOD/, lines.grep(/^epoll_ctl/)[0])
    @wr.close
    @rd.close

    @rd, @wr = IO.pipe
    io, err = Strace.me { @ep.set(@wr, Epoll::OUT) }
    assert_nil err
    lines = io.readlines; io.close
    assert_equal 1, lines.grep(/^epoll_ctl/).size
    assert_match(/EPOLL_CTL_ADD/, lines.grep(/^epoll_ctl/)[0])
  end

  def test_delete
    @ep.set(@wr, Epoll::OUT)
    rv = true
    io, err = Strace.me { rv = @ep.delete(@wr) }
    assert_equal @wr, rv
    assert_nil err
    lines = io.readlines; io.close
    assert_equal 1, lines.grep(/^epoll_ctl/).size
    assert_match(%r{=\s+0$}, lines.grep(/^epoll_ctl/)[0])

    rv = true
    io, err = Strace.me { rv = @ep.delete(@wr) }
    assert_nil rv
    assert_nil err
    lines = io.readlines; io.close
    assert_equal 0, lines.grep(/^epoll_ctl/).size
  end

  def test_delete_closed
    a = @wr.fileno
    @ep.set(@wr, Epoll::OUT)
    @rd.close
    @wr.close
    @rd, @wr = IO.pipe
    assert_equal a, @wr.fileno
    rv = true
    io, err = Strace.me { rv = @ep.delete(@wr) }
    lines = io.readlines; io.close
    assert_nil err
    assert_nil rv
    assert_equal 0, lines.grep(/^epoll_ctl/).size
  end

  def test_delete_closed_proxy
    obj = Struct.new(:to_io).new(@wr)
    rv = nil
    @ep.add(obj, Epoll::OUT)
    @wr.close
    io, err = Strace.me { rv = @ep.delete(obj) }
    lines = io.readlines; io.close
    assert_kind_of IOError, err
    assert_nil rv
    assert_equal 0, lines.grep(/^epoll_ctl/).size
  end

  def test_delete_aliased_a
    tmp = IO.for_fd @wr.fileno
    IO_PURGATORY << tmp
    @ep.set(tmp, Epoll::OUT)
    rv = nil
    io, err = Strace.me { rv = @ep.delete(@wr) }
    lines = io.readlines; io.close
    assert_equal @wr, rv
    assert_nil err
    assert_equal 1, lines.grep(/^epoll_ctl/).size
    assert_match %r{=\s+0$}, lines.grep(/^epoll_ctl/)[0]
    assert_equal 0, lines.grep(/ENOENT/).size
  end

  def test_delete_aliased_b
    tmp = IO.for_fd @wr.fileno
    IO_PURGATORY << tmp
    @ep.set(@wr, Epoll::OUT)
    rv = nil
    io, err = Strace.me { rv = @ep.delete(tmp) }
    lines = io.readlines; io.close
    assert_equal tmp, rv
    assert_nil err
    assert_equal 1, lines.grep(/^epoll_ctl/).size
    assert_match(%r{=\s+0$}, lines.grep(/^epoll_ctl/)[0])
    assert_equal 0, lines.grep(/ENOENT/).size
  end

  def test_delete_aliased_closed
    tmp = IO.for_fd @wr.fileno
    IO_PURGATORY << tmp
    @ep.set(tmp, Epoll::OUT)
    @wr.close
    rv = nil
    io, err = Strace.me { rv = @ep.delete(tmp) }
    lines = io.readlines; io.close
    assert_nil rv
    assert_nil err
    assert_equal 1, lines.grep(/^epoll_ctl/).size
    assert_equal 1, lines.grep(/EBADF/).size
  end
end if defined?(Strace)
