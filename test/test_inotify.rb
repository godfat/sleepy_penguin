require 'test/unit'
require 'fcntl'
require 'tempfile'
$-w = true
require 'sleepy_penguin'

class TestInotify < Test::Unit::TestCase
  include SleepyPenguin

  def test_new
    ino = Inotify.new
    assert_kind_of(IO, ino)
  end

  def test_new_nonblock
    ino = Inotify.new Inotify::NONBLOCK
    flags = ino.fcntl(Fcntl::F_GETFL) & Fcntl::O_NONBLOCK
    assert_equal(Fcntl::O_NONBLOCK, flags)
  end

  def test_new_cloeexec
    ino = Inotify.new Inotify::CLOEXEC
    flags = ino.fcntl(Fcntl::F_GETFD) & Fcntl::FD_CLOEXEC
    assert_equal(Fcntl::FD_CLOEXEC, flags)
  end

  def test_add_take
    ino = Inotify.new Inotify::CLOEXEC
    tmp1 = Tempfile.new 'take'
    tmp2 = Tempfile.new 'take'
    wd = ino.add_watch File.dirname(tmp1.path), Inotify::MOVE
    assert_kind_of Integer, wd
    File.rename tmp1.path, tmp2.path
    event = ino.take
    assert_equal wd, event.wd
    assert_kind_of Inotify::Event, event
    assert_equal File.basename(tmp1.path), event.name
    others = ino.instance_variable_get(:@inotify_tmp)
    assert_kind_of Array, others
    assert_equal 1, others.size
    assert_equal File.basename(tmp2.path), others[0].name
    assert_equal [ :MOVED_FROM ], event.events
    assert_equal [ :MOVED_TO ], others[0].events
    assert_equal wd, others[0].wd
    second_id = others[0].object_id
    assert_equal second_id, ino.take.object_id
    assert_nil ino.take(true)
  end
end
