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

  def test_gets
    @sfd = SignalFD.new(%w(USR1), 0)
    pid = fork { sleep 0.01; Process.kill(:USR1, Process.ppid) }
    siginfo = @sfd.gets
    assert_equal Signal.list["USR1"], siginfo.signo
    assert_equal pid, siginfo.pid
    assert Process.waitpid2(pid)[1].success?
  end
end if RUBY_VERSION =~ %r{\A1\.9} && defined?(SleepyPenguin::SignalFD)
