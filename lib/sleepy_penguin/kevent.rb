class SleepyPenguin::Kevent < Struct.new(:ident, :filter, :flags,
                                         :fflags, :data, :udata)
end
