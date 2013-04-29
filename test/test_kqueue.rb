require 'test/unit'
$-w = true
Thread.abort_on_exception = true
require 'sleepy_penguin'

class TestKqueue < Test::Unit::TestCase
  include SleepyPenguin

  def test_kqueue
    kq = Kqueue.new
    assert_kind_of IO, kq.to_io
    rd, wr = IO.pipe
    ev = Kevent[rd.fileno, EvFilt::READ, Ev::ADD|Ev::ONESHOT, 0, 0, rd]
    thr = Thread.new do
      kq.kevent(ev)
      wr.syswrite "."
    end

    events = []
    n = kq.kevent(nil, 1) do |kevent|
      assert_kind_of Kevent, kevent
      events << kevent
    end
    assert_equal 1, events.size
    assert_equal rd.fileno, events[0][0]
    assert_equal EvFilt::READ, events[0][1]
    assert_equal 1, n

    # we should be drained
    events = []
    n = kq.kevent(nil, 1, 0) do |kevent|
      assert_kind_of Kevent, kevent
      events << kevent
    end
    assert_equal 0, events.size
    assert_equal 0, n
    thr.join

    # synchronous add
    events = []
    ev = Kevent[wr.fileno, EvFilt::WRITE, Ev::ADD|Ev::ONESHOT, 0, 0, wr]
    kq.kevent(ev)
    n = kq.kevent(nil, 1, 0) do |kevent|
      assert_kind_of Kevent, kevent
      events << kevent
    end
    assert_equal 1, events.size
    assert_equal wr.fileno, events[0][0]
    assert_equal EvFilt::WRITE, events[0][1]
    assert_equal 1, n
  ensure
    kq.close
    rd.close if rd
    wr.close if wr
  end

  def test_usable_after_fork
    kq = Kqueue.new
    pid = fork do
      begin
        ok = false
        assert_equal(0, kq.kevent(nil, 1, 0.1) { exit!(false) })
        ok = true
      ensure
        exit!(ok)
      end
    end
    assert_equal(0, kq.kevent(nil, 1, 0.1) { exit!(false) })
    _, status = Process.waitpid2(pid)
    assert status.success?, status.inspect
  ensure
    kq.close
  end
end if defined?(SleepyPenguin::Kqueue)
