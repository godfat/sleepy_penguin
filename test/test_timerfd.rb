require 'test/unit'
require 'fcntl'
$-w = true

require 'sleepy_penguin'

class TestTimerFD < Test::Unit::TestCase
  include SleepyPenguin

  def test_constants
    assert_kind_of Integer, TimerFD::REALTIME
    assert_kind_of Integer, TimerFD::MONOTONIC
  end

  def test_create
    tfd = TimerFD.new
    assert_kind_of(IO, tfd)
    if RUBY_VERSION.to_f >= 2.0
      assert_equal 1, tfd.fcntl(Fcntl::F_GETFD)
    else
      assert_equal 0, tfd.fcntl(Fcntl::F_GETFD)
    end
  end

  def test_create_nonblock
    tfd = TimerFD.new(TimerFD::REALTIME, TimerFD::NONBLOCK)
    flags = tfd.fcntl(Fcntl::F_GETFL) & Fcntl::O_NONBLOCK
    assert_equal(Fcntl::O_NONBLOCK, flags)
  end if defined?(TimerFD::NONBLOCK)

  def test_create_nonblock_sym
    tfd = TimerFD.new(:REALTIME, :NONBLOCK)
    flags = tfd.fcntl(Fcntl::F_GETFL) & Fcntl::O_NONBLOCK
    assert_equal(Fcntl::O_NONBLOCK, flags)
  end if defined?(TimerFD::NONBLOCK)

  def test_create_cloexec
    tfd = TimerFD.new(TimerFD::REALTIME, TimerFD::CLOEXEC)
    flags = tfd.fcntl(Fcntl::F_GETFD) & Fcntl::FD_CLOEXEC
    assert_equal(Fcntl::FD_CLOEXEC, flags)
  end if defined?(TimerFD::CLOEXEC)

  def test_settime
    tfd = TimerFD.new(TimerFD::REALTIME)
    assert_equal([0, 0], tfd.settime(TimerFD::ABSTIME, 0, 0.01))
    sleep 0.01
    assert_equal 1, tfd.expirations
  end

  def test_settime_symbol
    tfd = TimerFD.new(:REALTIME)
    assert_equal([0, 0], tfd.settime(:ABSTIME, 0, 0.01))
    sleep 0.01
    assert_equal 1, tfd.expirations
  end

  def test_gettime
    tfd = TimerFD.new :REALTIME
    now = Time.now.to_i
    assert_equal([0, 0], tfd.settime(nil, 0, now + 5))
    interval, value = tfd.gettime
    assert_equal 0, interval
    assert_in_delta now + 5, value, 0.01
  end

  def test_expirations_nonblock
    tfd = TimerFD.new(:MONOTONIC)
    assert_equal([0, 0], tfd.settime(0, 0, 0.05))
    assert_nil tfd.expirations(true)
    sleep 0.05
    assert_equal 1, tfd.expirations
  end
end if defined?(SleepyPenguin::TimerFD)
