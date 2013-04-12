# -*- encoding: binary -*-
module SleepyPenguin

  # the version of sleepy_penguin, currently 3.1.0
  SLEEPY_PENGUIN_VERSION = '3.1.0'
end
require 'sleepy_penguin_ext'
require 'sleepy_penguin/epoll'

# :stopdoc:
#
# We need to serialize Inotify#take for Rubinius since that has no GVL
# to protect the internal array
if defined?(SleepyPenguin::Inotify) &&
   defined?(Rubinius) && Rubinius.respond_to?(:synchronize)
  class SleepyPenguin::Inotify
    alias __take take
    undef_method :take
    def take(*args)
      Rubinius.synchronize(@inotify_tmp) { __take(*args) }
    end
  end
end
