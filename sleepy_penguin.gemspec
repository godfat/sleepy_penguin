ENV["VERSION"] or abort "VERSION= must be specified"
manifest = File.readlines('.manifest').map! { |x| x.chomp! }
summary = File.readlines("README")[0].gsub(/\A=\s+\S+[^\w]+/, '').strip
description = File.read("README").split(/\n\n/)[1].strip

Gem::Specification.new do |s|
  s.name = %q{sleepy_penguin}
  s.version = ENV["VERSION"]

  s.homepage = 'http://bogomips.org/sleepy_penguin/'
  s.authors = ["sleepy_penguin hackers"]
  s.date = Time.now.utc.strftime('%Y-%m-%d')
  s.description = description
  s.email = %q{sleepy.penguin@librelist.com}

  s.extra_rdoc_files = File.readlines('.document').map! do |x|
    x.chomp!
    if File.directory?(x)
      manifest.grep(%r{\A#{x}/})
    elsif File.file?(x)
      x
    else
      nil
    end
  end.flatten.compact

  s.files = manifest
  s.rdoc_options = [ "-t", summary ]
  s.require_paths = %w(lib ext)
  s.rubyforge_project = %q{rainbows}
  s.summary = summary
  s.test_files = Dir['test/test_*.rb']
  s.extensions = %w(ext/sleepy_penguin/extconf.rb)

  # s.license = %w(LGPL) # disabled for compatibility with older RubyGems
end
