require 'test/unit'
$-w = true

require 'sleepy_penguin'

class TestEpollGC < Test::Unit::TestCase
  include SleepyPenguin

  def setup
    GC.stress = true if GC.respond_to?(:stress=)
    @ep = Epoll.new
  end

  def teardown
    GC.stress = false if GC.respond_to?(:stress=)
  end

  def add_pipe_no_tailcall(m, depth)
    add_pipe(m, depth += 1)
  end

  def add_pipe(m, depth = 0)
    if depth > 6000
      _, wr = IO.pipe
      warn "wr: #{wr.fileno}"
      @ep.__send__(m, wr, Epoll::OUT)
    else
      add_pipe_no_tailcall(m, depth + 1)
    end
  end

  def test_gc_safety
    done = false
    begin
      if done
        x = nil
        @ep.wait(nil, 10) { |flags, obj| p [  flags, x = obj ] }
        assert x, "#{x.inspect}"
        break
      else
        add_pipe(:add)
        2048.times { IO.pipe; File.open(__FILE__)}
        done = true
      end
    rescue Errno::EMFILE, Errno::ENFILE
      Thread.new { GC.start }.join
    end while true
  end
end if ENV["GC_STRESS"].to_i != 0
