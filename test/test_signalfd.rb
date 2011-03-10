require 'test/unit'
$-w = true
require 'sleepy_penguin'

class TestSignalFD < Test::Unit::TestCase
  include SleepyPenguin

  def setup
    @sfd = nil
  end

  def teardown
    @sfd.close if @sfd && ! @sfd.closed?
  end

  def test_new_with_flags
    @sfd = SignalFD.new(%w(USR1), [:CLOEXEC,:NONBLOCK])
    assert_instance_of SignalFD, @sfd
  end if defined?(SignalFD::CLOEXEC) && defined?(SignalFD::NONBLOCK)

  def test_new_with_sym_flag
    @sfd = SignalFD.new(%w(USR1), :CLOEXEC)
    assert_instance_of SignalFD, @sfd
  end if defined?(SignalFD::CLOEXEC)

  def test_take
    @sfd = SignalFD.new(%w(USR1), 0)
    pid = fork { sleep 0.01; Process.kill(:USR1, Process.ppid) }
    siginfo = @sfd.take
    assert_equal Signal.list["USR1"], siginfo.signo
    assert_equal pid, siginfo.pid
    assert Process.waitpid2(pid)[1].success?
  end

  def test_update
    assert_nothing_raised do
      @sfd = SignalFD.new
      @sfd.update! Signal.list["USR1"]
      @sfd.update! [ "USR1", Signal.list["USR2"] ]
      @sfd.update! [ "USR1", :USR2 ]
      @sfd.update! [ Signal.list["USR1"], Signal.list["USR2"] ]
    end
  end
end if RUBY_VERSION =~ %r{\A1\.9} && defined?(SleepyPenguin::SignalFD)
