# :stopdoc:
class SleepyPenguin::SignalFD::SigInfo

  def to_hash
    Hash[*MEMBERS.inject([]) { |ary,k| ary << k << __send__(k) }]
  end

  def hash
    to_hash.hash
  end

  def inspect
    "#<#{self.class}:#{to_hash.inspect}>"
  end

  def ==(other)
    other.kind_of?(self.class) && to_hash == other.to_hash
  end
end
# :startdoc:
