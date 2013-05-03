# -*- encoding: binary -*-
require 'sleepy_penguin_ext'

# We need to serialize Inotify#take for Rubinius since that has no GVL
# to protect the internal array
if defined?(SleepyPenguin::Inotify) &&
   defined?(Rubinius) && Rubinius.respond_to?(:synchronize)
  class SleepyPenguin::Inotify
    # :stopdoc:
    alias __take take
    undef_method :take
    def take(*args)
      Rubinius.synchronize(@inotify_tmp) { __take(*args) }
    end
    # :startdoc
  end
end
