require 'test/unit'
require 'fcntl'
require 'socket'
require 'thread'
$-w = true
Thread.abort_on_exception = true
require 'sleepy_penguin'

class TestEpollIO < Test::Unit::TestCase
  include SleepyPenguin

  def setup
    @rd, @wr = IO.pipe
    @epio = Epoll::IO.new(nil)
  end

  def test_add_wait
    @epio.epoll_ctl(Epoll::CTL_ADD, @wr, Epoll::OUT)
    ev = []
    @epio.epoll_wait { |events, obj| ev << [ events, obj ] }
    assert_equal([[Epoll::OUT, @wr]], ev)
  end

  class EpSub < Epoll::IO
    def self.new
      super(SleepyPenguin::Epoll::CLOEXEC)
    end
  end

  def test_subclass
    tmp = EpSub.new
    assert_equal Fcntl::FD_CLOEXEC, tmp.fcntl(Fcntl::F_GETFD)
    assert_nil tmp.close
  end
end if defined?(SleepyPenguin::Epoll)
