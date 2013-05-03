# This class represents a "struct kevent" structure for Ruby.
# This may be passed to Kqueue::IO#kevent as either a single element
# or as an element inside an array to inject changes into the kevent
# list.
class SleepyPenguin::Kevent < Struct.new(:ident, :filter, :flags,
                                         :fflags, :data, :udata)
end
