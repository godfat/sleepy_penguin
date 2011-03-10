require 'test/unit'
require 'fcntl'
$-w = true

require 'sleepy_penguin'

class TestEventFD < Test::Unit::TestCase
  include SleepyPenguin

  def test_constants
    defined?(EventFD::NONBLOCK) and
      assert_kind_of Integer, EventFD::NONBLOCK
    defined?(EventFD::CLOEXEC) and
      assert_kind_of Integer, EventFD::CLOEXEC
    defined?(EventFD::SEMAPHORE) and
      assert_kind_of Integer, EventFD::SEMAPHORE
    assert_equal 0xfffffffffffffffe, EventFD::MAX
  end

  def test_new
    efd = EventFD.new 0
    assert_kind_of(IO, efd)
  end

  def test_new_nonblock
    efd = EventFD.new(0, EventFD::NONBLOCK)
    flags = efd.fcntl(Fcntl::F_GETFL) & Fcntl::O_NONBLOCK
    assert_equal(Fcntl::O_NONBLOCK, flags)
  end if defined?(EventFD::NONBLOCK)

  def test_new_nonblock_cloexec_sym
    efd = EventFD.new(0, [:NONBLOCK,:CLOEXEC])
    flags = efd.fcntl(Fcntl::F_GETFL) & Fcntl::O_NONBLOCK
    assert_equal(Fcntl::O_NONBLOCK, flags)

    flags = efd.fcntl(Fcntl::F_GETFD) & Fcntl::FD_CLOEXEC
    assert_equal(Fcntl::FD_CLOEXEC, flags)
  end if defined?(EventFD::NONBLOCK) && defined?(EventFD::CLOEXEC)

  def test_new_cloexec
    efd = EventFD.new(0, EventFD::CLOEXEC)
    flags = efd.fcntl(Fcntl::F_GETFD) & Fcntl::FD_CLOEXEC
    assert_equal(Fcntl::FD_CLOEXEC, flags)
  end if defined?(EventFD::CLOEXEC)

  def test_incr_value
    efd = EventFD.new(0)
    assert_equal true, efd.incr(1)
    assert_equal 1, efd.value

    assert_nil efd.value(true)
    assert_equal true, efd.incr(9)
    assert_equal 9, efd.value(true)

    assert_equal true, efd.incr(0xfffffffffffffffe)
    assert_equal false, efd.incr(1, true)
  end

  def test_incr_value_semaphore
    efd = EventFD.new(6, :SEMAPHORE)
    6.times { assert_equal 1, efd.value }
    assert_nil efd.value(true)
    assert_equal true, efd.incr(1)
    assert_equal 1, efd.value
  end
end if defined?(SleepyPenguin::EventFD)
