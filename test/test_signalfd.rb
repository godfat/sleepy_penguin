require 'test/unit'
require "dl"
begin
  require "dl/func"
rescue LoadError
end
$-w = true
require 'sleepy_penguin'

class TestSignalFD < Test::Unit::TestCase
  include SleepyPenguin

  def setup
    @sfd = nil
    trap(:USR1, "IGNORE")
    trap(:USR2, "IGNORE")
  end

  def teardown
    @sfd.close if @sfd && ! @sfd.closed?
    trap(:USR1, "DEFAULT")
    trap(:USR2, "DEFAULT")
  end

  def test_rt_constants
    assert [33,34].include?(SignalFD::RTMIN)
    assert_equal 64, SignalFD::RTMAX
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
  end if RUBY_VERSION =~ %r{\A1\.9}

  def test_take_nonblock
    @sfd = SignalFD.new(%w(USR1), :NONBLOCK)
    assert_nil @sfd.take(true)
    assert_nil IO.select [ @sfd ], nil, nil, 0
    pid = fork { sleep 0.01; Process.kill(:USR1, Process.ppid) }
    siginfo = @sfd.take(true)
    if siginfo
      assert_equal Signal.list["USR1"], siginfo.signo
      assert_equal pid, siginfo.pid
    else
      warn "WARNING: SignalFD#take(nonblock=true) broken"
    end
    assert Process.waitpid2(pid)[1].success?
  end if RUBY_VERSION =~ %r{\A1\.9}

  def test_update
    assert_nothing_raised do
      @sfd = SignalFD.new
      @sfd.update! Signal.list["USR1"]
      @sfd.update! [ "USR1", Signal.list["USR2"] ]
      @sfd.update! [ "USR1", :USR2 ]
      @sfd.update! [ Signal.list["USR1"], Signal.list["USR2"] ]
    end
  end

  def test_with_sigqueue
    sig = Signal.list["USR2"]
    @sfd = SignalFD.new(:USR2)
    libc = DL::Handle.new("libc.so.6")
    if defined?(DL::Function)
      sigqueue = libc["sigqueue"]
      sigqueue = DL::CFunc.new(sigqueue, DL::TYPE_INT, "sigqueue")
      args = [DL::TYPE_INT, DL::TYPE_INT,DL::TYPE_INT]
      sigqueue = DL::Function.new(sigqueue, args, DL::TYPE_INT)
    else
      sigqueue = libc["sigqueue", "IIII"]
    end
    pid = fork { sleep 0.01; sigqueue.call(Process.ppid, sig, 666)  }
    siginfo = @sfd.take
    assert_equal sig, siginfo.signo
    assert_equal pid, siginfo.pid
    assert Process.waitpid2(pid)[1].success?
    assert_equal 666, siginfo.ptr
    assert_equal 666, siginfo.int
  end if RUBY_VERSION =~ %r{\A1\.9}
end if defined?(SleepyPenguin::SignalFD)
