require 'test/unit'
$-w = true
require 'sleepy_penguin'

class TestSignalFDSigInfo < Test::Unit::TestCase
  include SleepyPenguin

  def test_members
    members = SignalFD::SigInfo::MEMBERS
    assert_equal 16, members.size
    a = SignalFD::SigInfo.new
    members.each { |k| assert_equal 0, a.__send__(k) }
  end

  def test_equality
    a = SignalFD::SigInfo.new
    b = SignalFD::SigInfo.new
    assert_equal a, b

    c = Class.new(SignalFD::SigInfo).new
    assert_equal a, c
    assert c != c.to_hash
  end

  def test_to_hash
    hash = SignalFD::SigInfo.new.to_hash
    assert_instance_of Hash, hash
    members = SignalFD::SigInfo::MEMBERS
    assert_equal members.size, hash.size
    members.each { |k| assert_equal 0, hash[k] }
  end
end if defined?(SleepyPenguin::SignalFD)
